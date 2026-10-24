// SPDX-License-Identifier: GPL-2.0-only
// Буфер, который показывает дисплей. См. glass_capture.hpp.

#include "glass_capture.hpp"
#include "glass_present.hpp"

#include <atomic>

// libglass/deko3d (dk_memblock.cpp): номер nvmap -> свой дескриптор, без запроса kind (на него служба отвечает BadValue).
extern "C" Result ryzNvMapLoadRemote(NvMap* m, uint32_t id);

namespace ryz::glass {
namespace {

constexpr uint32_t MinBytes = 0x10000;
constexpr uint32_t MaxBytes = 0x40000000;   // пулы памяти игр бывают в сотни МБ: свопчейн лежит в них со смещением
constexpr uint32_t BigBytes = 0x4000000;    // больше — закреплять не чаще одного за вызов
constexpr uint32_t IovmmHeap = 0x40000000;
constexpr unsigned IdsPerCall = 32;
constexpr unsigned NeighborSpan = 16;       // соседние номера: буферы одного свопчейна создаются подряд
constexpr unsigned CeilingEvery = 240;      // раз в столько промахов — свежий верх поиска

// Чтение регистра MC через exosphere (smcReadWriteRegister с маской 0 — только чтение).
bool readSmc(uintptr_t address, uint32_t& value) {
    SecmonArgs args{};
    args.X[0] = 0xF0000002;
    args.X[1] = address;
    svcCallSecureMonitor(&args);
    if (args.X[0]) return false;
    value = uint32_t(args.X[1]);
    return true;
}

bool sceneFormat(uint32_t f) { return f == 12 || f == 13 || f == 37 || f == 38; }

}  // namespace

bool DirectCapture::initialize(uint32_t firstOwnId, uint32_t latestOwnId, void* censusStorage, uint32_t overlayWidth,
                               uint32_t overlayHeight, const uint32_t* hints, unsigned hintCount) {
    if (ready_) return true;
    firstOwn_ = firstOwnId;
    hintCount_ = 0;
    for (unsigned i = 0; i < hintCount && hintCount_ < MaxHints; ++i)
        if (hints[i]) hints_[hintCount_++] = hints[i];
    dc_[0] = dc_[1] = nullptr;
    ceiling_ = nextId_ = latestOwnId;
    censusStorage_ = censusStorage;
    overlayWidth_ = overlayWidth;
    overlayHeight_ = overlayHeight;
    if (!ceiling_) return false;
    // Часы (CAR): регистры выключенного контроллера дисплея читать нельзя — шина встанет.
    u64 clockMapping = 0, clockBytes = 0;
    const Result clockResult = svcQueryMemoryMapping(&clockMapping, &clockBytes, 0x60006000, 0x1000);
    if (R_FAILED(clockResult) || !clockMapping || clockBytes < 0x14) {
        glass::log("glass: clock registers not mapped (rc 0x%x)", clockResult);
        return false;
    }
    clocks_ = reinterpret_cast<volatile uint32_t*>(clockMapping);
    uint32_t smmu = 0;
    if (!readSmc(0x70019284, smmu)) {
        glass::log("glass: memory controller not readable");
        return false;
    }
    for (unsigned i = 0; i < 2; ++i) {
        u64 mapped = 0, bytes = 0;
        const Result rc = svcQueryMemoryMapping(&mapped, &bytes, i ? 0x54240000 : 0x54200000, 0x40000);
        if (R_SUCCEEDED(rc) && mapped && bytes >= 0x4800) dc_[i] = reinterpret_cast<volatile uint32_t*>(mapped);
    }
    if (!dc_[0] && !dc_[1]) {
        glass::log("glass: display registers not mapped (ovlloader without the display MapRange?)");
        return false;
    }
    Result rc = nvOpen(&vic_, "/dev/nvhost-vic");
    if (R_SUCCEEDED(rc)) rc = nvioctlChannel_SetNvmapFd(vic_, nvMapGetFd());
    if (R_FAILED(rc)) {
        glass::log("glass: VIC channel failed (rc 0x%x)", rc);
        close();
        return false;
    }
    misses_ = 0;
    ready_ = true;
    return true;
}

// Окна включённого контроллера: сцена — самое заднее полноэкранное окно с настоящим источником (заглушки 2x2,
// растянутые на весь экран, — не сцена), оверлей — окно с источником размера буфера оверлея.
bool DirectCapture::readFrame(DirectFrame& result) {
    uint32_t vicCfg = 0;
    if (!clocks_ || !readSmc(0x70019284, vicCfg) || !(vicCfg & 0x80000000)) return false;
    for (unsigned controller = 0; controller < 2; ++controller) {
        auto* regs = dc_[controller];
        // Выключенный по часам контроллер не трогать; проверка каждый раз — док и сон меняют выбор.
        if (!regs || !(clocks_[4] & (1u << (controller ? 26 : 27)))) continue;
        // Адреса окон сравнимы с адресами закреплений в VIC, только если у дисплея и VIC одно пространство SMMU.
        uint32_t dcCfg = 0;
        if (!readSmc(controller ? 0x70019244 : 0x70019240, dcCfg) || (dcCfg & 0x800000ff) != (vicCfg & 0x800000ff))
            continue;
        if (!(regs[0x032] & 0x60)) continue;
        const uint32_t active = regs[0x409];
        const uint32_t sw = active & 0x1fff, sh = (active >> 16) & 0x1fff;
        if (sw < 320 || sh < 180 || sw > 4096 || sh > 4096) continue;
        DirectFrame frame;
        frame.displayWidth = sw;
        frame.displayHeight = sh;
        // Сначала все окна, потом выбор: у полноэкранного оверлея (HUD 1280x720) источник того же размера, что у кадра
        // игры, — своё окно тогда самое переднее из совпавших по размеру, а сцена — из остальных.
        DcWindow windows[3];
        bool sceneLike[3] = {}, overlayLike[3] = {};
        for (unsigned i = 0; i < 3; ++i) {
            const unsigned control = 0xb80 + i * 0x200, buffer = 0xbc0 + i * 0x200;
            const uint32_t options = regs[control];
            if (!(options & 0x40000000)) continue;
            DcWindow w;
            w.controller = controller;
            w.index = i;
            w.options = options;
            w.format = regs[control + 3];
            const uint32_t position = regs[control + 4], extent = regs[control + 5], pre = regs[control + 6];
            w.x = position & 0x1fff;
            w.y = (position >> 16) & 0x1fff;
            w.extentX = extent & 0x1fff;
            w.extentY = (extent >> 16) & 0x1fff;
            w.srcX = (pre & 0x7fff) / 4;
            w.srcY = (pre >> 16) & 0x1fff;
            w.pitch = regs[control + 10] & 0xffff;
            w.depth = regs[control + 0x16] & 255;
            const uint32_t kind = regs[buffer + 11];
            w.blockHeight = (kind & 3) == 2 ? ((kind >> 4) & 7) + 1 : 0;
            w.address = uint64_t(regs[buffer]) | (uint64_t(regs[buffer + 13]) << 32);
            w.originX = int32_t(regs[buffer + 6] / 4);
            w.originY = int32_t(regs[buffer + 8]);
            // Строка вывода идёт вдоль x буфера, а с scan column — вдоль y. Направления — по осям буфера: H — по x,
            // V — по y (HOME в портативе: scan column, H, начало 1279,0 — строка вывода идёт по y вниз, строки — по x
            // от правого края).
            const int32_t sx = (options & 1) ? -1 : 1, sy = (options & 4) ? -1 : 1;
            if (options & 16) {
                w.stepX[1] = sy;
                w.stepY[0] = sx;
            } else {
                w.stepX[0] = sx;
                w.stepY[1] = sy;
            }
            const bool compressed = regs[buffer + 0x2f] & 1;
            const bool swapped = regs[control + 1] != 0;
            std::atomic_thread_fence(std::memory_order_seq_cst);
            // Окно перепрограммировали посреди чтения — пропустить этот раз.
            if (options != regs[control] || pre != regs[control + 6]) continue;
            windows[i] = w;
            // Окно оверлея: источник — буфер слоя (в любом повороте). Адрес ему не нужен: на листании своих буферов он
            // бывает 0 — без этой проверки окно терялось через раз, и стекло прыгало на запасной поворот.
            const bool overlaySize = (w.srcX == overlayWidth_ && w.srcY == overlayHeight_) ||
                                     (w.srcX == overlayHeight_ && w.srcY == overlayWidth_);
            overlayLike[i] = overlaySize && (w.format == 12 || w.format == 13) && w.extentX && w.extentY;
            if (uint32_t(w.address) != regs[buffer]) continue;
            if (compressed || swapped || ((kind & 3) != 0 && (kind & 3) != 2) || !w.address) continue;
            if (!w.srcX || !w.srcY || !w.extentX || !w.extentY) continue;
            if (!sceneFormat(w.format) || w.srcX < 64 || w.srcY < 64) continue;
            if (w.extentX < sw * 3 / 4 || w.extentY < sh * 3 / 4) continue;
            sceneLike[i] = true;
        }
        int overlay = -1;
        for (unsigned i = 0; i < 3; ++i)
            if (overlayLike[i] && (overlay < 0 || windows[i].depth < windows[overlay].depth)) overlay = int(i);
        if (overlay >= 0) {
            frame.overlay = windows[overlay];
            frame.overlayFound = true;
        }
        // Сцена — самая задняя из остальных (больше depth — дальше от зрителя), при равенстве — больше площадь.
        bool haveScene = false;
        for (unsigned i = 0; i < 3; ++i) {
            if (!sceneLike[i] || int(i) == overlay) continue;
            const DcWindow& w = windows[i];
            if (!haveScene || w.depth > frame.scene.depth ||
                (w.depth == frame.scene.depth && w.extentX * w.extentY > frame.scene.extentX * frame.scene.extentY))
                frame.scene = w;
            haveScene = true;
        }
        if (haveScene) {
            // Окно оверлея не прочиталось в этот раз — прежнее: слой не двигается, пока оверлей на экране.
            if (frame.overlayFound) {
                lastOverlay_ = frame.overlay;
                haveOverlay_ = true;
            } else if (haveOverlay_ && lastOverlay_.controller == controller) {
                frame.overlay = lastOverlay_;
                frame.overlayFound = true;
            }
            result = frame;
            return true;
        }
    }
    return false;
}

void DirectCapture::release(Pin& pin) {
    if (!pin.map.has_init) return;
    const nvioctl_command_buffer_map arg{ pin.map.handle, pin.address };
    nvioctlChannel_UnmapCommandBuffer(vic_, &arg, 1, false);
    nvMapClose(&pin.map);
    pin = {};
}

bool DirectCapture::matchKept(DirectFrame& frame) {
    const uint64_t address = frame.scene.address;
    for (auto& p : kept_) {
        if (!p.map.has_init || address < p.address || address - p.address >= p.map.size) continue;
        DirectFrame f = frame;
        f.id = p.map.id;
        f.allocationBytes = p.map.size;
        f.offset = uint32_t(address - p.address);
        f.base = p.address;
        if (!validDirectFrame(f)) continue;
        frame = f;
        return true;
    }
    return false;
}

// Номер nvmap: открыть, закрепить в канале VIC, сравнить адрес. Совпал — закрепление остаётся (буфер свопчейна
// понадобится снова), иначе снимается сразу: чужие закрепления занимают адресное пространство дисплея.
bool DirectCapture::tryId(uint32_t id, DirectFrame& frame) {
    for (const auto& k : kept_)
        if (k.map.has_init && k.map.id == id) return false;
    const uint64_t address = frame.scene.address;
    for (const auto& s : seen_) {
        if (s.id != id) continue;
        if (address < s.address || address >= uint64_t(s.address) + s.size) return false;
        break;
    }
    Pin p;
    if (R_FAILED(ryzNvMapLoadRemote(&p.map, id))) return false;
    if (p.map.size < MinBytes || p.map.size > MaxBytes || p.map.size < needBytes_) {
        nvMapClose(&p.map);
        return false;
    }
    // Закрепляются только буферы обычной кучи (IOVMM), как у кадров vi и игр: когда закреплялось всё подряд от 64 КБ,
    // nvservices падала (2162-0002) посреди поиска при запущенном приложении. Куча не прочиталась — тоже не трогать.
    u32 heapMask = 0;
    if (R_FAILED(nvioctlNvmap_Param(nvMapGetFd(), p.map.handle, NvMapParam_Heap, &heapMask)) || heapMask != IovmmHeap) {
        nvMapClose(&p.map);
        return false;
    }
    if (p.map.size > BigBytes) {
        if (!bigBudget_) {   // в этот вызов уже был большой — номер вернётся в следующем обходе
            nvMapClose(&p.map);
            return false;
        }
        --bigBudget_;
    }
    nvioctl_command_buffer_map args{ p.map.handle, ~0u };
    const Result rc = nvioctlChannel_MapCommandBuffer(vic_, &args, 1, false);
    if (R_FAILED(rc) || !args.iova || args.iova == ~0u) {
        nvMapClose(&p.map);
        return false;
    }
    p.address = args.iova;
    if (address >= p.address && address < uint64_t(p.address) + p.map.size) {
        auto& slot = kept_[keptNext_++ % MaxKept];
        release(slot);
        slot = p;
        if (!matchKept(frame)) return false;
        lastMatch_ = id;
        neighbor_ = 1;
        return true;
    }
    bool known = false;
    for (auto& s : seen_)
        if (s.id == id) {
            s.address = p.address;
            s.size = p.map.size;
            known = true;
            break;
        }
    if (!known) seen_[seenNext_++ % SeenSlots] = { id, p.address, uint32_t(p.map.size) };
    release(p);
    return false;
}

bool DirectCapture::acquire(DirectFrame& frame) {
    if (!ready_ || !readFrame(frame)) return false;
    if (matchKept(frame)) {
        misses_ = 0;
        return true;
    }
    bigBudget_ = 1;
    needBytes_ = windowBytes(frame.scene);
    // Дисплей листает 2-3 буфера, адрес меняется почти каждый кадр.
    if (frame.scene.address != lastAddress_) {
        lastAddress_ = frame.scene.address;
        // Новый адрес мог попасть в уже проверенный буфер (другой кадр того же пула) — его первым.
        const uint64_t address = frame.scene.address;
        for (const auto& s : seen_)
            if (s.id && address >= s.address && address < uint64_t(s.address) + s.size && tryId(s.id, frame)) {
                misses_ = 0;
                return true;
            }
    }
    if (++misses_ % CeilingEvery == 0) {
        // Свежий верх поиска: номер новой своей метки (номера nvmap общие на всю систему и только растут).
        NvMap marker{};
        if (censusStorage_ && R_SUCCEEDED(nvMapCreate(&marker, censusStorage_, 0x1000, 0x1000, NvKind_Pitch, true))) {
            const uint32_t id = nvMapGetId(&marker);
            if (id > ceiling_) ceiling_ = id;
            nvMapClose(&marker);
        }
    }
    for (unsigned n = 0; n < IdsPerCall; ++n) {
        uint32_t id;
        // Сначала номера, найденные в прошлые запуски (HOME — один и тот же пул с загрузки), потом соседи последнего
        // найденного (+1, -1, +2, -2, ...), потом общий обход сверху вниз по кругу.
        if (hintNext_ < hintCount_) {
            id = hints_[hintNext_++];
        } else if (neighbor_ && neighbor_ <= NeighborSpan * 2) {
            const uint32_t k = (neighbor_ + 1) / 2;
            id = (neighbor_ & 1) ? lastMatch_ + k : (lastMatch_ > k ? lastMatch_ - k : 0);
            ++neighbor_;
            if (!id) continue;
        } else {
            if (!nextId_ || nextId_ > ceiling_) nextId_ = ceiling_;
            id = nextId_--;
            if (firstOwn_ && id >= firstOwn_ && id <= firstOwn_ + 64) continue;   // свои блоки
        }
        if (tryId(id, frame)) {
            misses_ = 0;
            return true;
        }
    }
    return false;
}

bool DirectCapture::refreshAddress(DirectFrame& f) const {
    if (!ready_ || !f.id || f.scene.controller > 1 || f.scene.index > 2) return false;
    auto* regs = dc_[f.scene.controller];
    if (!regs || !clocks_ || !(clocks_[4] & (1u << (f.scene.controller ? 26 : 27)))) return false;
    const unsigned buffer = 0xbc0 + f.scene.index * 0x200;
    const uint64_t address = uint64_t(regs[buffer]) | (uint64_t(regs[buffer + 13]) << 32);
    if (address == f.scene.address) return true;
    // Другой буфер nvmap (свопчейн из отдельных буферов) — остаётся прежний, его найдёт поток.
    if (address < f.base || address - f.base >= f.allocationBytes) return false;
    DirectFrame g = f;
    g.offset = uint32_t(address - f.base);
    g.scene.address = address;
    if (!validDirectFrame(g)) return false;
    f = g;
    return true;
}

void DirectCapture::close() {
    if (vic_ != ~0u) {
        for (auto& p : kept_) release(p);
        nvClose(vic_);
        vic_ = ~0u;
    }
    keptNext_ = 0;
    ready_ = false;
}

}  // namespace ryz::glass
