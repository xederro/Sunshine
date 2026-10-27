#version 300 es

#ifdef GL_ES
precision highp float;
#endif

uniform sampler2D image;
uniform int gaze_enabled;
uniform vec2 gaze_center;
uniform vec4 gaze_radii;
uniform vec2 gaze_texel_size;
uniform vec3 gaze_blur_color;
uniform int gaze_tint_enabled;

layout(shared) uniform ColorMatrix {
  vec4 color_vec_y;
  vec4 color_vec_u;
  vec4 color_vec_v;
  vec2 range_y;
  vec2 range_uv;
};

in vec2 tex;
layout(location = 0) out float color;

vec3 foveated_rgb(vec2 uv) {
  vec3 original = texture(image, uv).rgb;
  if (gaze_enabled == 0) return original;
  vec2 delta = (uv - gaze_center) * vec2(1.0, gaze_texel_size.x / gaze_texel_size.y);
  float distance_from_gaze = length(delta);
  float soft_edge = smoothstep(gaze_radii.x, max(gaze_radii.y, gaze_radii.x + 0.0001), distance_from_gaze);
  if (soft_edge <= 0.0) return original;
  float level_1 = smoothstep(gaze_radii.y, max(gaze_radii.z, gaze_radii.y + 0.0001), distance_from_gaze);
  float level_2 = smoothstep(gaze_radii.z, max(gaze_radii.w, gaze_radii.z + 0.0001), distance_from_gaze);
  float outer_blur_end = gaze_radii.w + max(gaze_radii.w - gaze_radii.z, 0.0001);
  float level_3 = smoothstep(gaze_radii.w, outer_blur_end, distance_from_gaze);
  float sigma = 2.0 + 5.0 * level_1 + 7.0 * level_2 + 10.0 * level_3;
  const int sample_count = 24;
  const float golden_angle = 2.39996323;
  vec3 blurred = vec3(0.0);
  for (int i = 0; i < sample_count; ++i) {
    float fraction = (float(i) + 0.5) / float(sample_count);
    float radius = sigma * sqrt(-2.0 * log(1.0 - fraction));
    float angle = float(i) * golden_angle;
    vec2 offset = gaze_texel_size * radius * vec2(cos(angle), sin(angle));
    blurred += texture(image, uv + offset).rgb;
  }
  blurred /= float(sample_count);
  if (gaze_tint_enabled != 0) blurred = mix(blurred, gaze_blur_color, 0.7);
  return mix(original, blurred, soft_edge);
}

void main()
{
	vec3 rgb = foveated_rgb(tex);
	float u = dot(color_vec_u.xyz, rgb) + color_vec_u.w;

	color = u * range_uv.x + range_uv.y;
}
