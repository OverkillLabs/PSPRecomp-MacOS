#version 450

// Single-pass FXAA (luminance-edge-detection + directional blend), a
// simpler and safer substitute for true SMAA -- no precomputed area/search
// lookup textures, no multi-pass edge/blend-weight/neighborhood pipeline,
// just one shader sampling its own neighborhood. Standard, widely-used
// algorithm shape (Timothy Lottes' FXAA, simplified/console form): luma
// computed per-tap from the already-tonemapped output color, edge detected
// from local contrast, then blended along the estimated edge direction.
layout(push_constant) uniform FxaaPushConstants {
    vec2 texel_size;
} pc;

layout(set = 0, binding = 0) uniform sampler2D scene_color;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

void main() {
    vec3 rgb_center = texture(scene_color, v_uv).rgb;

    vec3 rgb_n = texture(scene_color, v_uv + vec2(0.0, -pc.texel_size.y)).rgb;
    vec3 rgb_s = texture(scene_color, v_uv + vec2(0.0, pc.texel_size.y)).rgb;
    vec3 rgb_e = texture(scene_color, v_uv + vec2(pc.texel_size.x, 0.0)).rgb;
    vec3 rgb_w = texture(scene_color, v_uv + vec2(-pc.texel_size.x, 0.0)).rgb;
    vec3 rgb_nw = texture(scene_color, v_uv + vec2(-pc.texel_size.x, -pc.texel_size.y)).rgb;
    vec3 rgb_ne = texture(scene_color, v_uv + vec2(pc.texel_size.x, -pc.texel_size.y)).rgb;
    vec3 rgb_sw = texture(scene_color, v_uv + vec2(-pc.texel_size.x, pc.texel_size.y)).rgb;
    vec3 rgb_se = texture(scene_color, v_uv + vec2(pc.texel_size.x, pc.texel_size.y)).rgb;

    float luma_center = luma(rgb_center);
    float luma_n = luma(rgb_n);
    float luma_s = luma(rgb_s);
    float luma_e = luma(rgb_e);
    float luma_w = luma(rgb_w);

    float luma_min = min(luma_center, min(min(luma_n, luma_s), min(luma_e, luma_w)));
    float luma_max = max(luma_center, max(max(luma_n, luma_s), max(luma_e, luma_w)));
    float range = luma_max - luma_min;

    // Below this contrast, the neighborhood isn't an edge -- leave the pixel
    // untouched rather than blurring flat/near-flat regions for no benefit.
    const float edge_threshold_min = 0.0312;
    const float edge_threshold = 0.125;
    if (range < max(edge_threshold_min, luma_max * edge_threshold)) {
        out_color = vec4(rgb_center, 1.0);
        return;
    }

    float luma_nw = luma(rgb_nw);
    float luma_ne = luma(rgb_ne);
    float luma_sw = luma(rgb_sw);
    float luma_se = luma(rgb_se);

    // Estimate the edge direction (horizontal vs vertical) from a simple
    // Sobel-like combination of the 8 neighbors' luma, then blend along the
    // perpendicular of that edge -- the standard FXAA horizontal/vertical
    // split, without the later subpixel-shift refinement passes real FXAA
    // 3.11 adds (kept out deliberately: this stays a single, easily
    // reasoned-about pass).
    float edge_horizontal =
        abs(luma_nw - 2.0 * luma_n + luma_ne) * 2.0 +
        abs(luma_w - 2.0 * luma_center + luma_e) +
        abs(luma_sw - 2.0 * luma_s + luma_se);
    float edge_vertical =
        abs(luma_nw - 2.0 * luma_w + luma_sw) * 2.0 +
        abs(luma_n - 2.0 * luma_center + luma_s) +
        abs(luma_ne - 2.0 * luma_e + luma_se);
    bool horizontal_edge = edge_horizontal >= edge_vertical;

    vec3 blend_a = horizontal_edge ? (rgb_n + rgb_s) * 0.5 : (rgb_w + rgb_e) * 0.5;
    vec3 blend_b = (rgb_n + rgb_s + rgb_e + rgb_w) * 0.25;
    // Blend factor scaled by local contrast: stronger edges blend more
    // toward the wider neighborhood average, subtle ones barely move.
    float blend_factor = clamp(range * 2.0, 0.0, 0.75);
    vec3 result = mix(rgb_center, mix(blend_a, blend_b, 0.5), blend_factor);
    out_color = vec4(result, 1.0);
}
