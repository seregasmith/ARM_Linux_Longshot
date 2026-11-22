texture Scene : COLOR;
texture HighlightMask {
    Width = BUFFER_WIDTH / 2;
    Height = BUFFER_HEIGHT / 2;
    Format = RGBA16F;
};

sampler SceneSampler { Texture = Scene; };
sampler MaskSampler { Texture = HighlightMask; };

void Fullscreen(uint id : SV_VertexID, out float4 position : SV_Position, out float2 uv : TEXCOORD0)
{
    uv = float2((id << 1) & 2, id & 2);
    position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 MakeMask(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float3 rgb = tex2D(SceneSampler, uv).rgb;
    float luma = dot(rgb, float3(0.2126, 0.7152, 0.0722));
    return float4(smoothstep(0.32, 0.9, luma).xxx, 1.0);
}

float4 MakeTint(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    return float4(float3(0.20, 0.07, 0.01) * tex2D(MaskSampler, uv).r, 0.0);
}

technique TintedHighlights
{
    pass Extract {
        VertexShader = Fullscreen;
        PixelShader = MakeMask;
        RenderTarget = HighlightMask;
    }
    pass Apply {
        VertexShader = Fullscreen;
        PixelShader = MakeTint;
        BlendEnable = true;
        SrcBlend = ONE;
        DestBlend = ONE;
        ColorWriteMask = 7;
    }
}
