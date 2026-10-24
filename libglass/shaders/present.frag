#version 460

// Кадр оверлея: интерфейс Tesla (буфер RGBA4, texelFetch) поверх размытого кадра игры (backdrop). Фон уже собран и
// размыт видеочипом (backdrop_src.frag, затем blur_down/blur_up) и покрывает ровно область панели — отсюда выборка по
// gl_FragCoord/размер панели, одна, с линейной фильтрацией.
layout(location = 0) out vec4 outColor;
layout(binding = 0) uniform sampler2D uiTex;       // интерфейс, 1:1
layout(binding = 1) uniform sampler2D backdrop;    // размытый кадр игры в половину панели

layout(std140, binding = 0) uniform Params {
    vec4 panel;   // xy — 1/ширина, 1/высота панели
    vec4 misc;    // x — фон есть (0/1)
    vec4 glass;   // x — альфа фона темы под стеклом (0..1), y — насыщенность размытого фона
};

void main()
{
    const vec4 ui = texelFetch(uiTex, ivec2(gl_FragCoord.xy), 0);
    if (misc.x < 0.5 || ui.a <= 0.0) {
        outColor = ui;
        return;
    }
    vec3 b = texture(backdrop, gl_FragCoord.xy * panel.xy).rgb;
    // Размытие гасит насыщенность — вернуть её, как у стекла в системах с размытым фоном.
    const float luma = dot(b, vec3(0.299, 0.587, 0.114));
    b = clamp(mix(vec3(luma), b, glass.y), 0.0, 1.0);
    // Плавный фон в 8 битах даёт полосы — шум в полшага их разбивает.
    const float n = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    b += (n - 0.5) * (1.0 / 255.0);
    // Фон темы ложится тонировкой, текст и элементы (альфа ~1) — как есть. Прозрачность по альфе темы: при появлении
    // и исчезновении Tesla уводит её к нулю, и стекло уходит вместе с ней.
    outColor = vec4(mix(b, ui.rgb, ui.a), clamp(ui.a / glass.x, 0.0, 1.0));
}
