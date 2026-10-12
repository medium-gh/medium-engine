#version 450
layout(set=2,binding=0) uniform sampler2D atlas;
// x: 1 draws the texture's texels crisp - pixel art - and 0 filtered, as everything else is.
layout(set=3,binding=0) uniform Params { vec4 params; };
layout(location=0) in vec2 uv;
layout(location=1) in vec4 tint;
layout(location=0) out vec4 outColor;
void main() {
    // Crisp texels at any scale: each texel's own colour across the screen pixels it covers, blended only
    // within the one screen pixel a texel edge falls in, so pixel art stays hard-edged at 1.5x without
    // uneven pixels or shimmer as it moves. Where a texel is smaller than a screen pixel `crisp` is `uv`
    // exactly, and it is filtered - through the texture's mip chain when it has one (a composed character
    // page), which is what keeps a minified sheet's ink lines and shade bands from shimmering. Magnified, the
    // ramp moves at most one texel a screen pixel, so its level of detail stays at the first level.
    vec2 size = vec2(textureSize(atlas, 0));
    vec2 texel = uv * size;
    vec2 span = max(fwidth(texel), vec2(1e-5));
    vec2 seam = floor(texel + 0.5);
    vec2 crisp = (seam + clamp((texel - seam) / min(span, vec2(1.0)), -0.5, 0.5)) / size;
    vec4 colour = texture(atlas, params.x > 0.5 ? crisp : uv);
    if (params.y > 0.0 || params.z > 0.0) {
        // A small separable Gaussian kernel evaluated in one pass. Derivatives make its radius
        // screen-space, independent of source texture resolution and camera zoom. Only distant
        // backdrop draws enable it. Premultiplied accumulation prevents transparent dark fringes.
        vec2 dx = dFdx(uv) * params.y;
        vec2 dy = dFdy(uv) * params.z;
        vec4 sum = vec4(0.0);
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x) {
                float weight = (x == 0 ? 2.0 : 1.0) * (y == 0 ? 2.0 : 1.0) / 16.0;
                vec4 sampleColour = texture(atlas, uv + float(x) * dx + float(y) * dy);
                sum += vec4(sampleColour.rgb * sampleColour.a, sampleColour.a) * weight;
            }
        colour = vec4(sum.a > 0.00001 ? sum.rgb / sum.a : vec3(0.0), sum.a);
    }
    // Untextured effects keep one bounded quad while their silhouette fades smoothly, instead
    // of covering the weapon and floor with opaque square flash/dust blocks.
    if (params.w > 0.5) {
        vec2 p = uv * 2.0 - 1.0;
        float radius = length(p);
        float coverage = 0.0;
        if (params.w < 1.5) {
            coverage = exp(-4.0 * radius * radius) * (1.0 - smoothstep(0.75, 1.0, radius));
        } else if (params.w < 2.5) {
            float diamond = abs(p.x) + abs(p.y);
            coverage = pow(max(1.0 - diamond, 0.0), 0.55);
        } else if (params.w < 3.5) {
            float angle = radius > 0.00001 ? atan(p.y, p.x) : 0.0;
            float edge = 0.77 + 0.055 * sin(angle * 7.0) + 0.035 * cos(angle * 11.0);
            coverage = (1.0 - smoothstep(edge - 0.22, edge + 0.04, radius)) *
                       (0.65 + 0.35 * max(1.0 - radius, 0.0));
        } else if (params.w < 4.5) {
            float width = max(fwidth(radius) * 1.5, 0.08);
            coverage = 1.0 - smoothstep(0.018, width, abs(radius - 0.76));
        }
        if (params.w > 4.5) {
            float across = abs(p.y);
            coverage = exp(-3.0 * across * across) * (1.0 - smoothstep(0.75, 1.0, across));
        }
        colour.a *= coverage;
    }
    outColor = colour * tint;
}
