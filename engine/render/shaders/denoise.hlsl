// Temporal and spatial denoiser for the ray-traced signals (US-045): the compute pass that TemporalDenoiser (engine/render/src/denoiser.cpp)
// dispatches. One thread filters one pixel: a depth- and normal-aware box prefilter, then reprojection into the previous history with the
// motion vector, a rejection of the history where its linear depth or normal disagree, and a blend whose weight is never below 1 / (age + 1).
// denoise_reference in denoiser.cpp runs the same steps. Keep the two in step.
//
// Descriptor set 0 (the binding is the register number):
//   t0  Params       the frame's parameters (layout below)
//   t1  Value        the noisy signal, rgba
//   t2  Depth        linear view depth in meters, R32F
//   t3  Normal       the world normal in xyz
//   t4  Motion       the UV offset from this frame to the previous one in xy (previous uv = uv + motion)
//   t5  PrevValue    the previous history's denoised value
//   t6  PrevDepth    the previous history's depth
//   t7  PrevNormal   the previous history's normal in xyz and its age in w
//   u8  NextValue    the next history's denoised value (the output of this frame)
//   u9  NextDepth    the next history's depth
//   u10 NextNormal   the next history's normal and age
//
// Parameters, in bytes: 0 width (uint), 4 height (uint), 8 flags (uint: bit 0 reset, bit 1 the previous history is valid), 12 max age
// (uint), 16 history weight, 20 depth tolerance (relative), 24 normal threshold, 28 spatial radius (uint, 0 to 2).

#ifdef __spirv__
[[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> NextValue : register(u8, space0);
#ifdef __spirv__
[[vk::image_format("r32f")]]
#endif
RWTexture2D<float> NextDepth : register(u9, space0);
#ifdef __spirv__
[[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> NextNormal : register(u10, space0);

ByteAddressBuffer Params : register(t0, space0);
Texture2D<float4> Value : register(t1, space0);
Texture2D<float> Depth : register(t2, space0);
Texture2D<float4> Normal : register(t3, space0);
Texture2D<float4> Motion : register(t4, space0);
Texture2D<float4> PrevValue : register(t5, space0);
Texture2D<float> PrevDepth : register(t6, space0);
Texture2D<float4> PrevNormal : register(t7, space0);

static const uint kParamWidth = 0;
static const uint kParamHeight = 4;
static const uint kParamFlags = 8;
static const uint kParamMaxAge = 12;
static const uint kParamHistoryWeight = 16;
static const uint kParamDepthTolerance = 20;
static const uint kParamNormalThreshold = 24;
static const uint kParamSpatialRadius = 28;

static const uint kFlagReset = 1;
static const uint kFlagHistoryValid = 2;

static const uint kThreads = 8;

// A non-finite sample contributes nothing, as the CPU reference does.
float4 Sanitized(float4 value)
{
    return all(isfinite(value)) ? value : float4(0, 0, 0, 0);
}

// Whether a sample's geometry agrees with the pixel's: the linear depth within the relative tolerance, and the normals close.
bool GeometryAgrees(float depth_sample, float3 normal_sample, float depth, float3 normal, float tolerance, float threshold)
{
    return abs(depth_sample - depth) <= tolerance * depth && dot(normal_sample, normal) >= threshold;
}

[numthreads(kThreads, kThreads, 1)]
void CSTemporalDenoise(uint3 id : SV_DispatchThreadID)
{
    uint width = Params.Load(kParamWidth);
    uint height = Params.Load(kParamHeight);
    if (id.x >= width || id.y >= height)
    {
        return;
    }

    uint flags = Params.Load(kParamFlags);
    uint max_age = Params.Load(kParamMaxAge);
    float history_weight = asfloat(Params.Load(kParamHistoryWeight));
    float tolerance = asfloat(Params.Load(kParamDepthTolerance));
    float threshold = asfloat(Params.Load(kParamNormalThreshold));
    int radius = (int)Params.Load(kParamSpatialRadius);
    bool use_history = (flags & kFlagHistoryValid) != 0 && (flags & kFlagReset) == 0;

    int2 pixel = int2(id.xy);
    float depth = Depth.Load(int3(pixel, 0));
    float3 normal = Normal.Load(int3(pixel, 0)).xyz;

    // The depth- and normal-aware box prefilter. The pixel itself always passes.
    float4 sum = float4(0, 0, 0, 0);
    float weight = 0.0;
    for (int dy = -radius; dy <= radius; ++dy)
    {
        for (int dx = -radius; dx <= radius; ++dx)
        {
            int2 q = clamp(pixel + int2(dx, dy), int2(0, 0), int2((int)width - 1, (int)height - 1));
            float depth_q = Depth.Load(int3(q, 0));
            float3 normal_q = Normal.Load(int3(q, 0)).xyz;
            if (!GeometryAgrees(depth_q, normal_q, depth, normal, tolerance, threshold))
            {
                continue;
            }
            sum += Sanitized(Value.Load(int3(q, 0)));
            weight += 1.0;
        }
    }
    float4 filtered = sum / weight;

    // Reprojection into the previous history, and the rejection of the sample where its geometry disagrees.
    float2 uv = (float2(id.xy) + 0.5) / float2(width, height);
    float2 previous_uv = uv + Motion.Load(int3(pixel, 0)).xy;
    int2 previous_pixel = int2(floor(previous_uv * float2(width, height)));
    bool accepted = false;
    float4 previous_value = float4(0, 0, 0, 0);
    float previous_age = 0.0;
    if (use_history && all(previous_pixel >= int2(0, 0)) && all(previous_pixel < int2((int)width, (int)height)))
    {
        float4 prior_value = PrevValue.Load(int3(previous_pixel, 0));
        float prior_depth = PrevDepth.Load(int3(previous_pixel, 0));
        float4 prior_normal = PrevNormal.Load(int3(previous_pixel, 0));
        accepted = all(isfinite(prior_value)) && GeometryAgrees(prior_depth, prior_normal.xyz, depth, normal, tolerance, threshold);
        previous_value = prior_value;
        previous_age = prior_normal.w;
    }

    float4 value = filtered;
    float age = 1.0;
    if (accepted)
    {
        age = min(previous_age + 1.0, (float)max_age);
        float alpha = max(1.0 - history_weight, 1.0 / (age + 1.0));
        value = lerp(previous_value, filtered, alpha);
    }

    NextValue[id.xy] = value;
    NextDepth[id.xy] = depth;
    NextNormal[id.xy] = float4(normal, age);
}
