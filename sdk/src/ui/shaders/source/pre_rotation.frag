#version 450
#extension GL_EXT_samplerless_texture_functions : require
// present_pre_rotation: copies the painted (landscape, logical) image into the swapchain image, which is in the
// display's native orientation, pixel for pixel, rotated by the swapchain's preTransform (the image content must
// already be rotated clockwise by that angle, Vulkan's VkSurfaceTransformFlagBitsKHR).
layout(set = 0, binding = 0) uniform texture2D xe_pre_rotation_source;
layout(push_constant) uniform XePreRotationConstants {
  layout(offset = 0) uint xe_pre_rotation_transform;  // VkSurfaceTransformFlagBitsKHR: 2 = 90, 4 = 180, 8 = 270
};
layout(location = 0) out vec4 xe_pre_rotation_color;
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  ivec2 size = textureSize(xe_pre_rotation_source, 0);
  ivec2 s = p;
  if (xe_pre_rotation_transform == 2u) {
    // 90 degrees clockwise: logical (x, y) -> physical (H - 1 - y, x).
    s = ivec2(p.y, size.y - 1 - p.x);
  } else if (xe_pre_rotation_transform == 8u) {
    // 270 degrees clockwise: logical (x, y) -> physical (y, W - 1 - x).
    s = ivec2(size.x - 1 - p.y, p.x);
  } else if (xe_pre_rotation_transform == 4u) {
    s = ivec2(size.x - 1 - p.x, size.y - 1 - p.y);
  }
  xe_pre_rotation_color = texelFetch(xe_pre_rotation_source, clamp(s, ivec2(0), size - 1), 0);
}
