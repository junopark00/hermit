Texture2D<min16float> luminancePlane : register(t0);
Texture2D<min16float2> chrominancePlane : register(t1);
SamplerState theSampler : register(s0);

struct ShaderInput
{
    float4 pos : SV_POSITION;
    float2 tex : TEXCOORD0;
};

cbuffer CSC_CONST_BUF : register(b0)
{
    min16float3x3 cscMatrix;
    min16float3 offsets;
#ifdef CATMULL_ROM
    // Full precision texcoords; same 32-bit cbuffer layout as min16float.
    float2 chromaOffset;
    float2 chromaTexMax;
#else
    min16float2 chromaOffset;
    min16float2 chromaTexMax;
#endif
};

#ifdef CATMULL_ROM
#include "d3d11_catmullrom.hlsli"
#endif

min16float4 main(ShaderInput input) : SV_TARGET
{
#ifdef CATMULL_ROM
    // Sharper luma when scaled: text detail lives in luma, and chroma is half resolution anyway.
    min16float3 yuv = min16float3((min16float)sampleCatmullRom1(luminancePlane, theSampler, input.tex, chromaTexMax),
                                  chrominancePlane.Sample(theSampler, min(input.tex + chromaOffset, chromaTexMax.rg)));
#else
    // Clamp the chrominance texcoords to avoid sampling the row of texels adjacent to the alignment padding
    min16float3 yuv = min16float3(luminancePlane.Sample(theSampler, input.tex),
                                  chrominancePlane.Sample(theSampler, min(input.tex + chromaOffset, chromaTexMax.rg)));
#endif

    // Subtract the YUV offset for limited vs full range
    yuv -= offsets;

    // Multiply by the conversion matrix for this colorspace
    yuv = mul(yuv, cscMatrix);

    return min16float4(yuv, 1.0);
}