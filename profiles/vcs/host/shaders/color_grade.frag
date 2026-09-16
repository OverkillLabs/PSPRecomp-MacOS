#version 450

// Parametric color grading: brightness (additive), contrast (around 0.5
// pivot), saturation (luma-lerp), and a multiplicative RGB tint -- applied
// in that order, the standard grading pipeline shape. Not a real 3D-LUT
// lookup (no .cube/strip texture asset exists in this project to sample);
// this reaches the same practical goal -- adjustable color/mood -- without
// needing a LUT asset pipeline. Runs after FXAA when both are enabled (see
// ge_gpu_backend_vulkan.cpp's pass ordering), so grading is the last thing
// touching pixels before they're read back/presented.
//
// Also carries an optional final unsharp-mask sharpen pass (Rendering.Sharpen
// in the .ini) -- PSP-era textures are low-res (many well under 128x128) and
// get magnified a lot by the internal-resolution upscale plus bilinear
// filtering, which reads as soft/blurry. This doesn't reconstruct real
// detail that was never captured (that needs per-texture AI upscaling, well
// out of scope here) -- it's the standard, cheap "boost local contrast at
// edges" trick that makes the existing detail read as crisper.
layout(push_constant) uniform ColorGradePushConstants {
    float saturation;
    float contrast;
    float brightness;
    float tint_r;
    float tint_g;
    float tint_b;
    float sharpen_strength;
    vec2 texel_size;
} pc;

layout(set = 0, binding = 0) uniform sampler2D scene_color;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

void main() {
    vec3 color = texture(scene_color, v_uv).rgb;

    if (pc.sharpen_strength > 0.0) {
        // Classic 4-neighbor unsharp mask: subtract a cheap box blur from
        // the center sample and add the difference back, scaled by
        // strength. Cheap (4 extra taps), no separate blur pass needed.
        vec3 n = texture(scene_color, v_uv + vec2(0.0, -pc.texel_size.y)).rgb;
        vec3 s = texture(scene_color, v_uv + vec2(0.0, pc.texel_size.y)).rgb;
        vec3 e = texture(scene_color, v_uv + vec2(pc.texel_size.x, 0.0)).rgb;
        vec3 w = texture(scene_color, v_uv + vec2(-pc.texel_size.x, 0.0)).rgb;
        vec3 blur = (n + s + e + w) * 0.25;
        color += (color - blur) * pc.sharpen_strength;
    }

    color += pc.brightness;
    color = (color - 0.5) * pc.contrast + 0.5;
    color = mix(vec3(luma(color)), color, pc.saturation);
    color *= vec3(pc.tint_r, pc.tint_g, pc.tint_b);
    out_color = vec4(clamp(color, 0.0, 1.0), 1.0);
}
