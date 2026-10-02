min16float4 main(ShaderInput input) : SV_TARGET
{
#ifdef CATMULL_ROM
    // Filtering commutes with the linear YUV to RGB conversion below, so filter in YUV space.
    min16float3 yuv = swizzle((min16float3)sampleCatmullRom3(videoTex, theSampler, input.tex, chromaTexMax));
#else
    min16float3 yuv = swizzle(videoTex.Sample(theSampler, input.tex));
#endif

    // Subtract the YUV offset for limited vs full range
    yuv -= offsets;

    // Multiply by the conversion matrix for this colorspace
    yuv = mul(yuv, cscMatrix);

    return min16float4(yuv, 1.0);
}