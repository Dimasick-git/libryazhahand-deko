#version 460

// Фон под стекло: кадр, который сейчас показывает дисплей, — прямо из его буфера (импорт nvmap, хранилище source).
// Пиксель образа фона -> точка буфера сцены: одно аффинное отображение (его считает процессор по окнам дисплея:
// панель -> вывод через окно оверлея, вывод -> буфер через окно сцены). Образ — половина панели, тексель — одна точка
// буфера: дальше его гладко размывает цепочка blur_down/blur_up. Адрес block linear считается здесь.
layout(location = 0) out vec4 outColor;

layout(std140, binding = 0) uniform Params {
    vec4 rowX;          // x буфера = dot(rowX.xy, p) + rowX.z, p — точка образа фона в пикселях
    vec4 rowY;          // y буфера = dot(rowY.xy, p) + rowY.z
    ivec4 region;       // xy — первый, zw — последний пиксель буфера, которые окно читает
    uvec4 surface;      // x — начало окна в буфере (байты), y — доступно байт от начала, z — шаг строки, w — высота блока
    uvec4 kind;         // x — формат DC
};

layout(std430, binding = 0) readonly buffer Source { uint source[]; };

uint offsetOf(uvec2 p)
{
    uint x = p.x * 4u, y = p.y, pitch = surface.z, bh = surface.w;
    if (bh == 0u) return y * pitch + x;
    uint logBlock = bh - 1u, logRows = logBlock + 3u, rows = 1u << logRows;
    // Только сдвиги и маски: делители — степени двойки, и адрес точен и за 16 МиБ.
    return (y >> logRows) * pitch * rows + ((x >> 6u) * 512u << logBlock) + ((y & (rows - 1u)) >> 3u) * 512u +
           ((x & 63u) >> 5u) * 256u + ((y & 7u) >> 1u) * 64u + ((x & 31u) >> 4u) * 32u + (y & 1u) * 16u + (x & 15u);
}

vec3 texel(vec2 p)
{
    const vec2 c = vec2(dot(rowX.xy, p) + rowX.z, dot(rowY.xy, p) + rowY.z);
    const ivec2 at = clamp(ivec2(floor(c)), region.xy, region.zw);
    const uint offset = offsetOf(uvec2(at));
    if (offset > surface.y - 4u) return vec3(0.0);
    uint v = source[(surface.x + offset) / 4u];
    // Форматы DC 12 и 37 — синий в младшем байте: поменять местами с красным.
    if (kind.x == 12u || kind.x == 37u) v = (v & 0xff00ff00u) | ((v & 0xffu) << 16u) | ((v >> 16u) & 0xffu);
    return vec3(float(v & 0xffu), float((v >> 8u) & 0xffu), float((v >> 16u) & 0xffu));
}

void main()
{
    outColor = vec4(texel(gl_FragCoord.xy) * (1.0 / 255.0), 1.0);
}
