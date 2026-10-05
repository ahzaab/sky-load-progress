#pragma once

namespace load_progress
{
    inline constexpr const char* FrozenFramePixelShader = R"(
Texture2D frozenTexture : register(t0);
SamplerState frozenSampler : register(s0);

float4 main(float4 color : COLOR0, float2 textureCoordinate : TEXCOORD0) : SV_Target
{
    uint width;
    uint height;
    frozenTexture.GetDimensions(width, height);
    float2 texel = float2($BLUR_AMOUNT$ / width, $BLUR_AMOUNT$ / height);

    float4 pixel = frozenTexture.Sample(frozenSampler, textureCoordinate) * 0.227027;
    pixel += frozenTexture.Sample(frozenSampler, textureCoordinate + float2(1.384615, 0.0) * texel) * 0.158108;
    pixel += frozenTexture.Sample(frozenSampler, textureCoordinate - float2(1.384615, 0.0) * texel) * 0.158108;
    pixel += frozenTexture.Sample(frozenSampler, textureCoordinate + float2(0.0, 1.384615) * texel) * 0.158108;
    pixel += frozenTexture.Sample(frozenSampler, textureCoordinate - float2(0.0, 1.384615) * texel) * 0.158108;
    pixel += frozenTexture.Sample(frozenSampler, textureCoordinate + float2(3.230769, 3.230769) * texel) * 0.035880;
    pixel += frozenTexture.Sample(frozenSampler, textureCoordinate + float2(3.230769, -3.230769) * texel) * 0.035880;
    pixel += frozenTexture.Sample(frozenSampler, textureCoordinate + float2(-3.230769, 3.230769) * texel) * 0.035880;
    pixel += frozenTexture.Sample(frozenSampler, textureCoordinate - float2(3.230769, 3.230769) * texel) * 0.035880;
    // Scene alpha is renderer metadata, not the opacity of our retained photograph.
    pixel.rgb /= 1.002979;
    return float4(pixel.rgb * color.rgb, color.a);
}
)";
}
