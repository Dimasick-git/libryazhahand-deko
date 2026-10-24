// SPDX-License-Identifier: GPL-2.0-only
#pragma once

namespace ryz::glass {

// ВЫВОД КАДРА ЧЕРЕЗ ВИДЕОЧИП (deko3d). Tesla рисует интерфейс процессором, как всегда, но не в буфер libnx, а в
// текстуру RGBA4 с той же блочной раскладкой; видеочип переносит её в образ свопчейна RGBA8 и выводит в слой
// оверлея. Под интерфейсом — стекло: размытый кадр игры (glass_backdrop.hpp), фон темы ложится на него тонировкой.
//
// Видеочип готовится в отдельном потоке (0.05–0.1 с с ovlloader-апплетом), первый кадр оверлея его ждёт и сразу
// выходит со стеклом. Окно он берёт, только ответив на две пробы: пустую команду и перенос кадра в свой образ.
// Не поднялся — кадры через libnx. Перестал отвечать посреди работы — окно возвращается к libnx. Вся
// память видеочипа — в отдельном запасе (glass_heap.hpp). Tesla зовёт модуль через слабые функции ryzGpu* (tesla.hpp):
// оверлей без USING_GLASS_DIRECTIVE их не содержит.
// Ошибки — sdmc:/config/ryazhahand/glass.log; sdmc:/config/ryazhahand/glass_off.flag выключает видеочип совсем.

// В начале tsl::loop (ryzGpuConfigure): запас памяти и nvdrv (transfer memory, служба) — до первого nvInitialize.
void configure();
// Строка в журнал ошибок с временем от запуска оверлея; из любого потока.
void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace ryz::glass
