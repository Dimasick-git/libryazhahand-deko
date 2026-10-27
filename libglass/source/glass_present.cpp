// SPDX-License-Identifier: GPL-2.0-only
// Стекло: вывод кадра Tesla через видеочип. См. glass_present.hpp.

#include "glass_present.hpp"
#include "glass_backdrop.hpp"
#include "glass_heap.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <switch.h>
#include <deko3d.hpp>

#include <global_vars.hpp>
#include <ini_funcs.hpp>

#include "glass_shaders.hpp"

// libglass/deko3d (dk_memblock.cpp): чужой буфер nvmap по номеру — только для чтения шейдером; номер своего блока.
extern "C" DkMemBlock ryzDkImportNvMapId(DkDevice device, uint32_t id);
extern "C" uint32_t ryzDkMemBlockId(DkMemBlock object);

// libryazha (tsl_utils): буфер слоя этого оверлея — его задаёт оверлей до tsl::loop (Status-Monitor — по режиму).
namespace ult {
extern u16 DefaultFramebufferWidth;
extern u16 DefaultFramebufferHeight;
}

extern "C" u32 __nx_nv_transfermem_size;
extern "C" u32 __nx_nv_service_type;

namespace ryz::glass {
namespace {

constexpr char LogPath[] = "sdmc:/config/ryazhahand/glass.log";
constexpr char OffPath[] = "sdmc:/config/ryazhahand/glass_off.flag";
constexpr u32 TransferMemoryBytes = 0x300000;       // с 512 КБ канал видеочипа не создаётся (0x235C)
constexpr u32 CommandBytes = 0x10000;
// Проба до перехвата окна: ответ видеочипа приходит за доли миллисекунды, секунда — с большим запасом.
constexpr s64 ProbeWaitNs = 1'000'000'000;
// Кадр: перенос интерфейса — доли миллисекунды. Три кадра подряд без ответа (или ошибка очереди) — окно обратно libnx.
constexpr s64 FrameWaitNs = 100'000'000;
constexpr unsigned MaxStalls = 3;
// Окно видеочип перехватывает, только если подготовка была быстрой: вне группы Applet каждое изменение каналов
// видеочипа во время игры ждёт ~11 с — тогда оверлей остаётся на libnx. С ovlloader-апплетом подготовка — 0.05–0.1 с.
constexpr u64 TakeoverMaxPrepareMs = 2000;
constexpr unsigned SwapImages = 2;
// Стекло: фон темы под размытием рисуется с этой альфой (из 15), шейдер кладёт его поверх кадра игры тонировкой:
// фон виден, текст на нём читается.
constexpr u8 GlassBackgroundAlpha = 6;
constexpr float GlassSaturation = 1.3f;            // размытие гасит цвет — вернуть
constexpr float PanelGlassBrightness = 0.35f;      // стекло под подложками и таблицами — темнее, текст не просвечивает
// Стекло: образ захвата (половина панели) и ступени вдвое меньше; размытие — вниз до последней и обратно (dual Kawase).
constexpr unsigned BlurLevels = 4;
// Первый кадр: оверлей не показывается без стекла. Подготовка видеочипа — 0.05–0.1 с, буфер дисплея по подсказке —
// миллисекунды; дольше этих сроков первый кадр не ждёт.
constexpr u64 HoldPrepareNs = 700'000'000;
constexpr u64 HoldBackdropNs = 150'000'000;
constexpr u32 UniformBytes = DK_UNIFORM_BUF_ALIGNMENT;
constexpr u32 CaptureCommandBytes = 0x4000;         // список захвата фона — одна отрисовка, пишется при смене буфера

u64 g_t0 = 0;
u64 g_prepareMs = 0;
u32 g_defaultTransferMemory = 0;
std::atomic<bool> g_enabled{true};   // переключатель «Размытие GPU»: выключено на лету — кадры без стекла
float g_layerRect[4] = { 0.0f, 0.0f, 1.0f / 1280.0f, 1.0f / 720.0f };
Mutex g_logMutex;

u64 nowMs() { return armTicksToNs(armGetSystemTick() - g_t0) / 1'000'000; }

u32 alignUp(u32 v, u32 a) { return (v + a - 1) & ~(a - 1); }

// Образы стекла: захват — половина буфера оверлея, дальше ступени вдвое меньше.
std::size_t blurBytesFor(u32 width, u32 height) { return std::size_t(width / 2) * (height / 2) * 4 * 4 / 3 + 0x40000; }

// Сколько памяти видеочипу нужно под буфер оверлея width x height: transfer memory, буфер интерфейса RGBA4, два образа
// свопчейна RGBA8, стекло и мелочь (очередь, команды, код, дескрипторы). Панель 448x720 — ~7.7 МБ, полноэкранный
// HUD 1280x720 — ~15 МБ. Не хватит — deko3d на нехватке памяти роняет процесс.
std::size_t gpuBytesFor(u32 width, u32 height) {
    const std::size_t ui = std::size_t(alignUp(width * 2, 64)) * alignUp(height, 128);
    const std::size_t swap = std::size_t(alignUp(width * 4, 64)) * alignUp(height, 128) * SwapImages;
    return TransferMemoryBytes + ui + swap + blurBytesFor(width, height) + 0x100000;
}

// Объекты deko3d и вся их память — из запаса видеочипа (glass_heap.hpp), не из кучи оверлея.
DkResult arenaAlloc(void*, size_t alignment, size_t size, void** out) {
    *out = heap::alloc(size, alignment);
    return *out ? DkResult_Success : DkResult_OutOfMemory;
}

void arenaFree(void*, void* mem) { heap::release(mem); }

// Параметры композита (shaders/present.frag, Params).
struct Params {
    float panel[4];
    float misc[4];
    float glass[4];
};

// Параметры захвата фона (shaders/capture.frag, Params).
struct CaptureParams {
    float rowX[4];
    float rowY[4];
    s32 region[4];
    u32 surface[4];
    u32 kind[4];
};

struct State {
    bool configured = false;
    dk::Device device;
    dk::Queue queue;
    dk::MemBlock uiMem, swapMem, cmdMem, codeMem, descMem, backdropMem, paramMem, captureMem, captureParamMem;
    dk::Shader vert, frag, srcFrag, downFrag, upFrag;
    dk::CmdBuf cmd, captureCmd;
    dk::Image uiImage;
    dk::Image blurImages[BlurLevels];   // 0 — захват и итог размытия, дальше ступени вдвое меньше
    u32 blurW[BlurLevels] = {}, blurH[BlurLevels] = {};
    dk::Image swapImages[SwapImages];
    dk::Swapchain swapchain;
    DkCmdList lists[SwapImages] = {};
    dk::Fence done;
    bool inFlight = false;
    bool lost = false;
    unsigned stalls = 0;
    u32 uiBytes = 0;
    unsigned frames = 0;
    // Фон: кадр дисплея этого кадра оверлея, импорт его буфера и список захвата под этот буфер.
    backdrop::Frame frame{};
    bool haveFrame = false;
    DkMemBlock source = nullptr;
    u32 sourceId = 0;
    DkCmdList captureList = 0;
    void* census = nullptr;
} g;

// Подготовка видеочипа — в фоне, оверлей в это время работает через libnx. Окно фон не трогает — его отдаёт
// Renderer между кадрами (ryzGpuAttach).
struct InitJob {
    Thread thread{};
    bool started = false, joined = false;
    u32 width = 0, height = 0;
    std::atomic<int> result{0};   // 0 — идёт, 1 — готово, -1 — отказ
    bool attached = false, failed = false;
    u64 startTick = 0;
} g_job;

void destroy() {
    backdrop::stop();
    for (auto& l : g.lists) l = 0;
    g.captureList = 0;
    if (g.swapchain) g.swapchain.destroy();
    if (g.captureCmd) g.captureCmd.destroy();
    if (g.cmd) g.cmd.destroy();
    if (g.queue) g.queue.destroy();   // ожидание внутри ограничено (libglass/deko3d: Queue::~Queue)
    if (g.source) {
        dkMemBlockDestroy(g.source);
        g.source = nullptr;
        g.sourceId = 0;
    }
    if (g.census) {
        heap::release(g.census);
        g.census = nullptr;
    }
    if (g.cmdMem) g.cmdMem.destroy();
    if (g.captureMem) g.captureMem.destroy();
    if (g.descMem) g.descMem.destroy();
    if (g.paramMem) g.paramMem.destroy();
    if (g.captureParamMem) g.captureParamMem.destroy();
    if (g.backdropMem) g.backdropMem.destroy();
    if (g.codeMem) g.codeMem.destroy();
    if (g.swapMem) g.swapMem.destroy();
    if (g.uiMem) g.uiMem.destroy();
    if (g.device) g.device.destroy();
}

// Проба до перехвата окна: видеочип выполняет команды этого процесса (list пустой) и рисует ими (list — кадр в
// образ свопчейна, без вывода). Не ответил — окно остаётся у libnx.
bool probe(const char* what, DkCmdList list) {
    if (list) g.queue.submitCommands(list);
    dk::Fence fence;
    g.queue.signalFence(fence, true);
    g.queue.flush();
    const DkResult r = fence.wait(ProbeWaitNs);
    const bool error = g.queue.isInErrorState();
    if (r != DkResult_Success || error) log("probe %s: %s", what, error ? "queue in error state" : "no answer");
    return r == DkResult_Success && !error;
}

bool prepare(u32 width, u32 height) {
    // nvdrv, nvmap, fence и канал видеочипа поднимает сам deko3d (Device::initialize).
    g.device = dk::DeviceMaker{}.setCbAlloc(arenaAlloc).setCbFree(arenaFree).create();
    if (!g.device) return false;

    // Буфер интерфейса: та же блочная раскладка, что у кадра libnx (блок 16 GOB) — Tesla рисует в него как прежде.
    dk::ImageLayout uiLayout;
    dk::ImageLayoutMaker{g.device}
        .setFlags(DkImageFlags_CustomTileSize)
        .setFormat(DkImageFormat_RGBA4_Unorm)
        .setDimensions(width, height)
        .setTileSize(DkTileSize_SixteenGobs)
        .initialize(uiLayout);
    const u32 expected = alignUp(width * 2, 64) * alignUp(height, 128);
    if (uiLayout.getSize() != expected) {
        log("ui layout %llu bytes, tesla %u: frames via libnx", (unsigned long long)uiLayout.getSize(), expected);
        return false;
    }
    g.uiBytes = expected;
    // Хватит ли запаса: deko3d на нехватке памяти не возвращает ошибку, а роняет процесс.
    const std::size_t need = gpuBytesFor(width, height) - TransferMemoryBytes;
    if (need > heap::available()) {
        log("GPU memory: %u KiB free, %ux%u needs %u KiB: frames via libnx", unsigned(heap::available() >> 10), width,
            height, unsigned(need >> 10));
        return false;
    }

    dk::ImageLayout swapLayout;
    dk::ImageLayoutMaker{g.device}
        .setFlags(DkImageFlags_UsagePresent | DkImageFlags_UsageRender)
        .setFormat(DkImageFormat_RGBA8_Unorm)
        .setDimensions(width, height)
        .initialize(swapLayout);
    const u32 swapBytes = alignUp(swapLayout.getSize(), swapLayout.getAlignment());

    g.uiMem = dk::MemBlockMaker{g.device, alignUp(g.uiBytes, DK_MEMBLOCK_ALIGNMENT)}
        .setFlags(DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
        .create();
    g.swapMem = dk::MemBlockMaker{g.device, alignUp(swapBytes * SwapImages, DK_MEMBLOCK_ALIGNMENT)}
        .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
        .create();
    g.cmdMem = dk::MemBlockMaker{g.device, CommandBytes}
        .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
        .create();
    if (!g.uiMem || !g.swapMem || !g.cmdMem || !g.uiMem.getCpuAddr()) return false;
    std::memset(g.uiMem.getCpuAddr(), 0, g.uiBytes);
    g.uiMem.flushCpuCache(0, g.uiBytes);

    g.uiImage.initialize(uiLayout, g.uiMem, 0);
    for (unsigned i = 0; i < SwapImages; ++i) g.swapImages[i].initialize(swapLayout, g.swapMem, i * swapBytes);

    g.queue = dk::QueueMaker{g.device}
        .setFlags(DkQueueFlags_Graphics | DkQueueFlags_LowPrio | DkQueueFlags_DisableZcull)
        .create();
    if (!g.queue) return false;

    // Шейдеры: код — в своём блоке (выравнивание 0x100, последние 0x400 байт блока не используются).
    const u32 fragOffset = alignUp(glass_present_vert_dksh_size, DK_SHADER_CODE_ALIGNMENT);
    const u32 srcOffset = alignUp(fragOffset + glass_present_frag_dksh_size, DK_SHADER_CODE_ALIGNMENT);
    const u32 downOffset = alignUp(srcOffset + glass_capture_frag_dksh_size, DK_SHADER_CODE_ALIGNMENT);
    const u32 upOffset = alignUp(downOffset + glass_blur_down_frag_dksh_size, DK_SHADER_CODE_ALIGNMENT);
    const u32 codeBytes = alignUp(upOffset + glass_blur_up_frag_dksh_size + DK_SHADER_CODE_UNUSABLE_SIZE, DK_MEMBLOCK_ALIGNMENT);
    g.codeMem = dk::MemBlockMaker{g.device, codeBytes}
        .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code)
        .create();
    if (!g.codeMem) return false;
    auto* code = static_cast<u8*>(g.codeMem.getCpuAddr());
    std::memcpy(code, glass_present_vert_dksh, glass_present_vert_dksh_size);
    std::memcpy(code + fragOffset, glass_present_frag_dksh, glass_present_frag_dksh_size);
    std::memcpy(code + srcOffset, glass_capture_frag_dksh, glass_capture_frag_dksh_size);
    std::memcpy(code + downOffset, glass_blur_down_frag_dksh, glass_blur_down_frag_dksh_size);
    std::memcpy(code + upOffset, glass_blur_up_frag_dksh, glass_blur_up_frag_dksh_size);
    dk::ShaderMaker{g.codeMem, 0}.initialize(g.vert);
    dk::ShaderMaker{g.codeMem, fragOffset}.initialize(g.frag);
    dk::ShaderMaker{g.codeMem, srcOffset}.initialize(g.srcFrag);
    dk::ShaderMaker{g.codeMem, downOffset}.initialize(g.downFrag);
    dk::ShaderMaker{g.codeMem, upOffset}.initialize(g.upFrag);
    if (!g.vert.isValid() || !g.frag.isValid() || !g.srcFrag.isValid() || !g.downFrag.isValid() || !g.upFrag.isValid())
        return false;

    // Фон под стекло: образ в половину панели RGBA8 — его рисует видеочип из буфера дисплея (список захвата) и там же
    // размывает ступенями; композит читает итог с линейной фильтрацией. Параметры композита и захвата — некэшируемые
    // блоки, процессор пишет их перед кадром.
    dk::ImageLayout blurLayouts[BlurLevels];
    u32 blurOffsets[BlurLevels] = {}, blurBytes = 0;
    for (unsigned i = 0; i < BlurLevels; ++i) {
        g.blurW[i] = ((width / 2) + (1u << i) - 1) >> i;
        g.blurH[i] = ((height / 2) + (1u << i) - 1) >> i;
        dk::ImageLayoutMaker{g.device}
            .setFlags(DkImageFlags_UsageRender)
            .setFormat(DkImageFormat_RGBA8_Unorm)
            .setDimensions(g.blurW[i], g.blurH[i])
            .initialize(blurLayouts[i]);
        blurBytes = alignUp(blurBytes, u32(blurLayouts[i].getAlignment()));
        blurOffsets[i] = blurBytes;
        blurBytes += u32(blurLayouts[i].getSize());
    }
    g.backdropMem = dk::MemBlockMaker{g.device, alignUp(blurBytes, DK_MEMBLOCK_ALIGNMENT)}
        .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
        .create();
    g.paramMem = dk::MemBlockMaker{g.device, DK_MEMBLOCK_ALIGNMENT}
        .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
        .create();
    g.captureParamMem = dk::MemBlockMaker{g.device, DK_MEMBLOCK_ALIGNMENT}
        .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
        .create();
    g.captureMem = dk::MemBlockMaker{g.device, CaptureCommandBytes}
        .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
        .create();
    if (!g.backdropMem || !g.paramMem || !g.captureParamMem || !g.captureMem) return false;
    // До первого кадра дисплея фона нет: композит выводит интерфейс как есть.
    std::memset(g.paramMem.getCpuAddr(), 0, UniformBytes);
    std::memset(g.captureParamMem.getCpuAddr(), 0, UniformBytes);
    for (unsigned i = 0; i < BlurLevels; ++i) g.blurImages[i].initialize(blurLayouts[i], g.backdropMem, blurOffsets[i]);

    // Дескрипторы: образ 0 — интерфейс, 1.. — ступени стекла (1 — итог); сэмплеры 0 — ближайший (texelFetch берёт
    // пиксель как есть), 1 — линейный с прижимом к краю (стекло).
    g.descMem = dk::MemBlockMaker{g.device, DK_MEMBLOCK_ALIGNMENT}
        .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
        .create();
    if (!g.descMem) return false;
    auto* desc = static_cast<u8*>(g.descMem.getCpuAddr());
    dk::ImageDescriptor imageDesc[1 + BlurLevels];
    imageDesc[0].initialize(dk::ImageView{g.uiImage});
    for (unsigned i = 0; i < BlurLevels; ++i) imageDesc[1 + i].initialize(dk::ImageView{g.blurImages[i]});
    dk::SamplerDescriptor samplerDesc[2];
    samplerDesc[0].initialize(dk::Sampler{});
    samplerDesc[1].initialize(dk::Sampler{}
        .setFilter(DkFilter_Linear, DkFilter_Linear)
        .setWrapMode(DkWrapMode_ClampToEdge, DkWrapMode_ClampToEdge, DkWrapMode_ClampToEdge));
    std::memcpy(desc, imageDesc, sizeof(imageDesc));
    std::memcpy(desc + 0x100, samplerDesc, sizeof(samplerDesc));
    const DkGpuAddr descAddr = g.descMem.getGpuAddr();

    // По списку композита на образ свопчейна, записаны один раз: свежий интерфейс и фон -> полноэкранный треугольник.
    // Есть ли фон — в параметрах, их процессор пишет перед кадром. Сам фон рисует список захвата (recordCapture).
    g.cmd = dk::CmdBufMaker{g.device}.create();
    g.captureCmd = dk::CmdBufMaker{g.device}.create();
    if (!g.cmd || !g.captureCmd) return false;
    g.cmd.addMemory(g.cmdMem, 0, CommandBytes);
    for (unsigned i = 0; i < SwapImages; ++i) {
        const dk::ImageView target{g.swapImages[i]};
        // Процессор писал в буфер интерфейса, видеочип — в фон: дождаться захвата и сбросить кэш текстур.
        g.cmd.barrier(DkBarrier_Fragments, DkInvalidateFlags_Image | DkInvalidateFlags_L2Cache);
        g.cmd.bindRenderTargets(&target);
        g.cmd.setViewports(0, { { 0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f } });
        g.cmd.setScissors(0, { { 0, 0, width, height } });
        g.cmd.bindShaders(DkStageFlag_GraphicsMask, { &g.vert, &g.frag });
        g.cmd.bindImageDescriptorSet(descAddr, 1 + BlurLevels);
        g.cmd.bindSamplerDescriptorSet(descAddr + 0x100, 2);
        g.cmd.bindTextures(DkStage_Fragment, 0, { dkMakeTextureHandle(0, 0), dkMakeTextureHandle(1, 1) });
        g.cmd.bindUniformBuffer(DkStage_Fragment, 0, g.paramMem.getGpuAddr(), UniformBytes);
        g.cmd.bindRasterizerState(dk::RasterizerState{}.setCullMode(DkFace_None));
        g.cmd.bindColorState(dk::ColorState{});
        g.cmd.bindColorWriteState(dk::ColorWriteState{});
        g.cmd.bindDepthStencilState(dk::DepthStencilState{}.setDepthTestEnable(false));
        g.cmd.draw(DkPrimitive_Triangles, 3, 1, 0, 0);
        g.lists[i] = g.cmd.finishList();
    }
    // Страница под метку поиска буфера дисплея (DirectCapture создаёт на ней свой nvmap, чтобы узнать свежий номер).
    g.census = heap::alloc(0x1000, 0x1000);
    if (!g.census) return false;
    // Видеочип отвечает и рисует — только тогда окно можно отдавать ему.
    return probe("fence", 0) && probe("draw", g.lists[0]);
}

void initThread(void*) {
    const u64 start = nowMs();
    const bool ok = prepare(g_job.width, g_job.height);
    g_prepareMs = nowMs() - start;
    if (!ok || g_prepareMs > TakeoverMaxPrepareMs)
        log("GPU preparation %s after %llu ms: frames via libnx", ok ? "too slow" : "failed",
            (unsigned long long)g_prepareMs);
    // Буфер дисплея ищется сразу, ещё до перехвата окна: к первому кадру через видеочип фон уже найден, и оверлей
    // появляется сразу со стеклом. Верх поиска — номер своего последнего блока (создан только что).
    if (ok && g_prepareMs <= TakeoverMaxPrepareMs)
        backdrop::start(ryzDkMemBlockId(g.uiMem), ryzDkMemBlockId(g.descMem), g.census, g_job.width, g_job.height);
    g_job.result.store(ok ? 1 : -1, std::memory_order_release);
}

void join() {
    if (g_job.started && !g_job.joined) {
        threadWaitForExit(&g_job.thread);
        threadClose(&g_job.thread);
        g_job.joined = true;
    }
}

// Видеочип перестал отвечать: окно отпустить, Tesla снова создаст буфер libnx. Объекты видеочипа живут до выхода.
void lose(const char* why) {
    log("frame %u: %s — frames back to libnx", g.frames, why);
    g.lost = true;
    g.inFlight = false;
    backdrop::stop();
    if (g.swapchain) g.swapchain.destroy();
}

// Проход стекла в образ ступени level: полноэкранный треугольник, вход — образ ступени source (или буфер дисплея).
void blurPass(unsigned level, const dk::Shader& shader, int source) {
    const dk::ImageView target{g.blurImages[level]};
    g.captureCmd.bindRenderTargets(&target);
    g.captureCmd.setViewports(0, { { 0.0f, 0.0f, float(g.blurW[level]), float(g.blurH[level]), 0.0f, 1.0f } });
    g.captureCmd.setScissors(0, { { 0, 0, g.blurW[level], g.blurH[level] } });
    g.captureCmd.bindShaders(DkStageFlag_GraphicsMask, { &g.vert, &shader });
    if (source >= 0) g.captureCmd.bindTextures(DkStage_Fragment, 0, { dkMakeTextureHandle(1 + source, 1) });
    g.captureCmd.draw(DkPrimitive_Triangles, 3, 1, 0, 0);
    // Следующий проход читает этот образ.
    g.captureCmd.barrier(DkBarrier_Fragments, DkInvalidateFlags_Image);
}

// Список стекла под импортированный буфер дисплея: образ захвата <- буфер (storage), затем размытие ступенями вниз до
// последней и обратно вверх в образ захвата. Пишется заново только при смене буфера (номер nvmap): видеочип прошлый кадр
// к этому моменту закончил (ryzGpuBegin).
void recordCapture() {
    g.captureCmd.clear();
    g.captureCmd.addMemory(g.captureMem, 0, CaptureCommandBytes);
    const DkGpuAddr descAddr = g.descMem.getGpuAddr();
    g.captureCmd.barrier(DkBarrier_Fragments, DkInvalidateFlags_Image | DkInvalidateFlags_L2Cache);
    g.captureCmd.bindImageDescriptorSet(descAddr, 1 + BlurLevels);
    g.captureCmd.bindSamplerDescriptorSet(descAddr + 0x100, 2);
    g.captureCmd.bindStorageBuffer(DkStage_Fragment, 0, dkMemBlockGetGpuAddr(g.source), dkMemBlockGetSize(g.source));
    g.captureCmd.bindUniformBuffer(DkStage_Fragment, 0, g.captureParamMem.getGpuAddr(), UniformBytes);
    g.captureCmd.bindRasterizerState(dk::RasterizerState{}.setCullMode(DkFace_None));
    g.captureCmd.bindColorState(dk::ColorState{});
    g.captureCmd.bindColorWriteState(dk::ColorWriteState{});
    g.captureCmd.bindDepthStencilState(dk::DepthStencilState{}.setDepthTestEnable(false));
    blurPass(0, g.srcFrag, -1);
    for (unsigned i = 1; i < BlurLevels; ++i) blurPass(i, g.downFrag, int(i - 1));
    for (unsigned i = BlurLevels - 1; i-- > 0;) blurPass(i, g.upFrag, int(i + 1));
    g.captureList = g.captureCmd.finishList();
}

// Буфер дисплея этого кадра — импортирован и под него записан список захвата. false — фона в этом кадре нет.
bool ensureSource(const backdrop::Frame& f) {
    if (!f.id) return false;
    if (g.source && g.sourceId == f.id) return g.captureList != 0;
    if (g.source) {
        dkMemBlockDestroy(g.source);
        g.source = nullptr;
        g.captureList = 0;
    }
    g.sourceId = f.id;
    g.source = ryzDkImportNvMapId(g.device, f.id);
    static unsigned logged = 0;
    if (g.source && dkMemBlockGetSize(g.source) < f.allocationBytes) {
        dkMemBlockDestroy(g.source);
        g.source = nullptr;
    }
    if (!g.source) {
        if (logged++ < 3) log("glass: display buffer id %u could not be imported", f.id);
        return false;
    }
    recordCapture();
    return true;
}

// Пиксель образа фона -> точка буфера сцены. Образ покрывает панель: пиксель образа -> пиксель буфера слоя ->
// вывод дисплея (через окно оверлея; не нашлось — по месту слоя и повороту панели) -> буфер сцены (через её окно).
ryz::glass::Affine captureMap(const backdrop::Frame& f) {
    const ryz::glass::Affine toLayer{ { float(g_job.width) / float(g.blurW[0]), 0.0f, 0.0f,
                                 0.0f, float(g_job.height) / float(g.blurH[0]), 0.0f } };
    ryz::glass::Affine toDisplay;
    if (f.overlayFound) {
        toDisplay = ryz::glass::bufferToDisplay(f.overlay);
    } else {
        const float* r = g_layerRect;
        const float dw = float(f.displayWidth), dh = float(f.displayHeight);
        if (dw >= dh)
            toDisplay = { { r[2] * dw, 0.0f, r[0] * dw, 0.0f, r[3] * dh, r[1] * dh } };
        else   // книжная панель портативного режима: альбомный x идёт вдоль её высоты снизу вверх, y — вдоль ширины
            toDisplay = { { 0.0f, r[3] * dw, r[1] * dw, -r[2] * dh, 0.0f, (1.0f - r[0]) * dh } };
    }
    return ryz::glass::compose(ryz::glass::displayToBuffer(f.scene), ryz::glass::compose(toDisplay, toLayer));
}

}  // namespace

void log(const char* fmt, ...) {
    mutexLock(&g_logMutex);
    if (FILE* f = std::fopen(LogPath, "a")) {
        std::fprintf(f, "[%6llu ms] ", (unsigned long long)nowMs());
        va_list args;
        va_start(args, fmt);
        std::vfprintf(f, fmt, args);
        va_end(args);
        std::fputc('\n', f);
        std::fclose(f);
    }
    mutexUnlock(&g_logMutex);
}

void configure() {
    g_t0 = armGetSystemTick();
    if (FILE* f = std::fopen(OffPath, "r")) {
        std::fclose(f);
        return;
    }
    // «Размытие GPU» выключено в настройках темы меню — оверлей как без стекла: кадры через libnx.
    if (ult::parseValueFromIniSection(ult::RYZHAND_CONFIG_INI_PATH, ult::RYZHAND_PROJECT_NAME, "glass_blur") == "false")
        return;
    const u32 width = ult::DefaultFramebufferWidth ? ult::DefaultFramebufferWidth : 448;
    const u32 height = ult::DefaultFramebufferHeight ? ult::DefaultFramebufferHeight : 720;
    const std::size_t extra = heap::reserve(gpuBytesFor(width, height));
    if (!extra) {
        log("no memory beyond the overlay heap (%s): frames via libnx", heap::reserveStory());
        return;
    }
    g_defaultTransferMemory = __nx_nv_transfermem_size;
    __nx_nv_transfermem_size = TransferMemoryBytes;
    // Служба nvdrv:t — только через неё процесс оверлея видит видеочип (nvdrv:s: nvGpuInit 0x255C).
    Service probe{};
    if (R_SUCCEEDED(smGetService(&probe, "nvdrv:t"))) {
        serviceClose(&probe);
        __nx_nv_service_type = NvServiceType_Factory;
    }
    g.configured = true;
}

}  // namespace ryz::glass

using namespace ryz::glass;

// libnx: transfer memory nvdrv при первом nvInitialize (это framebufferCreate в Renderer::init). Из запаса видеочипа:
// пока nvdrv её держит, она заперта, и в куче оверлея уронила бы ovlloader на чтении следующего оверлея.
extern "C" Result __nx_nv_create_tmem(TransferMemory* t, u32* out_size, Permission perm) {
    void* mem = g.configured ? ryz::glass::heap::alloc(TransferMemoryBytes, 0x1000) : nullptr;
    if (!mem) {
        // Без запаса — прежний размер Tesla: кадр libnx он тянет, видеочип без него не поднимется и не будет пытаться.
        if (g.configured) {
            __nx_nv_transfermem_size = g_defaultTransferMemory;
            g.configured = false;
            log("no GPU memory for transfer memory: frames via libnx");
        }
        *out_size = __nx_nv_transfermem_size;
        return tmemCreate(t, *out_size, perm);
    }
    *out_size = TransferMemoryBytes;
    const Result rc = tmemCreateFromMemory(t, mem, TransferMemoryBytes, perm);
    if (R_FAILED(rc)) {
        ryz::glass::heap::release(mem);
        return rc;
    }
    // tmemWaitForPermission и tmemClose берут адрес отсюда; память вернёт в запас __libnx_free (glass_heap.cpp).
    t->src_addr = mem;
    return rc;
}

// Сообщения deko3d (dk_device.cpp) — ошибка очереди видеочипа с разбором, ошибка перед падением.
extern "C" void ryzDkMessage(const char* message) { log("deko3d: %s", message); }

extern "C" void ryzGpuConfigure(void) { configure(); }

extern "C" void ryzGlassSetEnabled(bool on) { g_enabled.store(on, std::memory_order_relaxed); }
extern "C" bool ryzGlassBackdropReady(void) { return g.haveFrame && !g.lost; }

extern "C" void ryzGpuStart(NWindow*, u32 width, u32 height, const float layerRect[4]) {
    if (!g.configured || g_job.started) return;
    g_job.width = width;
    g_job.height = height;
    for (unsigned i = 0; i < 4; ++i) g_layerRect[i] = layerRect[i];
    g_job.startTick = armGetSystemTick();
    // Приоритет ниже интерфейса: подготовка не должна отнимать у Tesla кадры.
    if (R_FAILED(threadCreate(&g_job.thread, initThread, nullptr, nullptr, 0x8000, 0x3b, -2)) ||
        R_FAILED(threadStart(&g_job.thread))) {
        log("prepare thread failed: frames via libnx");
        return;
    }
    g_job.started = true;
}

extern "C" bool ryzGpuReady(void) {
    // Зовёт Renderer в начале каждого кадра, пока кадры идут через libnx. Первый кадр оверлея ждёт подготовку
    // видеочипа, чтобы оверлей сразу появился со стеклом, а не с обычным фоном темы. Не дольше HoldPrepareNs, дальше —
    // кадры через libnx.
    static bool first = true;
    if (first) {
        first = false;
        while (g_job.started && g_job.result.load(std::memory_order_acquire) == 0 &&
               armTicksToNs(armGetSystemTick() - g_job.startTick) < HoldPrepareNs)
            svcSleepThread(2'000'000);
    }
    // Сначала итог подготовки (acquire), потом её время: g_prepareMs записан до result.
    if (g_job.attached || g_job.failed || g_job.result.load(std::memory_order_acquire) != 1) return false;
    return g_prepareMs <= TakeoverMaxPrepareMs;
}

// Главный поток, между кадрами: буфер libnx уже отпущен — свопчейн на окно, дальше кадры через видеочип.
// Поток подготовки может ещё вести разведку — его не ждём, объекты видеочипа к этому моменту уже готовы.
extern "C" bool ryzGpuAttach(NWindow* window, void** buffer, u32* bufferSize) {
    DkImage const* images[SwapImages];
    for (unsigned i = 0; i < SwapImages; ++i) images[i] = &g.swapImages[i];
    g.swapchain = dk::SwapchainMaker{g.device, window, images, SwapImages}.create();
    if (!g.swapchain) {
        g_job.failed = true;
        log("swapchain failed: frames via libnx");
        return false;
    }
    g_job.attached = true;
    *buffer = g.uiMem.getCpuAddr();
    *bufferSize = g.uiBytes;
    // Фон под стекло — буфер, который показывает дисплей; поиск начат ещё при подготовке (initThread), здесь — на случай,
    // если тогда не поднялся. Не поднялся и тут — кадры без стекла.
    backdrop::start(ryzDkMemBlockId(g.uiMem), ryzDkMemBlockId(g.descMem), g.census, g_job.width, g_job.height);
    return true;
}

extern "C" void* ryzGpuBegin(u8* backgroundAlpha) {
    if (g.lost) return nullptr;
    // Видеочип ещё переносит прошлый кадр из этого же буфера — подождать его.
    if (g.inFlight) {
        const DkResult r = g.done.wait(FrameWaitNs);
        g.inFlight = false;
        if (g.queue.isInErrorState()) {
            lose("GPU queue error");
            return nullptr;
        }
        if (r != DkResult_Success) {
            if (++g.stalls >= MaxStalls) {
                lose("GPU stopped answering");
                return nullptr;
            }
        } else {
            g.stalls = 0;
        }
    }
    // Буфер дисплея импортируется здесь: видеочип прошлый кадр закончил, список захвата можно переписать.
    backdrop::frameShown();
    // Первый кадр через видеочип ждёт найденный буфер дисплея (по подсказке — миллисекунды), чтобы оверлей сразу
    // появился со стеклом. Не дольше HoldBackdropNs.
    if (g.frames == 0) {
        const u64 holdStart = armGetSystemTick();
        while (!backdrop::latest(g.frame) && armTicksToNs(armGetSystemTick() - holdStart) < HoldBackdropNs)
            svcSleepThread(2'000'000);
    }
    g.haveFrame = g_enabled.load(std::memory_order_relaxed) && backdrop::latest(g.frame) && ensureSource(g.frame);
    // Стекло решается до отрисовки: Tesla рисует фон темы с этой альфой, шейдер кладёт его на размытый кадр игры.
    *backgroundAlpha = g.haveFrame ? GlassBackgroundAlpha : 0xFF;
    return g.uiMem.getCpuAddr();
}

extern "C" void ryzGpuEnd(void) {
    if (g.lost) return;
    if (g.queue.isInErrorState()) {
        lose("GPU queue error");
        return;
    }
    g.uiMem.flushCpuCache(0, g.uiBytes);
    // Параметры: видеочип прошлый кадр закончил (ryzGpuBegin), блоки некэшируемые — пишутся сразу.
    Params params{};
    params.panel[0] = 1.0f / float(g_job.width);
    params.panel[1] = 1.0f / float(g_job.height);
    params.misc[0] = g.haveFrame ? 1.0f : 0.0f;
    params.glass[0] = float(GlassBackgroundAlpha) / 15.0f;
    params.glass[1] = GlassSaturation;
    params.glass[2] = PanelGlassBrightness;
    std::memcpy(g.paramMem.getCpuAddr(), &params, sizeof(params));
    if (g.haveFrame) {
        backdrop::refresh(g.frame);
        const backdrop::Frame& f = g.frame;
        const ryz::glass::Affine m = captureMap(f);
        s32 minX, minY, maxX, maxY;
        ryz::glass::windowRegion(f.scene, minX, minY, maxX, maxY);
        const CaptureParams cp{
            { m.m[0], m.m[1], m.m[2], 0.0f },
            { m.m[3], m.m[4], m.m[5], 0.0f },
            { minX, minY, maxX, maxY },
            { f.offset, f.allocationBytes - f.offset, f.scene.pitch, f.scene.blockHeight },
            { f.scene.format, 0, 0, 0 },
        };
        std::memcpy(g.captureParamMem.getCpuAddr(), &cp, sizeof(cp));
    }
    const int slot = g.queue.acquireImage(g.swapchain);
    if (slot < 0 || slot >= int(SwapImages)) {
        lose("bad swapchain slot");
        return;
    }
    // Сначала фон из буфера дисплея (живой, каждый кадр), потом композит интерфейса поверх него.
    if (g.haveFrame && g.captureList) g.queue.submitCommands(g.captureList);
    g.queue.submitCommands(g.lists[slot]);
    // Метка конца переноса — до вывода: presentImage отправляет видеочипу всё накопленное, и её тоже. Поставленная
    // после вывода, она ушла бы только со следующим кадром.
    g.queue.signalFence(g.done, true);
    g.queue.presentImage(g.swapchain, slot);
    g.inFlight = true;
    ++g.frames;
}

extern "C" void ryzGpuExit(void) {
    if (!g_job.started) return;
    join();
    destroy();
    if (g_job.attached && ryz::glass::heap::inUse())
        log("exit: %u KiB of GPU memory still held", unsigned(ryz::glass::heap::inUse() >> 10));
}
