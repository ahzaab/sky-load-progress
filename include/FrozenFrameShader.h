#pragma once

namespace load_progress
{
    // Pixel-shader inputs come from SpriteBatch: COLOR0 carries tint/opacity, TEXCOORD0
    // is a normalized texture coordinate, and SV_Target is the output render-target color.
    // t0 binds the retained texture and s0 its sampler. Dividing the blur radius by texture
    // size converts a pixel distance into normalized coordinates; LinearClamp interpolates
    // samples and prevents edge taps from wrapping to the opposite side of the image.
    // The weighted center/axis/diagonal taps approximate a blur in one fullscreen pass.
    // The weights sum to 1.002979, so dividing RGB by that sum preserves brightness.
    // At radius zero all taps sample the same location. The shader still supplies explicit
    // photograph opacity, independent of alpha metadata left by the original world renderer.
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
