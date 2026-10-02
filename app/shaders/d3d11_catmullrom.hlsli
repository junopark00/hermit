// Catmull-Rom (bicubic, B = 0, C = 0.5) sampling in 9 bilinear taps instead of 16 point taps.
// See "An HLSL Function for Sampling a 2D Texture With Catmull-Rom Filtering" (M. Pettineo).
//
// Used when the video is drawn at a size other than its native resolution (for example in a
// window). A single bilinear tap per output pixel blurs some glyph strokes into two pixels and
// leaves others sharp depending on the sub-pixel phase, which makes text look uneven; this
// filter keeps stroke weight much more consistent.
//
// Tap coordinates are clamped to [half a texel, maxUV] so the wide kernel never reads the
// texture's alignment padding at the right or bottom edge. All coordinate math uses full
// precision floats: min16float is not precise enough to address 2560+ texel wide textures.

struct CatmullRomTaps
{
    float2 pos0;   // coordinates of the 3 x 3 bilinear taps
    float2 pos12;
    float2 pos3;
    float2 w0;     // weights
    float2 w12;
    float2 w3;
};

CatmullRomTaps computeCatmullRomTaps(float2 uv, float2 texSize, float2 maxUV)
{
    CatmullRomTaps t;
    float2 samplePos = uv * texSize;
    float2 texPos1 = floor(samplePos - 0.5) + 0.5;
    float2 f = samplePos - texPos1;

    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);

    // The middle two taps are merged into one bilinear tap placed between them.
    t.w0 = w0;
    t.w12 = w1 + w2;
    t.w3 = w3;

    float2 minUV = 0.5 / texSize;
    t.pos0 = clamp((texPos1 - 1.0) / texSize, minUV, maxUV);
    t.pos12 = clamp((texPos1 + w2 / t.w12) / texSize, minUV, maxUV);
    t.pos3 = clamp((texPos1 + 2.0) / texSize, minUV, maxUV);
    return t;
}

float3 sampleCatmullRom3(Texture2D<min16float3> tex, SamplerState s, float2 uv, float2 maxUV)
{
    float2 texSize;
    tex.GetDimensions(texSize.x, texSize.y);
    CatmullRomTaps t = computeCatmullRomTaps(uv, texSize, maxUV);

    float3 result = 0;
    result += (float3)tex.SampleLevel(s, float2(t.pos0.x,  t.pos0.y),  0) * (t.w0.x  * t.w0.y);
    result += (float3)tex.SampleLevel(s, float2(t.pos12.x, t.pos0.y),  0) * (t.w12.x * t.w0.y);
    result += (float3)tex.SampleLevel(s, float2(t.pos3.x,  t.pos0.y),  0) * (t.w3.x  * t.w0.y);
    result += (float3)tex.SampleLevel(s, float2(t.pos0.x,  t.pos12.y), 0) * (t.w0.x  * t.w12.y);
    result += (float3)tex.SampleLevel(s, float2(t.pos12.x, t.pos12.y), 0) * (t.w12.x * t.w12.y);
    result += (float3)tex.SampleLevel(s, float2(t.pos3.x,  t.pos12.y), 0) * (t.w3.x  * t.w12.y);
    result += (float3)tex.SampleLevel(s, float2(t.pos0.x,  t.pos3.y),  0) * (t.w0.x  * t.w3.y);
    result += (float3)tex.SampleLevel(s, float2(t.pos12.x, t.pos3.y),  0) * (t.w12.x * t.w3.y);
    result += (float3)tex.SampleLevel(s, float2(t.pos3.x,  t.pos3.y),  0) * (t.w3.x  * t.w3.y);
    return result;
}

float sampleCatmullRom1(Texture2D<min16float> tex, SamplerState s, float2 uv, float2 maxUV)
{
    float2 texSize;
    tex.GetDimensions(texSize.x, texSize.y);
    CatmullRomTaps t = computeCatmullRomTaps(uv, texSize, maxUV);

    float result = 0;
    result += (float)tex.SampleLevel(s, float2(t.pos0.x,  t.pos0.y),  0) * (t.w0.x  * t.w0.y);
    result += (float)tex.SampleLevel(s, float2(t.pos12.x, t.pos0.y),  0) * (t.w12.x * t.w0.y);
    result += (float)tex.SampleLevel(s, float2(t.pos3.x,  t.pos0.y),  0) * (t.w3.x  * t.w0.y);
    result += (float)tex.SampleLevel(s, float2(t.pos0.x,  t.pos12.y), 0) * (t.w0.x  * t.w12.y);
    result += (float)tex.SampleLevel(s, float2(t.pos12.x, t.pos12.y), 0) * (t.w12.x * t.w12.y);
    result += (float)tex.SampleLevel(s, float2(t.pos3.x,  t.pos12.y), 0) * (t.w3.x  * t.w12.y);
    result += (float)tex.SampleLevel(s, float2(t.pos0.x,  t.pos3.y),  0) * (t.w0.x  * t.w3.y);
    result += (float)tex.SampleLevel(s, float2(t.pos12.x, t.pos3.y),  0) * (t.w12.x * t.w3.y);
    result += (float)tex.SampleLevel(s, float2(t.pos3.x,  t.pos3.y),  0) * (t.w3.x  * t.w3.y);
    return result;
}
