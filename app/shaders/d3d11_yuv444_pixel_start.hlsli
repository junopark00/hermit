Texture2D<min16float3> videoTex : register(t0);
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
    // Full precision: chromaTexMax bounds the scaled sampling taps, and min16float cannot address
    // single texels of a wide texture. Same 32-bit cbuffer layout as min16float.
    float2 chromaOffset; // Unused for 4:4:4
    float2 chromaTexMax; // Max valid texcoord (excludes alignment padding)
#else
    min16float2 chromaOffset; // Unused for 4:4:4
    min16float2 chromaTexMax; // Unused for 4:4:4
#endif
};

#ifdef CATMULL_ROM
#include "d3d11_catmullrom.hlsli"
#endif