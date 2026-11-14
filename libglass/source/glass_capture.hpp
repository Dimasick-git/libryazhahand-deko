// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "glass_frame.hpp"
#include <switch.h>

namespace ryz::glass {

// Буфер, который сейчас показывает дисплей. Регистры DC (только чтение) дают адрес окна сцены в адресном
// пространстве SMMU дисплея; у VIC то же пространство (проверка по MC), поэтому буфер nvmap, закреплённый в канале
// VIC, получает тот же адрес — по нему и ищется номер. Один поток владеет всеми закреплениями.
class DirectCapture {
public:
    // firstOwnId..latestOwnId — свои буферы (не трогать; верх поиска — последний), census — страница 0x1000 под
    // метку поиска, overlayWidth/Height — буфер слоя оверлея (его окно задаёт место панели на выводе),
    // hints — номера, найденные в прошлые разы (первыми).
    bool initialize(uint32_t firstOwnId, uint32_t latestOwnId, void* censusStorage, uint32_t overlayWidth,
                    uint32_t overlayHeight, const uint32_t* hints, unsigned hintCount);
    bool acquire(DirectFrame&);
    // Из потока кадра: адрес, который дисплей показывает прямо сейчас, если он в том же буфере nvmap. Читает только
    // регистры окна (без SMC) — дёшево, каждый кадр.
    bool refreshAddress(DirectFrame&) const;
    void close();

private:
    struct Pin {
        NvMap map{};
        uint32_t address = 0;
    };
    static constexpr unsigned MaxKept = 4;     // найденные буферы сцены (свопчейн игры — 2-3 буфера)
    Pin kept_[MaxKept]{};
    unsigned keptNext_ = 0;
    // Уже проверенные буферы: их адрес у дисплея стабилен, пока их кто-то держит, — второй раз не закреплять
    // (закрепление большого незакреплённого пула — тяжёлая работа службы видеочипа, игра бы подтормаживала).
    struct Seen {
        uint32_t id = 0, address = 0, size = 0;
    };
    static constexpr unsigned SeenSlots = 1024;
    Seen seen_[SeenSlots]{};
    unsigned seenNext_ = 0;
    unsigned bigBudget_ = 0;                   // больших закреплений ещё можно в этом вызове
    static constexpr unsigned MaxHints = 8;
    uint32_t hints_[MaxHints]{};
    unsigned hintCount_ = 0, hintNext_ = 0;
    uint32_t firstOwn_ = 0;
    DcWindow lastOverlay_{};                   // окно оверлея с прошлого чтения: на листании его адрес бывает 0
    bool haveOverlay_ = false;
    uint64_t needBytes_ = 0;                   // меньше этого буфер не может держать окно сцены
    volatile uint32_t* dc_[2]{};
    volatile uint32_t* clocks_ = nullptr;
    uint32_t vic_ = ~0u;
    uint32_t overlayWidth_ = 0, overlayHeight_ = 0;
    uint32_t nextId_ = 0, ceiling_ = 0, misses_ = 0;
    uint32_t lastMatch_ = 0, neighbor_ = 0;    // соседи последнего найденного номера — первыми
    uint64_t lastAddress_ = 0;
    bool ready_ = false;
    void* censusStorage_ = nullptr;
    // Что окно сейчас ПОКАЗЫВАЕТ (активный банк), выведенное без READ_MUX, — см. shownAddress. Оба потока читают
    // под общим замком (DcReadGuard), поэтому состояние общее.
    struct Shown {
        uint64_t queued = 0, shown = 0;
        uint32_t signature = 0;
        bool known = false;
    };
    mutable Shown shown_[2][3]{};

    uint64_t shownAddress(unsigned controller, unsigned index, uint64_t queued, uint32_t signature,
                          const volatile uint32_t* regs) const;
    bool readFrame(DirectFrame&);
    bool matchKept(DirectFrame&);
    bool tryId(uint32_t id, DirectFrame&);
    void release(Pin&);
};

}  // namespace ryz::glass
