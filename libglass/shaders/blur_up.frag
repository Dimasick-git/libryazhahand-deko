#version 460

// Стекло, ступень вверх (dual Kawase): образ вдвое больше источника. Восемь выборок по кругу радиусом в тексель
// источника — без ступенек и колец, которые даёт простое растяжение.
layout(location = 0) out vec4 outColor;
layout(binding = 0) uniform sampler2D src;

void main()
{
    const vec2 texel = 1.0 / vec2(textureSize(src, 0));
    const vec2 uv = gl_FragCoord.xy * 0.5 * texel;
    const vec2 h = texel * 0.5;
    vec3 sum = texture(src, uv + vec2(-h.x * 2.0, 0.0)).rgb;
    sum += texture(src, uv + vec2(-h.x, h.y)).rgb * 2.0;
    sum += texture(src, uv + vec2(0.0, h.y * 2.0)).rgb;
    sum += texture(src, uv + vec2(h.x, h.y)).rgb * 2.0;
    sum += texture(src, uv + vec2(h.x * 2.0, 0.0)).rgb;
    sum += texture(src, uv + vec2(h.x, -h.y)).rgb * 2.0;
    sum += texture(src, uv + vec2(0.0, -h.y * 2.0)).rgb;
    sum += texture(src, uv + vec2(-h.x, -h.y)).rgb * 2.0;
    outColor = vec4(sum * (1.0 / 12.0), 1.0);
}
