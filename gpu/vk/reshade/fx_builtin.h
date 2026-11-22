#pragma once

#include "fx_clarity.h"

#include <string>

namespace mkwvk::reshade {

// The numbers are reshade_preset's in longshot_options.ini, so a number once given never changes. The Clarity looks are
// the last block: a look appended to clarity_presets takes the next number and moves none
inline constexpr int user_preset = 3;
inline constexpr int hdr_preset = 4;
inline constexpr int depth_view_preset = 5;
inline constexpr int depth_haze_preset = 6;
inline constexpr int first_clarity_preset = 7;
inline constexpr int last_builtin_preset = first_clarity_preset + static_cast<int>(clarity_presets.size()) - 1;
static_assert(depth_haze_preset + 1 == first_clarity_preset, "the Clarity looks are the last block");

// Work in the same sRGB range as the SDR versions. Normalize by the pixel's peak
// before grading, then restore the peak in linear light so highlights can exceed 1.
inline constexpr const char* builtin_color_space = R"FX(
#if BUFFER_COLOR_SPACE == 2
float GradePeak(float3 rgb) { return max(1.0, max(rgb.r, max(rgb.g, rgb.b))); }

float3 GradeInput(float3 rgb)
{
    float3 v = max(rgb, 0.0) / GradePeak(rgb);
    return lerp(v * 12.92, 1.055 * pow(v, 1.0 / 2.4) - 0.055, step(0.0031308, v));
}

float3 GradeOutput(float3 graded, float3 original)
{
    float3 decoded = lerp(graded / 12.92, pow((graded + 0.055) / 1.055, 2.4), step(0.04045, graded));
    return decoded * GradePeak(original) + min(original, 0.0);
}
#else
float3 GradeInput(float3 rgb) { return rgb; }
float3 GradeOutput(float3 graded, float3 original) { return graded; }
#endif
)FX";

inline constexpr const char* color_fx = R"FX(
texture BackBuffer : COLOR;
sampler BackBufferSampler { Texture = BackBuffer; };
uniform float3 Tint = float3(1.0, 1.0, 1.0);
uniform float Saturation = 1.0;

void Fullscreen(uint id : SV_VertexID, out float4 position : SV_Position, out float2 uv : TEXCOORD0)
{
    uv = float2((id << 1) & 2, id & 2);
    position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 Grade(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float4 color = tex2D(BackBufferSampler, uv);
    float3 rgb = GradeInput(color.rgb);
    float luma = dot(rgb, float3(0.2126, 0.7152, 0.0722));
    color.rgb = GradeOutput(saturate(lerp(luma.xxx, rgb, Saturation) * Tint), color.rgb);
    return color;
}

technique LongshotColor
{
    pass { VertexShader = Fullscreen; PixelShader = Grade; }
}
)FX";

inline constexpr const char* color_presets[] = {
    "Techniques=LongshotColor@LongshotColor.fx\n[LongshotColor.fx]\nTint=1.04,1.0,0.94\nSaturation=1.12\n",
    "Techniques=LongshotColor@LongshotColor.fx\n[LongshotColor.fx]\nTint=0.95,1.0,1.04\nSaturation=0.88\n",
};

inline constexpr const char* hdr_fx = R"FX(
texture Scene : COLOR;
sampler SceneSampler { Texture = Scene; };

void Fullscreen(uint id : SV_VertexID, out float4 position : SV_Position, out float2 uv : TEXCOORD0)
{
    uv = float2((id << 1) & 2, id & 2);
    position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 Grade(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float4 color = tex2D(SceneSampler, uv);
    float luma = dot(color.rgb, float3(0.2126, 0.7152, 0.0722));
    float lowest = min(color.r, min(color.g, color.b));
    float amount = min(0.12, max(0.0, lowest) / max(luma - lowest, 0.00001));
    color.rgb += (color.rgb - luma.xxx) * amount;
    return color;
}

technique LongshotHDRVibrance
{
    pass { VertexShader = Fullscreen; PixelShader = Grade; }
}
)FX";

inline constexpr const char* depth_fx = R"FX(
texture Scene : COLOR;
texture SceneDepth : DEPTH;
sampler SceneSampler { Texture = Scene; };
sampler DepthSampler { Texture = SceneDepth; MinFilter = POINT; MagFilter = POINT; };

void Fullscreen(uint id : SV_VertexID, out float4 position : SV_Position, out float2 uv : TEXCOORD0)
{
    uv = float2((id << 1) & 2, id & 2);
    position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 DepthView(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float d = tex2D(DepthSampler, uv).r;
    float shade = pow(saturate(d), 20.0);
    return float4(shade, shade, shade, 1.0);
}

float4 DepthHaze(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float d = tex2D(DepthSampler, uv).r;
    float4 scene = tex2D(SceneSampler, uv);
    float3 grade = lerp(GradeInput(scene.rgb), float3(0.57, 0.68, 0.77), 0.19 * smoothstep(0.985, 1.0, d));
    scene.rgb = GradeOutput(grade, scene.rgb);
    return scene;
}

technique LongshotDepthView { pass { VertexShader = Fullscreen; PixelShader = DepthView; } }
technique LongshotDepthHaze { pass { VertexShader = Fullscreen; PixelShader = DepthHaze; } }
)FX";

struct BuiltinPreset {
    const char* shader;
    std::string ini;
    std::string source() const { return std::string(builtin_color_space) + shader; }
};

inline constexpr bool supports_color_space(bool native_hdr, bool linear_hdr, bool builtin) {
    return native_hdr == linear_hdr || (native_hdr && builtin);
}

inline BuiltinPreset builtin_preset(int choice) {
    if (choice == 1 || choice == 2) return {color_fx, color_presets[choice - 1]};
    if (choice == hdr_preset) return {hdr_fx, "LongshotColorSpace=LinearHDR\nTechniques=LongshotHDRVibrance@LongshotHDR.fx\n"};
    if (choice == depth_view_preset) return {depth_fx, "Techniques=LongshotDepthView@LongshotDepth.fx\n"};
    if (choice == depth_haze_preset) return {depth_fx, "Techniques=LongshotDepthHaze@LongshotDepth.fx\n"};
    if (choice < first_clarity_preset || choice > last_builtin_preset) return {nullptr, {}};
    const auto& preset = clarity_presets[choice - first_clarity_preset];
    return {clarity_fx, std::string("Techniques=LongshotClarity@LongshotClarity.fx\n[LongshotClarity.fx]\n") + preset.settings};
}

}  // namespace mkwvk::reshade
