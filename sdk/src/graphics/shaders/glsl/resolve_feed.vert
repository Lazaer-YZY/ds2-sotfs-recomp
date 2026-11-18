#version 450
// ReXGlue resolve feeding: a triangle covering the viewport (the destination
// rectangle in the texture).
void main() {
  vec2 position = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);
}
