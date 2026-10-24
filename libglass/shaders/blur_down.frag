#version 460

// Стекло, ступень вниз (dual Kawase): образ вдвое меньше источника. Тексель — центр его участка 2x2 (вес 4) и четыре
// диагонали на текселе источника; линейная выборка усредняет по 2x2, так что каждая ступень — гладкий фильтр.
layout(location = 0) out vec4 outColor;
layout(binding = 0) uniform sampler2D src;

void main()
{
    const vec2 texel = 1.0 / vec2(textureSize(src, 0));
    const vec2 uv = gl_FragCoord.xy * 2.0 * texel;
    vec3 sum = texture(src, uv).rgb * 4.0;
    sum += texture(src, uv - texel).rgb;
    sum += texture(src, uv + texel).rgb;
    sum += texture(src, uv + vec2(texel.x, -texel.y)).rgb;
    sum += texture(src, uv - vec2(texel.x, -texel.y)).rgb;
    outColor = vec4(sum * 0.125, 1.0);
}
