// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Dimasick-git (RCU)
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

Mutex g_dcReadMutex;

// Регистры контроллера дисплея стекло только читает. Переключать DC_CMD_STATE_ACCESS (READ_MUX, слово 0x40), чтобы
// читать активный банк вместо сборки, нельзя: бит общий с системным драйвером дисплея, и его чтение-правка-запись окна в
// тот миг, когда бит переключён нами, откатывает ждущее обновление окна — слой мигает. Готовый кадр берётся из взведённой
// копии адреса (committedAddress). Чтение двойное (сверяется повторным чтением), а оба наших потока читают по очереди —
// снимок одного окна не перемешивается.
class DcReadGuard {
public:
    DcReadGuard() { mutexLock(&g_dcReadMutex); }
    ~DcReadGuard() { mutexUnlock(&g_dcReadMutex); }
    DcReadGuard(const DcReadGuard&) = delete;
    DcReadGuard& operator=(const DcReadGuard&) = delete;
};

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

// Окно для сверки: тот же слой, пока не сменились режим, формат и размер источника.
uint32_t windowSignature(const DcWindow& w) { return w.options ^ (w.format << 8) ^ (w.srcX << 16) ^ w.srcY; }

// Защёлку запрашивали после того, как кадр дорисован, а взводили раньше — столько раз увидеть, чтобы перестать верить
// новому взведённому адресу без запроса (см. committedAddress).
constexpr uint32_t ArmedEarlyTrust = 3;
// Новый взведённый адрес без запроса защёлки дольше этого — запрос и защёлку проглядели, кадр уже на экране.
constexpr uint64_t MissedLatchNs = 50'000'000;
// Взведённая копия не читается: столько взглядов подряд она не похожа на адрес при живом адресе сборки, или столько
// взглядов она ни разу не совпала со сборкой (сборка и взведённая расходятся лишь между записью адреса и его взводом).
constexpr uint32_t ArmZeroLimit = 32;
constexpr uint32_t ArmMatchLimit = 2400;

}  // namespace

// Взведённая копия адреса окна: START_ADDR — слово buffer, его взведённая копия (START_ADDR_NS) — словом следом,
// buffer+1 (TRM 24.5.4: у регистров с тремя копиями взведённая лежит по адресу на единицу больше). READ_MUX её чтение не
// трогает. Старшие биты (START_ADDR_HI, buffer+13) — из сборки: между кадрами они не меняются. 0 — копию взвели посреди
// чтения или она не читается.
uint64_t DirectCapture::armedAddress(const volatile uint32_t* regs, unsigned buffer, uint64_t assembly) const {
    if (armOff_) return assembly;
    const uint32_t lo = regs[buffer + 1];
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (lo != regs[buffer + 1]) return 0;
    if (!lo || (lo & 0xf)) {   // не адрес буфера: START_ADDR кратен 16
        if (assembly && !armSeen_ && ++armZero_ >= ArmZeroLimit) {
            armOff_ = true;
            glass::log("glass: armed address copy is not an address (0x%x), window address from the assembly copy", lo);
            return assembly;
        }
        return 0;
    }
    armSeen_ = true;
    armZero_ = 0;
    if (!armMatched_) {
        if (lo == uint32_t(assembly)) {
            armMatched_ = true;
        } else if (++armMismatch_ >= ArmMatchLimit) {
            armOff_ = true;
            glass::log("glass: armed address copy never matched the assembly copy (0x%x vs 0x%llx), assembly copy used",
                       lo, (unsigned long long)assembly);
            return assembly;
        }
    }
    return (assembly & ~0xffffffffull) | lo;
}

// Кадр, который окно показывает или покажет следующим, — по взведённой копии адреса, без READ_MUX. У адреса буфера окна
// три копии (TRM 24.5.4): сборка, взведённая и активная. Драйвер дисплея пишет следующий адрес в сборку заранее — кадр в
// нём может ещё рисоваться (на OLED стекло ловило такие кадры из сборки и мерцало), — взводит его (WIN_x_UPDATE) и
// просит защёлку (WIN_x_ACT_REQ: слово 0x41, бит 1 + номер окна), когда кадр дорисован; на ближайшей границе кадра
// взведённое становится активным.
//  - Защёлка запрошена: взведённый кадр готов и выйдет на экран следующим, свободным станет только после ещё одного.
//  - Не запрошена: взведённое уже на экране.
// Бывает, что адрес взводят заранее, а защёлку просят позже. Это видно так: новый взведённый адрес без запроса, при
// следующем взгляде он же с запросом. Увидев это ArmedEarlyTrust раз, новому адресу без запроса не верим: на экране
// прежний подтверждённый, а сменился взведённый адрес ещё раз — прежний взведённый уже на экране (следующий взводят только
// после его защёлки). Новый адрес без запроса дольше MissedLatchNs — защёлку проглядели, он на экране. 0 — готовый кадр
// ещё не известен, этот раз пропустить.
uint64_t DirectCapture::committedAddress(unsigned controller, unsigned index, uint64_t armed, uint64_t assembly,
                                         uint32_t signature, const volatile uint32_t* regs) const {
    Track& t = track_[controller & 1][index % 3];
    if (t.signature != signature) {   // другой слой в этом окне: прежняя история к нему не относится
        t = Track{};
        t.signature = signature;
    }
    const bool requested = regs[0x41] & (1u << (1 + index % 3));
    const bool fresh = armed != t.last;
    if (requested && !fresh && t.lastFreshIdle) ++stats_.armedEarly;
    ++stats_.reads;
    if (requested) ++stats_.requested;
    if (assembly != armed) ++stats_.assemblyAhead;
    uint64_t result;
    if (requested || stats_.armedEarly < ArmedEarlyTrust || (armed == t.confirmed && !fresh)) {
        t.confirmed = armed;
        t.idleSince = 0;
        result = armed;
    } else {
        const uint64_t now = armGetSystemTick();
        if (fresh) {
            if (t.last && t.last != t.confirmed) t.confirmed = t.last;
            t.idleSince = now;
        } else if (!t.idleSince) {
            t.idleSince = now;
        }
        if (armTicksToNs(now - t.idleSince) > MissedLatchNs) {
            t.confirmed = armed;
            t.idleSince = 0;
        }
        result = t.confirmed;
        if (!result) ++stats_.unknown;
        else if (result != armed) ++stats_.held;
    }
    t.last = armed;
    t.lastFreshIdle = fresh && !requested;
    return result;
}

DirectCapture::Stats DirectCapture::stats() const {
    DcReadGuard guard;
    return stats_;
}

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
    glass::log("glass: DC scanout capture, registers read-only, frame from the armed address copy");
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
        DcReadGuard guard;
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
            const uint64_t armed = armedAddress(regs, buffer, w.address);
            if (!armed) continue;
            if (!armLogged_) {
                armLogged_ = true;
                glass::log("glass: scene window %c of DC %u: assembly 0x%llx, armed 0x%llx, latch request %u",
                           char('A' + i), controller, (unsigned long long)w.address, (unsigned long long)armed,
                           unsigned((regs[0x41] >> (1 + i)) & 1));
            }
            windows[i].address = committedAddress(controller, i, armed, w.address, windowSignature(w), regs);
            if (!windows[i].address) continue;
            sceneLike[i] = true;
        }
        int overlay = -1;
        for (unsigned i = 0; i < 3; ++i)
            if (overlayLike[i] && (overlay < 0 || windows[i].depth < windows[overlay].depth)) overlay = int(i);
        if (overlay >= 0) {
            frame.overlay = windows[overlay];
            frame.overlayFound = true;
        }
        // With HOME + another system layer the compositor may flatten our own
        // layer into its output. Without a separate overlay plane that output
        // can contain us; feeding it back into glass creates rapid flickering.
        // Keep the last GPU blur until a separate overlay plane is visible again.
        if (overlay < 0) continue;
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
    watchController_ = frame.scene.controller;
    watchIndex_ = frame.scene.index;
    watchSignature_ = windowSignature(frame.scene);
    watchValid_ = true;
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
    DcReadGuard guard;
    // A DC window index is not a persistent surface identity. HOME can reuse
    // that index for a different plane between capture-thread publications.
    // Validate the whole mapping before following even an unchanged address.
    if (!f.overlayFound || f.overlay.controller != f.scene.controller || f.overlay.index > 2) return false;
    if (!windowMappingMatches(f.scene, regs) || !windowMappingMatches(f.overlay, regs)) return false;
    const unsigned buffer = 0xbc0 + f.scene.index * 0x200;
    const uint64_t assembly = uint64_t(regs[buffer]) | (uint64_t(regs[buffer + 13]) << 32);
    const uint64_t armed = armedAddress(regs, buffer, assembly);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (!armed || !windowMappingMatches(f.scene, regs) || !windowMappingMatches(f.overlay, regs)) return false;
    const uint64_t address =
        committedAddress(f.scene.controller, f.scene.index, armed, assembly, windowSignature(f.scene), regs);
    if (!address) return false;
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

void DirectCapture::watch() const {
    if (!ready_ || !watchValid_ || watchController_ > 1 || watchIndex_ > 2) return;
    auto* regs = dc_[watchController_];
    if (!regs || !clocks_ || !(clocks_[4] & (1u << (watchController_ ? 26 : 27)))) return;
    DcReadGuard guard;
    if (!(regs[0x032] & 0x60)) return;
    const unsigned control = 0xb80 + watchIndex_ * 0x200, buffer = 0xbc0 + watchIndex_ * 0x200;
    DcWindow w;
    w.options = regs[control];
    if (!(w.options & 0x40000000)) return;
    w.format = regs[control + 3];
    const uint32_t pre = regs[control + 6];
    w.srcX = (pre & 0x7fff) / 4;
    w.srcY = (pre >> 16) & 0x1fff;
    if (windowSignature(w) != watchSignature_) return;   // в окне уже другой слой — его найдёт полное чтение
    const uint64_t assembly = uint64_t(regs[buffer]) | (uint64_t(regs[buffer + 13]) << 32);
    const uint64_t armed = armedAddress(regs, buffer, assembly);
    if (armed) committedAddress(watchController_, watchIndex_, armed, assembly, watchSignature_, regs);
}

void DirectCapture::close() {
    watchValid_ = false;
    if (vic_ != ~0u) {
        for (auto& p : kept_) release(p);
        nvClose(vic_);
        vic_ = ~0u;
    }
    keptNext_ = 0;
    ready_ = false;
}

}  // namespace ryz::glass
