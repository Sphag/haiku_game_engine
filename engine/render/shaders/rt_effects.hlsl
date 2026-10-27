// Ray-traced shadows and single-bounce reflections (US-043): the compute pass that RtEffects (engine/render/src/rt_effects.cpp) dispatches. It
// traces the camera's primary rays with inline ray queries, then writes the directional shadow and the one-bounce reflection. The CPU
// reference, trace_reference in rt_effects.cpp, runs the same steps. Keep the two in step.
//
// Descriptor set 0 (the binding is the register number):
//   t0  Params      the frame's parameters (layout below)
//   t1  Scene       the top-level acceleration structure
//   t2  Instances   one 64-byte record per instance (layout below), indexed by InstanceID
//   t3  Triangles   one 64-byte record per triangle, indexed by instance triangle base + PrimitiveIndex
//   t4  MaskAtlas   the alpha-mask atlas; alpha is in .a
//   u5  Shadow      RGBA16F: x = 1 where the primary surface sees the sun
//   u6  Reflection  RGBA16F: rgb = the reflected radiance weighted by Fresnel, a = 1 where a reflection hit
//   u7  Depth       R32F: the linear view depth of the primary hit; kMissDepth where the primary ray hits nothing (US-045)
//   u8  Normal      RGBA16F: xyz = the primary hit's world normal toward the camera; (0, 1, 0) where the ray misses
//   u9  Motion      RGBA16F: xy = the UV offset to the same point in the previous frame (previous uv = uv + motion); zero where the ray misses
//
// Parameters, in bytes (float unless noted). The vectors use three floats and a pad:
//   0 eye, 16 right (already scaled by tan(fov/2) * aspect), 32 up (scaled by tan(fov/2)), 48 forward,
//   64 sun direction (toward the sun), 80 sun radiance, 96 ambient,
//   112 width (uint), 116 height (uint), 120 flags (uint: bit 0 shadows, bit 1 reflections),
//   124 roughness cutoff, 128 reflection distance, 132 shadow distance, 136 bias (meters along the normal),
//   144 previous eye, 160 previous forward, 176 previous right (scaled as right), 192 previous up (scaled as up).
//
// Instance record, 64 bytes: 0 albedo (3 floats), 12 metallic, 16 roughness, 20 triangle base (uint), 24 masked (uint, 1 when the mask
// applies), 28 mask cutoff, 32 mask rectangle (offset x, y and size z, w in UV units).
//
// Triangle record, 64 bytes: 0, 12, 24 the object-space positions of vertices 0, 1, 2 (3 floats each); 36, 44, 52 their UVs (2 floats each).
//
// Barycentrics follow DXR: the point is (1 - b.x - b.y) * v0 + b.x * v1 + b.y * v2, which the UV and position interpolation use too.

#ifdef __spirv__
[[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> Shadow : register(u5, space0);
#ifdef __spirv__
[[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> Reflection : register(u6, space0);
#ifdef __spirv__
[[vk::image_format("r32f")]]
#endif
RWTexture2D<float> Depth : register(u7, space0);
#ifdef __spirv__
[[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> Normal : register(u8, space0);
#ifdef __spirv__
[[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> Motion : register(u9, space0);

ByteAddressBuffer Params : register(t0, space0);
RaytracingAccelerationStructure Scene : register(t1, space0);
ByteAddressBuffer Instances : register(t2, space0);
ByteAddressBuffer Triangles : register(t3, space0);
Texture2D<float4> MaskAtlas : register(t4, space0);

static const uint kParamEye = 0;
static const uint kParamRight = 16;
static const uint kParamUp = 32;
static const uint kParamForward = 48;
static const uint kParamSunDirection = 64;
static const uint kParamSunRadiance = 80;
static const uint kParamAmbient = 96;
static const uint kParamSize = 112;
static const uint kParamFlags = 120;
static const uint kParamRoughnessCutoff = 124;
static const uint kParamReflectionDistance = 128;
static const uint kParamShadowDistance = 132;
static const uint kParamBias = 136;
static const uint kParamPreviousEye = 144;
static const uint kParamPreviousForward = 160;
static const uint kParamPreviousRight = 176;
static const uint kParamPreviousUp = 192;

// Depth of a primary ray that misses. Matches kRtMissDepth in rt_effects.hpp.
static const float kMissDepth = 1.0e6;

static const uint kFlagShadows = 1;
static const uint kFlagReflections = 2;

static const uint kInstanceStride = 64;
static const uint kTriangleStride = 64;

// Dielectric reflectance at normal incidence, as the caller's PBR shading uses.
static const float3 kDielectricF0 = float3(0.04, 0.04, 0.04);

struct Hit {
    float t;
    uint instance;
    uint primitive;
    float2 barycentrics;
    float3x4 object_to_world;
};

struct Surface {
    float3 position;
    float3 normal;   // the geometric normal, turned toward the ray that found the surface
    float3 albedo;
    float metallic;
    float roughness;
};

// The alpha test of a masked candidate: the UV at its barycentrics, sampled from the atlas inside the instance's rectangle.
// Matches masked_candidate_passes in rt_effects.cpp.
bool AlphaPasses(uint instance, uint primitive, float2 barycentrics)
{
    uint record = instance * kInstanceStride;
    if (Instances.Load(record + 24) == 0)
    {
        return true;
    }
    uint base = (Instances.Load(record + 20) + primitive) * kTriangleStride;
    float2 uv0 = asfloat(Triangles.Load2(base + 36));
    float2 uv1 = asfloat(Triangles.Load2(base + 44));
    float2 uv2 = asfloat(Triangles.Load2(base + 52));
    float2 uv = (1.0 - barycentrics.x - barycentrics.y) * uv0 + barycentrics.x * uv1 + barycentrics.y * uv2;

    float4 rect = asfloat(Instances.Load4(record + 32));
    uint width;
    uint height;
    MaskAtlas.GetDimensions(width, height);
    float2 texel = (rect.xy + saturate(uv) * rect.zw) * float2(width, height);
    int2 index = clamp(int2(floor(texel)), int2(0, 0), int2(width, height) - 1);
    return MaskAtlas.Load(int3(index, 0)).a >= asfloat(Instances.Load(record + 28));
}

// Traces one ray. With any_hit the traversal ends at the first accepted hit (a shadow); otherwise it finds the closest one.
bool Trace(float3 origin, float3 direction, float t_max, bool any_hit, out Hit hit)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.0;
    ray.TMax = t_max;

    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(Scene, any_hit ? RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH : RAY_FLAG_NONE, 0xFF, ray);
    while (query.Proceed())
    {
        if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            AlphaPasses(query.CandidateInstanceID(), query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics()))
        {
            query.CommitNonOpaqueTriangleHit();
        }
    }

    hit.t = 0.0;
    hit.instance = 0;
    hit.primitive = 0;
    hit.barycentrics = float2(0.0, 0.0);
    hit.object_to_world = float3x4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0);
    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    {
        return false;
    }
    hit.t = query.CommittedRayT();
    hit.instance = query.CommittedInstanceID();
    hit.primitive = query.CommittedPrimitiveIndex();
    hit.barycentrics = query.CommittedTriangleBarycentrics();
    hit.object_to_world = query.CommittedObjectToWorld3x4();
    return true;
}

// The surface at a hit: the world positions of its triangle, the normal turned toward the incoming ray, and the material.
Surface SurfaceAt(Hit hit, float3 direction)
{
    uint record = hit.instance * kInstanceStride;
    uint base = (Instances.Load(record + 20) + hit.primitive) * kTriangleStride;
    float3 w0 = mul(hit.object_to_world, float4(asfloat(Triangles.Load3(base)), 1.0));
    float3 w1 = mul(hit.object_to_world, float4(asfloat(Triangles.Load3(base + 12)), 1.0));
    float3 w2 = mul(hit.object_to_world, float4(asfloat(Triangles.Load3(base + 24)), 1.0));

    Surface surface;
    surface.position = w0 + hit.barycentrics.x * (w1 - w0) + hit.barycentrics.y * (w2 - w0);
    float3 normal = normalize(cross(w1 - w0, w2 - w0));
    surface.normal = dot(normal, direction) > 0.0 ? -normal : normal;
    surface.albedo = asfloat(Instances.Load3(record));
    surface.metallic = asfloat(Instances.Load(record + 12));
    surface.roughness = asfloat(Instances.Load(record + 16));
    return surface;
}

// The UV of a world point in a view (eye, forward, and the scaled right and up axes), and its linear depth along the forward axis.
// The right and up axes are scaled by tan(fov / 2) and the aspect, so the NDC coordinate is the projection onto each axis over the depth.
float2 ViewUv(float3 world, float3 eye, float3 forward, float3 right, float3 up, out float depth)
{
    float3 offset = world - eye;
    depth = dot(offset, forward);
    float ndc_x = dot(offset, right) / (depth * dot(right, right));
    float ndc_y = dot(offset, up) / (depth * dot(up, up));
    return float2(ndc_x * 0.5 + 0.5, 0.5 - ndc_y * 0.5);
}

// Whether the sun is blocked from a surface, by a shadow ray that starts one bias along the normal.
bool SunBlocked(float3 position, float3 normal, float3 sun, float distance, float bias)
{
    Hit hit;
    return Trace(position + normal * bias, sun, distance, true, hit);
}

[numthreads(8, 8, 1)]
void CSRayTracedEffects(uint3 id : SV_DispatchThreadID)
{
    uint width = Params.Load(kParamSize);
    uint height = Params.Load(kParamSize + 4);
    if (id.x >= width || id.y >= height)
    {
        return;
    }

    float3 eye = asfloat(Params.Load3(kParamEye));
    float3 right = asfloat(Params.Load3(kParamRight));
    float3 up = asfloat(Params.Load3(kParamUp));
    float3 forward = asfloat(Params.Load3(kParamForward));
    float3 sun = asfloat(Params.Load3(kParamSunDirection));
    float3 sun_radiance = asfloat(Params.Load3(kParamSunRadiance));
    float3 ambient = asfloat(Params.Load3(kParamAmbient));
    uint flags = Params.Load(kParamFlags);
    float roughness_cutoff = asfloat(Params.Load(kParamRoughnessCutoff));
    float reflection_distance = asfloat(Params.Load(kParamReflectionDistance));
    float shadow_distance = asfloat(Params.Load(kParamShadowDistance));
    float bias = asfloat(Params.Load(kParamBias));
    float3 previous_eye = asfloat(Params.Load3(kParamPreviousEye));
    float3 previous_forward = asfloat(Params.Load3(kParamPreviousForward));
    float3 previous_right = asfloat(Params.Load3(kParamPreviousRight));
    float3 previous_up = asfloat(Params.Load3(kParamPreviousUp));

    float2 pixel = (float2(id.xy) + 0.5) / float2(width, height);
    float3 direction = normalize(forward + right * (pixel.x * 2.0 - 1.0) + up * (1.0 - pixel.y * 2.0));

    // The primary ray is not limited by the camera's range (kRtPrimaryDistance in rt_effects.hpp).
    Hit primary;
    if (!Trace(eye, direction, 1.0e6, false, primary))
    {
        Shadow[id.xy] = float4(1.0, 1.0, 1.0, 1.0);
        Reflection[id.xy] = float4(0.0, 0.0, 0.0, 0.0);
        Depth[id.xy] = kMissDepth;
        Normal[id.xy] = float4(0.0, 1.0, 0.0, 0.0);
        Motion[id.xy] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    Surface surface = SurfaceAt(primary, direction);

    // The geometry for the denoiser: this frame's depth and normal, and the motion of the point between the two cameras.
    float surface_depth;
    float2 uv = ViewUv(surface.position, eye, forward, right, up, surface_depth);
    float previous_depth;
    float2 previous_uv = ViewUv(surface.position, previous_eye, previous_forward, previous_right, previous_up, previous_depth);
    Depth[id.xy] = surface_depth;
    Normal[id.xy] = float4(surface.normal, 0.0);
    Motion[id.xy] = float4(previous_uv - uv, 0.0, 0.0);

    float lit = 1.0;
    if ((flags & kFlagShadows) != 0 && SunBlocked(surface.position, surface.normal, sun, shadow_distance, bias))
    {
        lit = 0.0;
    }
    Shadow[id.xy] = float4(lit, lit, lit, 1.0);

    float4 reflected = float4(0.0, 0.0, 0.0, 0.0);
    if ((flags & kFlagReflections) != 0 && surface.roughness <= roughness_cutoff)
    {
        float3 bounce_direction = reflect(direction, surface.normal);
        Hit bounce;
        if (Trace(surface.position + surface.normal * bias, bounce_direction, reflection_distance, false, bounce))
        {
            Surface hit_surface = SurfaceAt(bounce, bounce_direction);
            float visible = 1.0;
            if ((flags & kFlagShadows) != 0 && SunBlocked(hit_surface.position, hit_surface.normal, sun, shadow_distance, bias))
            {
                visible = 0.0;
            }
            float3 radiance = hit_surface.albedo * (ambient + sun_radiance * (max(dot(hit_surface.normal, sun), 0.0) * visible));

            float3 f0 = lerp(kDielectricF0, surface.albedo, surface.metallic);
            float facing = saturate(dot(surface.normal, -direction));
            float3 fresnel = f0 + (1.0 - f0) * pow(1.0 - facing, 5.0);
            reflected = float4(radiance * fresnel, 1.0);
        }
    }
    Reflection[id.xy] = reflected;
}
