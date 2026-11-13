// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cstdint>

namespace ryz::glass {

// Окно контроллера дисплея (DC): какой кусок какого буфера и куда оно выводит.
// Модель выборки: пиксель вывода берёт пиксель буфера corner + stepX*s + stepY*t, где s — источник вдоль строки
// вывода (0..srcX), t — между строками (0..srcY), stepX/stepY — оси буфера со знаком: отражения окна и поворот
// (scan column — строка вывода идёт по столбцу буфера, так портативный режим кладёт альбомный кадр на книжную панель;
// знаки — по осям буфера: H_DIRECTION по x, V_DIRECTION по y).
struct DcWindow {
    uint32_t controller = 0, index = 0, depth = 0, format = 0, options = 0;
    uint32_t srcX = 0, srcY = 0;                       // пикселей источника вдоль строки вывода и строк
    uint32_t x = 0, y = 0, extentX = 0, extentY = 0;   // место окна на выводе
    int32_t originX = 0, originY = 0;                  // первый выбираемый пиксель буфера
    int32_t stepX[2]{}, stepY[2]{};
    uint32_t pitch = 0, blockHeight = 0;               // blockHeight: 0 — построчно, n — блок 2^(n-1) GOB
    uint64_t address = 0;
};

// Кадр под стекло: окно сцены (игра или HOME) и буфер nvmap, в котором оно лежит.
struct DirectFrame {
    uint32_t id = 0, allocationBytes = 0, offset = 0;  // номер nvmap, его размер, начало окна в нём
    uint64_t base = 0;                                 // адрес этого буфера у дисплея (закрепление в канале VIC)
    uint32_t displayWidth = 0, displayHeight = 0;
    DcWindow scene;
    DcWindow overlay;                                  // окно слоя оверлея, если нашлось
    bool overlayFound = false;
};

// Only a still-separate, unchanged hardware plane can supply a live frame.
// Address is intentionally separate: it may advance within the same nvmap pool.
inline bool windowMappingMatches(const DcWindow& w, const volatile uint32_t* regs) {
    if (w.index > 2) return false;
    const unsigned c = 0xb80 + w.index * 0x200, b = 0xbc0 + w.index * 0x200;
    const uint32_t pre = regs[c + 6], pos = regs[c + 4], extent = regs[c + 5], kind = regs[b + 11];
    return ((kind & 3) == 0 || (kind & 3) == 2) && (regs[c] & 0x40000000) && regs[c] == w.options &&
           regs[c + 3] == w.format && (pre & 0x7fff) / 4 == w.srcX && (pre >> 16) == w.srcY &&
           (pos & 0x1fff) == w.x && ((pos >> 16) & 0x1fff) == w.y &&
           (extent & 0x1fff) == w.extentX && ((extent >> 16) & 0x1fff) == w.extentY &&
           (regs[c + 10] & 0xffff) == w.pitch && (regs[c + 0x16] & 255) == w.depth &&
           int32_t(regs[b + 6] / 4) == w.originX && int32_t(regs[b + 8]) == w.originY &&
           (((kind & 3) == 2 ? ((kind >> 4) & 7) + 1 : 0) == w.blockHeight) &&
           !(regs[b + 0x2f] & 1) && !regs[c + 1];
}

// Аффинное отображение плоскости: out = (m[0]*x + m[1]*y + m[2], m[3]*x + m[4]*y + m[5]).
struct Affine {
    float m[6] = { 1, 0, 0, 0, 1, 0 };
};

inline Affine compose(const Affine& outer, const Affine& inner) {
    const float* a = outer.m;
    const float* b = inner.m;
    return { { a[0] * b[0] + a[1] * b[3], a[0] * b[1] + a[1] * b[4], a[0] * b[2] + a[1] * b[5] + a[2],
               a[3] * b[0] + a[4] * b[3], a[3] * b[1] + a[4] * b[4], a[3] * b[2] + a[4] * b[5] + a[5] } };
}

// Внешний угол первого пикселя: с него окно начинает выборку (с отражением — с дальнего края пикселя).
inline void windowCorner(const DcWindow& w, float& cx, float& cy) {
    cx = float(w.originX) + float(1 - w.stepX[0] - w.stepY[0]) * 0.5f;
    cy = float(w.originY) + float(1 - w.stepX[1] - w.stepY[1]) * 0.5f;
}

// Точка буфера -> точка вывода.
inline Affine bufferToDisplay(const DcWindow& w) {
    float cx, cy;
    windowCorner(w, cx, cy);
    const float kx = float(w.extentX) / float(w.srcX), ky = float(w.extentY) / float(w.srcY);
    // s = stepX·(c - corner), t = stepY·(c - corner); вывод = место + (s*kx, t*ky).
    const float sx = float(w.stepX[0]), sy = float(w.stepX[1]), tx = float(w.stepY[0]), ty = float(w.stepY[1]);
    return { { sx * kx, sy * kx, float(w.x) - (sx * cx + sy * cy) * kx,
               tx * ky, ty * ky, float(w.y) - (tx * cx + ty * cy) * ky } };
}

// Точка вывода -> точка буфера.
inline Affine displayToBuffer(const DcWindow& w) {
    float cx, cy;
    windowCorner(w, cx, cy);
    const float ks = float(w.srcX) / float(w.extentX), kt = float(w.srcY) / float(w.extentY);
    // c = corner + stepX*s + stepY*t, s = (dx - x)*ks, t = (dy - y)*kt.
    const float sx = float(w.stepX[0]), sy = float(w.stepX[1]), tx = float(w.stepY[0]), ty = float(w.stepY[1]);
    return { { sx * ks, tx * kt, cx - sx * ks * float(w.x) - tx * kt * float(w.y),
               sy * ks, ty * kt, cy - sy * ks * float(w.x) - ty * kt * float(w.y) } };
}

// Область буфера, которую окно читает: [minX..maxX] x [minY..maxY] включительно.
inline void windowRegion(const DcWindow& w, int32_t& minX, int32_t& minY, int32_t& maxX, int32_t& maxY) {
    const int32_t ex = w.originX + w.stepX[0] * int32_t(w.srcX - 1) + w.stepY[0] * int32_t(w.srcY - 1);
    const int32_t ey = w.originY + w.stepX[1] * int32_t(w.srcX - 1) + w.stepY[1] * int32_t(w.srcY - 1);
    minX = w.originX < ex ? w.originX : ex;
    maxX = w.originX < ex ? ex : w.originX;
    minY = w.originY < ey ? w.originY : ey;
    maxY = w.originY < ey ? ey : w.originY;
}

// Сколько байт буфера окно сцены читает от своего начала: меньший буфер сценой быть не может.
inline uint64_t windowBytes(const DcWindow& w) {
    int32_t minX, minY, maxX, maxY;
    windowRegion(w, minX, minY, maxX, maxY);
    if (maxY < 0) return 0;
    const uint64_t rows = w.blockHeight ? 8u << (w.blockHeight - 1) : 1;
    return (uint64_t(maxY) / rows + 1) * rows * w.pitch;
}

// Окно сцены целиком лежит в буфере nvmap размером allocationBytes начиная с offset.
inline bool validDirectFrame(const DirectFrame& f) {
    const DcWindow& w = f.scene;
    if (!f.id || !w.srcX || !w.srcY || w.srcX > 4096 || w.srcY > 4096 || !w.extentX || !w.extentY ||
        w.pitch < 64 || w.pitch > 65536 || w.pitch % 64 || w.blockHeight > 6 || f.offset % 4 ||
        f.offset >= f.allocationBytes)
        return false;
    int32_t minX, minY, maxX, maxY;
    windowRegion(w, minX, minY, maxX, maxY);
    if (minX < 0 || minY < 0 || uint32_t(maxX) >= w.pitch / 4) return false;
    const uint64_t rows = w.blockHeight ? 8u << (w.blockHeight - 1) : 1;
    const uint64_t required = (uint64_t(maxY) / rows + 1) * rows * w.pitch;
    return required <= uint64_t(f.allocationBytes - f.offset);
}

}  // namespace ryz::glass
