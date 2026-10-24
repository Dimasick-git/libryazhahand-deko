#version 460

// Полноэкранный треугольник: вершины (-1,-1), (3,-1), (-1,3) — без буфера вершин.
void main()
{
    const vec2 pos = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
    gl_Position = vec4(pos, 0.0, 1.0);
}
