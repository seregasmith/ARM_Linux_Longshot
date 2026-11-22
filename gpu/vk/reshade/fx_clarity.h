#pragma once

#include <array>

namespace mkwvk::reshade {

struct ClarityPreset {
    const char* name;
    const char* settings;
};

// SDR adaptations of the Clarity GFX presets by Jamie, Kiri, Crementif and the preset authors.
// Background, license and the upstream revision are recorded in docs/user/reshade-presets.md.
// The order is part of reshade_preset in longshot_options.ini (fx_builtin.h first_clarity_preset): append new entries.
inline constexpr auto clarity_presets = std::to_array<ClarityPreset>({
    {"Serfrost (Recommended)", "Exposure=1.06\nLevels=5,232\nGamma=0.91,0.91,0.91\nGain=1.06,1.06,1.06\n"
                              "Vibrance=0.45\nSaturation=0.95\nFilm=0.22\nSharpness=0.45\n"},
    {"Serfrost (Dim Display)", "Exposure=1.13\nLevels=1,235\nGamma=0.99,0.99,0.99\nGain=1.06,1.06,1.06\n"
                              "Vibrance=0.45\nSaturation=1.15\nFilm=0.15\n"},
    {"Serfrost (Bright Display)", "Levels=2,300\nGamma=0.88,0.88,0.88\nGain=1.1,1.1,1.1\n"
                                 "Vibrance=0.15\nSaturation=1.10\nFilm=0.15\n"},
    {"Bruz", "Exposure=1.08\nGamma=0.9,0.9,0.9\nVibrance=0.15\nSaturation=1.12\nFilm=0.30\n"},
    {"t3nka Neutral+", "Exposure=1.08\nVibrance=0.15\nFilm=0.06\n"},
    {"Jamie", "Exposure=1.08\nLift=0.98,0.98,0.98\nVibrance=0.15\nSaturation=1.10\nFilm=0.14\nSharpness=0.35\n"},
    {"Contrasty", "Exposure=1.10\nGamma=0.88,0.88,0.88\nContrast=0.65\nVibrance=0.15\n"},
    {"The Complaining Gamer", "Gamma=0.78,0.78,0.78\nSaturation=0.72\nVibrance=0.15\nContrast=0.40\nSharpness=0.25\n"},
    {"Filmic", "Exposure=1.08\nContrast=0.42\nSaturation=0.90\nVibrance=0.15\nFilm=0.36\nSharpness=0.35\n"},
    {"CellHunter", "Gamma=1.00,0.965,1.01\nGain=1.00,0.975,1.01\nSaturation=0.80\n"
                   "Vibrance=0.335\nContrast=0.115\nFilm=0.17\n"},
    {"ViskClarity", "Exposure=0.92\nLevels=2,285\nGamma=1.0,0.99,1.0\nGain=1.0,0.99,1.0\n"
                    "Vibrance=0.50\nContrast=-0.35\nFilm=0.22\n"},
    {"Survival of the Wild (Warmer)", "Gamma=0.95,0.95,0.95\nGain=1.025,1.00,0.96\nVibrance=0.90\nSaturation=1.07\nSharpness=0.08\n"},
    {"Survival of the Wild (Colder)", "Gamma=0.93,0.93,0.93\nGain=0.96,1.00,1.04\nVibrance=0.65\n"
                                     "Contrast=0.10\nSaturation=1.10\nSharpness=0.05\n"},
    {"Godot's Mexican", "Exposure=0.87\nLevels=5,205\nGain=1.04,1.00,0.96\nContrast=0.72\nFilm=0.22\nSharpness=0.23\n"},
    {"Hexaae's Clear", "Levels=0,277\nGamma=0.96,0.96,0.96\nSaturation=0.994\n"},
    {"MaranBr (Little More Intense)", "Exposure=1.10\nVibrance=0.20\nContrast=0.32\nFilm=0.12\n"},
    {"YeMaoWuXin (Bright Colors)", "Gamma=0.95,0.95,0.95\nGain=1.075,1.075,1.125\nVibrance=0.90\n"},
    {"YeMaoWuXin (Vivid and Clear)", "Gamma=0.97,0.96,0.98\nGain=1.05,1.00,1.05\nVibrance=0.80\n"},
    {"Cobalt (Saturated & Bright)", "Exposure=1.10\nSaturation=1.17\nContrast=0.74\n"},
    {"Clarity Base", "Saturation=1.18\n"},
});

inline constexpr const char* clarity_fx = R"FX(
texture BackBuffer : COLOR;
sampler BackBufferSampler { Texture = BackBuffer; };
uniform float Exposure = 1.0;
uniform float2 Levels = float2(0.0, 255.0);
uniform float3 Lift = float3(1.0, 1.0, 1.0);
uniform float3 Gamma = float3(1.0, 1.0, 1.0);
uniform float3 Gain = float3(1.0, 1.0, 1.0);
uniform float Contrast = 0.0;
uniform float Saturation = 1.0;
uniform float Vibrance = 0.0;
uniform float Film = 0.0;
uniform float Sharpness = 0.0;

void Fullscreen(uint id : SV_VertexID, out float4 position : SV_Position, out float2 uv : TEXCOORD0)
{
    uv = float2((id << 1) & 2, id & 2);
    position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 Grade(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    const float3 weights = float3(0.2126, 0.7152, 0.0722);
    float4 scene = tex2D(BackBufferSampler, uv);
    float3 rgb = GradeInput(scene.rgb);
    if (Sharpness > 0.0) {
        const float2 texel = float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT);
        float3 average = GradeInput(tex2D(BackBufferSampler, uv + float2(texel.x, 0.0)).rgb);
        average += GradeInput(tex2D(BackBufferSampler, uv - float2(texel.x, 0.0)).rgb);
        average += GradeInput(tex2D(BackBufferSampler, uv + float2(0.0, texel.y)).rgb);
        average += GradeInput(tex2D(BackBufferSampler, uv - float2(0.0, texel.y)).rgb);
        rgb += clamp(dot(rgb - 0.25 * average, weights) * Sharpness, -0.045, 0.045);
    }

    // Exposure in display space with a shoulder that keeps white at white.
    rgb = saturate(rgb);
    rgb = rgb * Exposure / (1.0 + (Exposure - 1.0) * rgb);
    rgb = saturate((255.0 * rgb - Levels.x) / (Levels.y - Levels.x));
    rgb = saturate((rgb + 0.5 * (Lift - 1.0) * (1.0 - rgb)) * Gain);
    rgb = pow(rgb, 1.0 / Gamma);

    float luma = dot(rgb, weights);
    float centered = luma - 0.5;
    float contrastLuma = 0.5 + centered / (0.5 + abs(centered));
    rgb += (contrastLuma - luma) * Contrast;
    float filmLuma = luma * luma * (3.0 - 2.0 * luma);
    rgb += (filmLuma - luma) * Film;
    luma = dot(rgb, weights);
    float spread = max(rgb.r, max(rgb.g, rgb.b)) - min(rgb.r, min(rgb.g, rgb.b));
    float3 grade = saturate(lerp(luma.xxx, rgb, Saturation * (1.0 + Vibrance * (1.0 - saturate(spread)))));
    scene.rgb = GradeOutput(grade, scene.rgb);
    return scene;
}

technique LongshotClarity
{
    pass { VertexShader = Fullscreen; PixelShader = Grade; }
}
)FX";

}  // namespace mkwvk::reshade
