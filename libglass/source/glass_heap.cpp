// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Dimasick-git (RCU)
#include "glass_heap.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <switch.h>

// Граница кучи newlib в libnx (newlib.c: sbrk не выходит за неё).
extern "C" char* fake_heap_end;

namespace ryz::glass::heap {
namespace {

constexpr std::size_t Granularity = 0x200000;       // svcSetHeapSize — кратно 2 МБ
constexpr std::size_t ExtraWant = 0xA00000;         // 10 МБ, при отказе — шагами по 2 МБ вниз до нужного
constexpr std::size_t MinAlign = 0x10;
constexpr unsigned MaxBlocks = 64;                  // видеочип берёт два десятка кусков

// Запас — список кусков по возрастанию адреса: занятые и свободные, соседние свободные слиты.
struct Block {
    std::uintptr_t addr;
    std::size_t size;
    bool used;
};

std::size_t g_original = 0;
std::uintptr_t g_base = 0, g_end = 0;
Block g_blocks[MaxBlocks];
unsigned g_count = 0;
std::size_t g_used = 0;
Mutex g_mutex;
char g_story[192] = "not tried";

std::size_t fail(const char* fmt, unsigned long long a = 0, unsigned long long b = 0, unsigned long long c = 0) {
    std::snprintf(g_story, sizeof(g_story), fmt, a, b, c);
    return 0;
}

std::size_t take(std::size_t original, std::uintptr_t tail, std::size_t extra) {
    g_original = original;
    g_base = tail;
    g_end = tail + extra;
    g_blocks[0] = {tail, extra, false};
    g_count = 1;
    return extra;
}

void insert(unsigned at, const Block& b) {
    for (unsigned i = g_count; i > at; --i) g_blocks[i] = g_blocks[i - 1];
    g_blocks[at] = b;
    ++g_count;
}

void erase(unsigned at) {
    for (unsigned i = at; i + 1 < g_count; ++i) g_blocks[i] = g_blocks[i + 1];
    --g_count;
}

}  // namespace

std::size_t reserve(std::size_t need) {
    need = (need + Granularity - 1) & ~(Granularity - 1);
    const std::size_t want = need > ExtraWant ? need : ExtraWant;
    if (g_count) return fail("already reserved");
    if (!envHasHeapOverride()) return fail("no heap override from the loader");
    u64 base = 0, region = 0;
    if (R_FAILED(svcGetInfo(&base, InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0)) ||
        R_FAILED(svcGetInfo(&region, InfoType_HeapRegionSize, CUR_PROCESS_HANDLE, 0)))
        return fail("heap region unknown");
    // Куча оверлея — хвост кучи процесса, newlib кончается там же: только тогда прибавка встаёт сразу за ней.
    const std::uintptr_t tail = reinterpret_cast<std::uintptr_t>(envGetHeapOverrideAddr()) + envGetHeapOverrideSize();
    if (reinterpret_cast<std::uintptr_t>(fake_heap_end) != tail || tail <= base)
        return fail("newlib ends at 0x%llx, overlay heap at 0x%llx", (unsigned long long)(uintptr_t)fake_heap_end, tail);
    const std::size_t original = tail - base;
    if (original % Granularity) return fail("process heap 0x%llx is not 2 MiB aligned", original);
    MemoryInfo last{};
    u32 page = 0;
    if (R_FAILED(svcQueryMemory(&last, &page, tail - 1)) || (last.type & MemState_Type) != MemType_Heap)
        return fail("no heap right before 0x%llx (type 0x%llx)", tail, last.type);
    // Куча процесса после кучи оверлея: прибавка прошлого запуска, которую он не вернул. Соседние куски кучи с одними
    // правами система показывает одним куском — поэтому идём по кускам до конца кучи.
    std::uintptr_t heapEnd = tail;
    bool clean = true;
    for (unsigned guard = 0; guard < 16; ++guard) {
        MemoryInfo mi{};
        if (R_FAILED(svcQueryMemory(&mi, &page, heapEnd)) || (mi.type & MemState_Type) != MemType_Heap) break;
        if (mi.perm != Perm_Rw || mi.attr) clean = false;
        heapEnd = mi.addr + mi.size;
    }
    if (heapEnd > tail) {
        const std::size_t left = heapEnd - tail;
        if (!clean) return fail("previous extension 0x%llx at 0x%llx is still held (not plain RW heap)", left, tail);
        if (left < need) {
            void* out = nullptr;
            if (R_FAILED(svcSetHeapSize(&out, original)))
                return fail("previous extension 0x%llx too small and could not be dropped", left);
        } else {
            std::snprintf(g_story, sizeof(g_story), "adopted 0x%llx left by the previous run", (unsigned long long)left);
            return take(original, tail, left);
        }
    }
    MemoryInfo next{};
    if (R_FAILED(svcQueryMemory(&next, &page, tail)) || (next.type & MemState_Type) != MemType_Unmapped ||
        next.addr != tail || next.size < need)
        return fail("no room after the heap (type 0x%llx, size 0x%llx)", next.type, next.size);

    Result last_rc = 0;
    for (std::size_t extra = want; extra >= need; extra -= Granularity) {
        const std::size_t total = original + extra;
        if (total > region) continue;
        void* out = nullptr;
        last_rc = svcSetHeapSize(&out, total);
        if (R_FAILED(last_rc)) continue;
        if (reinterpret_cast<std::uintptr_t>(out) != base) break;
        std::snprintf(g_story, sizeof(g_story), "grown by 0x%llx", (unsigned long long)extra);
        return take(original, tail, extra);
    }
    void* out = nullptr;
    svcSetHeapSize(&out, original);
    return fail("memory pool refused 0x%llx (rc 0x%llx, heap 0x%llx)", need, last_rc, original);
}

const char* reserveStory() { return g_story; }

void* alloc(std::size_t size, std::size_t align) {
    if (!size) return nullptr;
    if (align < MinAlign) align = MinAlign;
    size = (size + MinAlign - 1) & ~(MinAlign - 1);
    void* out = nullptr;
    mutexLock(&g_mutex);
    for (unsigned i = 0; i < g_count; ++i) {
        const Block whole = g_blocks[i];
        if (whole.used) continue;
        const std::uintptr_t start = (whole.addr + align - 1) & ~(std::uintptr_t(align) - 1);
        const std::size_t pad = start - whole.addr;
        if (pad + size > whole.size) continue;
        const std::size_t rest = whole.size - pad - size;
        if (g_count + (pad ? 1 : 0) + (rest ? 1 : 0) > MaxBlocks) break;
        unsigned at = i;
        if (pad) {
            g_blocks[at] = {whole.addr, pad, false};
            insert(++at, {start, size, true});
        } else {
            g_blocks[at] = {start, size, true};
        }
        if (rest) insert(at + 1, {start + size, rest, false});
        g_used += size;
        out = reinterpret_cast<void*>(start);
        break;
    }
    mutexUnlock(&g_mutex);
    return out;
}

void release(void* p) {
    const auto addr = reinterpret_cast<std::uintptr_t>(p);
    mutexLock(&g_mutex);
    for (unsigned i = 0; i < g_count; ++i) {
        if (!g_blocks[i].used || g_blocks[i].addr != addr) continue;
        g_blocks[i].used = false;
        g_used -= g_blocks[i].size;
        if (i + 1 < g_count && !g_blocks[i + 1].used) {
            g_blocks[i].size += g_blocks[i + 1].size;
            erase(i + 1);
        }
        if (i > 0 && !g_blocks[i - 1].used) {
            g_blocks[i - 1].size += g_blocks[i].size;
            erase(i);
        }
        break;
    }
    mutexUnlock(&g_mutex);
}

bool owns(const void* p) {
    const auto addr = reinterpret_cast<std::uintptr_t>(p);
    return addr >= g_base && addr < g_end;
}

std::size_t inUse() { return g_used; }
std::size_t available() { return (g_end - g_base) - g_used; }

}  // namespace ryz::glass::heap

// libnx отдаёт сюда всё, что брал через __libnx_aligned_alloc, в том числе transfer memory nvdrv. Её видеочип берёт
// из запаса (glass_present.cpp: __nx_nv_create_tmem) — туда же и возвращается.
extern "C" void __libnx_free(void* p) {
    if (ryz::glass::heap::owns(p))
        ryz::glass::heap::release(p);
    else
        std::free(p);
}

// Конец __appExit libtesla: видеочип и nvdrv уже закрыты. Что-то в запасе осталось занятым — значит, его ещё держит
// драйвер; тогда прибавку не трогаем (ovlloader её не использует, следующий оверлей просто обойдётся без видеочипа).
extern "C" void ryzAfterAppExit(void) {
    using namespace ryz::glass::heap;
    if (!g_count || g_used) return;
    void* out = nullptr;
    if (R_SUCCEEDED(svcSetHeapSize(&out, g_original))) {
        g_count = 0;
        g_base = g_end = 0;
    }
}
