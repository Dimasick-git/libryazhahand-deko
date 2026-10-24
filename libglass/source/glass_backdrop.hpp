// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cstdint>
#include <switch.h>

#include "glass_frame.hpp"

namespace ryz::glass::backdrop {

// ФОН ПОД СТЕКЛО: кадр, который сейчас показывает дисплей, — живой, каждый кадр, без снимков. Поток читает регистры
// контроллера дисплея и находит буфер сцены среди буферов nvmap (glass_capture.hpp); видеочип оверлея берёт этот
// буфер по номеру и сам собирает из него уменьшенный образ панели (shaders/capture.frag). Поток работает,
// только пока оверлей на экране.

using Frame = DirectFrame;

// firstOwnId..ownNvmapId — свои буферы nvmap (верх поиска — последний), census — страница 0x1000 под метку поиска,
// overlayWidth/Height — буфер слоя оверлея.
bool start(uint32_t firstOwnId, uint32_t ownNvmapId, void* census, uint32_t overlayWidth, uint32_t overlayHeight);
void stop();
// Кадр оверлея выводится — фон нужен. Без кадров дольше 0.3 с поток не ищет.
void frameShown();
// Свежий кадр дисплея; false — фона нет (не найден или старше 2 с при оверлее на экране).
bool latest(Frame& out);
// Перед кадром: адрес буфера, который дисплей показывает сейчас. Поток публикует раз в ~8 мс — за это время дисплей
// успевает перелистнуть, и без перечитывания стекло брало бы недорисованный задний буфер.
void refresh(Frame& frame);

}  // namespace ryz::glass::backdrop
