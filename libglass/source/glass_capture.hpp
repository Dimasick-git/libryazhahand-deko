// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Dimasick-git (RCU)
#pragma once
#include "glass_frame.hpp"
#include <switch.h>

namespace ryz::glass {

// Буфер, который показывает дисплей. Регистры DC (только чтение) дают адрес окна сцены в адресном пространстве SMMU
// дисплея; у VIC то же пространство (проверка по MC), поэтому буфер nvmap, закреплённый в канале VIC, получает тот же
// адрес — по нему и ищется номер. Один поток владеет всеми закреплениями.
class DirectCapture {
public:
    // firstOwnId..latestOwnId — свои буферы (не трогать; верх поиска — последний), census — страница 0x1000 под
    // метку поиска, overlayWidth/Height — буфер слоя оверлея (его окно задаёт место панели на выводе),
    // hints — номера, найденные в прошлые разы (первыми).
    bool initialize(uint32_t firstOwnId, uint32_t latestOwnId, void* censusStorage, uint32_t overlayWidth,
                    uint32_t overlayHeight, const uint32_t* hints, unsigned hintCount);
    bool acquire(DirectFrame&);
    // Из потока кадра: адрес готового кадра окна сцены прямо сейчас, если он в том же буфере nvmap. Читает только
    // регистры окна (без SMC) — дёшево, каждый кадр.
    bool refreshAddress(DirectFrame&) const;
    // Из потока захвата между полными чтениями: запрос защёлки и взведённый адрес окна сцены последнего кадра (без
    // SMC), чтобы не проглядеть кадр, отданный на показ.
    void watch() const;
    void close();
    // Счётчики для журнала (см. committedAddress).
    struct Stats {
        uint32_t reads = 0, requested = 0, assemblyAhead = 0, armedEarly = 0, held = 0, unknown = 0;
    };
    Stats stats() const;

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
    // Окно сцены последнего найденного кадра — его смотрит watch().
    unsigned watchController_ = 0, watchIndex_ = 0;
    uint32_t watchSignature_ = 0;
    bool watchValid_ = false;
    // Готовый кадр окна по взведённой копии адреса (см. committedAddress). Оба потока читают под общим замком
    // (DcReadGuard), поэтому состояние общее.
    struct Track {
        uint64_t last = 0, confirmed = 0;      // взведённый адрес прошлого взгляда; последний подтверждённый готовым
        uint64_t idleSince = 0;                // с этого тика взведён новый адрес без запроса защёлки
        uint32_t signature = 0;
        bool lastFreshIdle = false;            // прошлый взгляд: новый адрес и защёлка не запрошена
    };
    mutable Track track_[2][3]{};
    mutable Stats stats_{};
    // Взведённая копия не читается (всегда 0 при живом адресе сборки) — адрес из сборки, как раньше.
    mutable bool armOff_ = false, armLogged_ = false, armSeen_ = false;
    mutable uint32_t armZero_ = 0;

    uint64_t armedAddress(const volatile uint32_t* regs, unsigned buffer, uint64_t assembly) const;
    uint64_t committedAddress(unsigned controller, unsigned index, uint64_t armed, uint64_t assembly,
                              uint32_t signature, const volatile uint32_t* regs) const;
    bool readFrame(DirectFrame&);
    bool matchKept(DirectFrame&);
    bool tryId(uint32_t id, DirectFrame&);
    void release(Pin&);
};

}  // namespace ryz::glass
