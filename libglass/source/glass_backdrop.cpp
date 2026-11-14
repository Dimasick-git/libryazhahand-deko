// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Dimasick-git (RCU)
// Фон под стекло. См. glass_backdrop.hpp.

#include "glass_backdrop.hpp"
#include "glass_capture.hpp"
#include "glass_present.hpp"

#include <atomic>
#include <cstdio>

namespace ryz::glass::backdrop {
namespace {

constexpr u64 PeriodNs = 8'000'000;        // регистры DC — раз в ~8 мс: дисплей меняет буфер не чаще кадра
// Показанный буфер неизвестен (защёлка ждёт, а прошлый взгляд старше кадра) — смотреть раз в миллисекунду, пока окно не
// перестанет ждать или очередь не сменится при свежем взгляде; не дольше FastTries подряд.
constexpr u64 FastNs = 1'000'000;
constexpr unsigned FastTries = 50;
constexpr u64 IdleNs = 300'000'000;        // оверлей не рисует дольше — спрятан, искать незачем
constexpr u64 FreshNs = 2'000'000'000;     // столько оверлей на экране без найденного кадра — фон убрать
// Найденные номера буферов сцены — между запусками: пул HOME создаётся на загрузке и держит один номер до
// перезагрузки, с подсказкой он находится сразу, без перебора чужих буферов.
constexpr char HintsPath[] = "sdmc:/config/ryazhahand/glass_ids.txt";
constexpr unsigned MaxHints = 8;

DirectCapture g_capture;
Thread g_thread{};
bool g_started = false;
std::atomic<bool> g_run{false};
std::atomic<u64> g_frameTick{0};
std::atomic<u64> g_shownTick{0};

// Последний найденный кадр: пишет поток, читает кадр оверлея.
Mutex g_mutex;
Frame g_frame{};
bool g_have = false;
u64 g_frameFoundTick = 0;

u64 nsSince(u64 tick) { return armTicksToNs(armGetSystemTick() - tick); }

uint32_t g_hints[MaxHints] = {};
unsigned g_hintCount = 0;

void loadHints() {
    g_hintCount = 0;
    if (FILE* f = std::fopen(HintsPath, "r")) {
        unsigned id = 0;
        while (g_hintCount < MaxHints && std::fscanf(f, "%u", &id) == 1)
            if (id) g_hints[g_hintCount++] = id;
        std::fclose(f);
    }
}

// Новый найденный номер — в начало списка подсказок.
void remember(uint32_t id) {
    for (unsigned i = 0; i < g_hintCount; ++i)
        if (g_hints[i] == id) return;
    for (unsigned i = (g_hintCount < MaxHints ? g_hintCount : MaxHints - 1); i > 0; --i) g_hints[i] = g_hints[i - 1];
    g_hints[0] = id;
    if (g_hintCount < MaxHints) ++g_hintCount;
    if (FILE* f = std::fopen(HintsPath, "w")) {
        for (unsigned i = 0; i < g_hintCount; ++i) std::fprintf(f, "%u\n", g_hints[i]);
        std::fclose(f);
    }
}

void publish(const DirectFrame& f) {
    mutexLock(&g_mutex);
    g_frame = f;
    g_have = true;
    g_frameFoundTick = armGetSystemTick();
    mutexUnlock(&g_mutex);
}

void run(void*) {
    unsigned misses = 0, fast = 0;
    uint32_t ambiguousLogged = 0;
    while (g_run.load(std::memory_order_acquire)) {
        if (nsSince(g_frameTick.load(std::memory_order_acquire)) > IdleNs) {
            svcSleepThread(50'000'000);
            continue;
        }
        DirectFrame frame;
        if (g_capture.acquire(frame)) {
            misses = 0;
            publish(frame);
            remember(frame.id);
        } else if (++misses == 500) {
            glass::log("glass: display buffer not found in 500 tries");
        }
        // Журнал — на 1, 2, 4, 8… случаях, не на каждом.
        const uint32_t ambiguous = g_capture.ambiguousLatches();
        if (ambiguous && ambiguous >= ambiguousLogged * 2) {
            ambiguousLogged = ambiguous;
            glass::log("glass: shown frame unclear %u times (latch pending, previous read over a frame ago), frames skipped",
                       ambiguous);
        }
        fast = g_capture.shownUnknown() ? fast + 1 : 0;
        svcSleepThread(fast && fast <= FastTries ? FastNs : PeriodNs);
    }
}

}  // namespace

bool start(uint32_t firstOwnId, uint32_t ownNvmapId, void* census, uint32_t overlayWidth, uint32_t overlayHeight) {
    if (g_started) return true;
    loadHints();
    if (!g_capture.initialize(firstOwnId, ownNvmapId, census, overlayWidth, overlayHeight, g_hints, g_hintCount)) {
        glass::log("glass: display capture unavailable");
        return false;
    }
    g_run.store(true, std::memory_order_release);
    // Оверлей вот-вот покажется: искать сразу, не дожидаясь первого кадра.
    g_frameTick.store(armGetSystemTick(), std::memory_order_release);
    g_shownTick.store(armGetSystemTick(), std::memory_order_release);
    // Ниже интерфейса: поиск буфера не должен отнимать у Tesla кадры.
    if (R_FAILED(threadCreate(&g_thread, run, nullptr, nullptr, 0x8000, 0x3c, -2)) || R_FAILED(threadStart(&g_thread))) {
        g_run.store(false, std::memory_order_release);
        g_capture.close();
        glass::log("glass: capture thread failed");
        return false;
    }
    g_started = true;
    return true;
}

void stop() {
    if (!g_started) return;
    g_run.store(false, std::memory_order_release);
    threadWaitForExit(&g_thread);
    threadClose(&g_thread);
    g_capture.close();
    g_started = false;
    mutexLock(&g_mutex);
    g_have = false;
    mutexUnlock(&g_mutex);
}

void frameShown() {
    const u64 now = armGetSystemTick();
    const u64 prev = g_frameTick.exchange(now, std::memory_order_acq_rel);
    if (!prev || armTicksToNs(now - prev) > IdleNs) g_shownTick.store(now, std::memory_order_release);
}

bool refresh(Frame& frame) {
    return g_started && g_capture.refreshAddress(frame);
}

bool latest(Frame& out) {
    mutexLock(&g_mutex);
    const bool have = g_have;
    const u64 found = g_frameFoundTick;
    if (have) out = g_frame;
    mutexUnlock(&g_mutex);
    if (!have) return false;
    // После повторного показа прежний кадр держится, пока поток не найдёт новый: буфер дисплея тот же или соседний.
    if (nsSince(found) > FreshNs && nsSince(g_shownTick.load(std::memory_order_acquire)) > FreshNs) return false;
    return true;
}

}  // namespace ryz::glass::backdrop
