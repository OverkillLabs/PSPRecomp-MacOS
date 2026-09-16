#version 450

// Bright-pass extraction + a small fixed blur kernel, in one pass. Reads the
// already-composited scene (world + HUD, screen-space), keeps only pixels
// above `threshold` luminance, and averages a 13-tap cross/diagonal pattern
// around each texel so the extracted highlights spread into a soft glow
// instead of staying pixel-sharp. This is a single-pass approximation (real
// separable multi-pass Gaussian bloom would look smoother at a wider radius)
// -- intentionally simple for a first, verifiable version: one pass, one
// input image, one output image, easy to reason about and to diff against
// with the threshold/intensity turned up for a diagnostic capture.
layout(push_constant) uniform BloomPushConstants {
    vec2 texel_size;
    float threshold;
    float intensity;
} pc;

layout(set = 0, binding = 0) uniform sampler2D scene_color;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

float luminance(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

vec3 bright_pass(vec2 uv) {
    vec3 c = texture(scene_color, uv).rgb;
    float l = luminance(c);
    float weight = max(l - pc.threshold, 0.0) / max(1.0 - pc.threshold, 1.0e-4);
    return c * clamp(weight, 0.0, 1.0);
}

void main() {
    const vec2 offsets[13] = vec2[13](
        vec2(0.0, 0.0),
        vec2(1.0, 0.0),  vec2(-1.0, 0.0),  vec2(0.0, 1.0),  vec2(0.0, -1.0),
        vec2(1.0, 1.0),  vec2(-1.0, 1.0),  vec2(1.0, -1.0), vec2(-1.0, -1.0),
        vec2(2.0, 0.0),  vec2(-2.0, 0.0),  vec2(0.0, 2.0),  vec2(0.0, -2.0)
    );
    vec3 sum = vec3(0.0);
    for (int i = 0; i < 13; ++i) {
        sum += bright_pass(v_uv + offsets[i] * pc.texel_size);
    }
    out_color = vec4(sum * (pc.intensity / 13.0), 1.0);
}
