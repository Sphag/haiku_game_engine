// HDR post-process passes (US-039): TAA resolve with motion-vector history, a separable bloom chain, and the exposure, ACES, sRGB, and
// reference-overlay composite. The formulas match engine/render/src/postprocess.cpp, which is the CPU reference the GPU suites compare against.
// Every pass is a compute entry over 8 x 8 pixel groups. All passes share one descriptor layout (post_processor.cpp):
//   t0  Params       the parameter buffer (layout below)
//   t1  SceneColor   linear HDR color: the scene color for TAA, the resolved frame for bloom and the composite
//   t2  Velocity     motion vectors in UV units (x right, y down), as scene.hlsl writes them
//   t3  HistoryIn    the previous resolved frame
//   t4  BloomIn      the blur input of the vertical pass and the composite's bloom
//   t5  Overlay      the reference overlay, straight alpha, display space
//   u6  HdrOut       linear HDR output: TAA's history, and the two bloom blur targets (rgba16f)
//   u7  DisplayOut   the composite, display sRGB with the overlay applied (rgba8)
// Sanitizing: NaN and Inf become zero and negatives clamp to zero before any filtering, as sanitize_color does on the CPU.

Texture2D<float4> SceneColor : register(t1);
Texture2D<float2> Velocity : register(t2);
Texture2D<float4> HistoryIn : register(t3);
Texture2D<float4> BloomIn : register(t4);
Texture2D<float4> Overlay : register(t5);
ByteAddressBuffer Params : register(t0);

// The Vulkan image formats of the storage textures. DXC infers rgba32f from the element type otherwise, which does not match the views.
// DXIL needs no format, and its compiler warns about the attribute, so it is compiled for SPIR-V only.
#ifdef __spirv__
[[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> HdrOut : register(u6);
#ifdef __spirv__
[[vk::image_format("rgba8")]]
#endif
RWTexture2D<float4> DisplayOut : register(u7);

// Parameter buffer, in bytes. The CPU writes it each frame (post_processor.cpp).
static const uint kParamExposure = 0;
static const uint kParamBloomIntensity = 4;
static const uint kParamBloomThreshold = 8;
static const uint kParamBloomKnee = 12;
static const uint kParamTaaWeight = 16;
static const uint kParamHistoryValid = 20;
static const uint kParamWidth = 24;
static const uint kParamHeight = 28;
static const uint kParamWeights = 32;  // kBlurTaps floats: the Gaussian weights, from -radius to +radius
static const int kBlurRadius = 4;

float ParamFloat(uint offset)
{
    return asfloat(Params.Load(offset));
}

uint ParamUint(uint offset)
{
    return Params.Load(offset);
}

float BlurWeight(int tap)
{
    return ParamFloat(kParamWeights + 4 * (uint)(tap + kBlurRadius));
}

int2 FrameSize()
{
    return int2(ParamUint(kParamWidth), ParamUint(kParamHeight));
}

float3 Sanitize(float3 c)
{
    float3 finite = float3(isfinite(c.x) ? c.x : 0.0, isfinite(c.y) ? c.y : 0.0, isfinite(c.z) ? c.z : 0.0);
    return max(finite, float3(0.0, 0.0, 0.0));
}

// Matches bloom_prefilter: the soft knee around the threshold, with the full excess above it.
float3 BloomPrefilter(float3 color, float threshold, float knee)
{
    float3 clean = Sanitize(color);
    float brightness = max(clean.x, max(clean.y, clean.z));
    float t = max(threshold, 0.0);
    float k = max(knee, 0.0);
    float soft = clamp(brightness - t + k, 0.0, 2.0 * k);
    soft = soft * soft / (4.0 * k + 1e-4);
    float contribution = max(soft, brightness - t) / max(brightness, 1e-4);
    return clean * max(contribution, 0.0);
}

float AcesChannel(float x)
{
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

float3 SrgbEncode(float3 linear_color)
{
    float3 x = saturate(Sanitize(linear_color));
    float3 low = 12.92 * x;
    float3 high = 1.055 * pow(x, float3(1.0 / 2.4, 1.0 / 2.4, 1.0 / 2.4)) - 0.055;
    return float3(x.x <= 0.0031308 ? low.x : high.x, x.y <= 0.0031308 ? low.y : high.y, x.z <= 0.0031308 ? low.z : high.z);
}

// Temporal resolve (CPU: TaaHistory::resolve). The history is sampled at the pixel's previous position, clamped to the color box of the
// current 3 x 3 neighborhood, and blended with the current sample. A reset frame (history_valid = 0) and an off-screen or non-finite
// motion keep only the current sample.
[numthreads(8, 8, 1)]
void CSTaa(uint3 id : SV_DispatchThreadID)
{
    int2 size = FrameSize();
    int2 p = int2(id.xy);
    if (p.x >= size.x || p.y >= size.y)
    {
        return;
    }
    float3 now = Sanitize(SceneColor.Load(int3(p, 0)).rgb);
    float3 box_min = now;
    float3 box_max = now;
    for (int dy = -1; dy <= 1; ++dy)
    {
        for (int dx = -1; dx <= 1; ++dx)
        {
            int2 q = clamp(p + int2(dx, dy), int2(0, 0), size - int2(1, 1));
            float3 c = Sanitize(SceneColor.Load(int3(q, 0)).rgb);
            box_min = min(box_min, c);
            box_max = max(box_max, c);
        }
    }
    float3 result = now;
    if (ParamUint(kParamHistoryValid) != 0)
    {
        float2 motion = Velocity.Load(int3(p, 0)) * float2(size);
        if (isfinite(motion.x) && isfinite(motion.y))
        {
            float2 previous = float2(p) + 0.5 - motion;
            if (previous.x >= 0.0 && previous.y >= 0.0 && previous.x < (float)size.x && previous.y < (float)size.y)
            {
                int2 at = int2(floor(previous));
                float3 past = HistoryIn.Load(int3(at, 0)).rgb;
                result = lerp(clamp(past, box_min, box_max), now, ParamFloat(kParamTaaWeight));
            }
        }
    }
    HdrOut[p] = float4(result, 1.0);
}

// Horizontal bloom: the prefiltered color, blurred along x. The vertical pass blurs the result along y.
[numthreads(8, 8, 1)]
void CSBloomHorizontal(uint3 id : SV_DispatchThreadID)
{
    int2 size = FrameSize();
    int2 p = int2(id.xy);
    if (p.x >= size.x || p.y >= size.y)
    {
        return;
    }
    float threshold = ParamFloat(kParamBloomThreshold);
    float knee = ParamFloat(kParamBloomKnee);
    float3 sum = float3(0.0, 0.0, 0.0);
    for (int i = -kBlurRadius; i <= kBlurRadius; ++i)
    {
        int2 q = clamp(p + int2(i, 0), int2(0, 0), size - int2(1, 1));
        sum += BlurWeight(i) * BloomPrefilter(SceneColor.Load(int3(q, 0)).rgb, threshold, knee);
    }
    HdrOut[p] = float4(sum, 1.0);
}

[numthreads(8, 8, 1)]
void CSBloomVertical(uint3 id : SV_DispatchThreadID)
{
    int2 size = FrameSize();
    int2 p = int2(id.xy);
    if (p.x >= size.x || p.y >= size.y)
    {
        return;
    }
    float3 sum = float3(0.0, 0.0, 0.0);
    for (int j = -kBlurRadius; j <= kBlurRadius; ++j)
    {
        int2 q = clamp(p + int2(0, j), int2(0, 0), size - int2(1, 1));
        sum += BlurWeight(j) * Sanitize(BloomIn.Load(int3(q, 0)).rgb);
    }
    HdrOut[p] = float4(sum, 1.0);
}

// Composite: bloom is added in linear light, the exposure scales the sum, ACES maps it to display range, and the result is encoded to sRGB.
// The reference overlay blends over the encoded color last, so it never sees the scene's exposure.
[numthreads(8, 8, 1)]
void CSTonemap(uint3 id : SV_DispatchThreadID)
{
    int2 size = FrameSize();
    int2 p = int2(id.xy);
    if (p.x >= size.x || p.y >= size.y)
    {
        return;
    }
    float exposure = max(ParamFloat(kParamExposure), 0.0);
    float intensity = max(ParamFloat(kParamBloomIntensity), 0.0);
    float3 scene = Sanitize(SceneColor.Load(int3(p, 0)).rgb);
    float3 bloom = Sanitize(BloomIn.Load(int3(p, 0)).rgb);
    float3 exposed = (scene + bloom * intensity) * exposure;
    float3 mapped = float3(AcesChannel(exposed.x), AcesChannel(exposed.y), AcesChannel(exposed.z));
    float3 display = SrgbEncode(mapped);
    float4 overlay = Overlay.Load(int3(p, 0));
    float alpha = saturate(overlay.a);
    display = Sanitize(display) * (1.0 - alpha) + Sanitize(overlay.rgb) * alpha;
    DisplayOut[p] = float4(display, 1.0);
}
