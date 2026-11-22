#version 450
#extension GL_GOOGLE_include_directive : require
#include "colour_vision.glsl"
layout(set = 0, binding = 0) uniform sampler2D source_image;
layout(location = 0) out vec4 colour;
layout(push_constant) uniform PresentColour {
    vec4 uv;
    vec4 rect;  // offset and inverse extent of the content rectangle
    int mode;
    int source_linear;  // sampling an sRGB view already decoded it
    int target_srgb;    // an sRGB attachment encodes on store
} pc;
void main() {
    const vec2 t = (gl_FragCoord.xy - pc.rect.xy) * pc.rect.zw;
    const vec4 pixel = textureLod(source_image, mix(pc.uv.xy, pc.uv.zw, t), 0.0);
    const vec3 encoded = clamp(pixel.rgb, 0.0, 1.0);
    const vec3 light = pc.source_linear != 0 ? encoded : cv_decode_srgb(encoded);
    const vec3 corrected = cv_correct(light, pc.mode, false, 1.0);
    colour = vec4(pc.target_srgb != 0 ? corrected : cv_encode_srgb(corrected), pixel.a);
}
