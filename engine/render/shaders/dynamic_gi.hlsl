// Dynamic ray-traced GI (US-044): the compute pass that DynamicGi (engine/render/src/dynamic_gi.cpp) dispatches. One thread traces one
// planned probe. It casts rays_per_probe directions on a spherical Fibonacci set from the probe's cell centre. A miss brings back the sky. A
// hit brings back the surface's albedo times the sky plus the sun, where a shadow ray decides the sun. The radiance is projected onto nine SH
// coefficients in the diffuse convention of gi/probes.hpp (band weights 1, 2/3, 1/4), and the history blends the projection with the weight
// that the grid planned. Keep this file, sh_basis in probes.cpp, and the byte offsets in dynamic_gi.cpp in step.
//
// Descriptor set 0 (the binding is the register number):
//   t0  Params      the frame's parameters (layout below)
//   t1  Scene       the top-level acceleration structure
//   t2  Instances   one 64-byte record per instance, as rt_effects.hlsl (albedo at 0)
//   t3  Triangles   one 64-byte record per triangle, as rt_effects.hlsl (object-space positions at 0, 12, 24)
//   t4  Updates     one 32-byte record per planned probe (layout below)
//   u5  History     nine float4 per grid slot (144 bytes); the rgb of coefficient i is at 16 * i
//
// Parameters, in bytes (float unless noted): 0 spacing, 4 rays per probe (uint), 8 update count (uint), 12 shadow distance, 16 sun direction
// (three floats, toward the sun), 32 sun radiance, 48 sky radiance, 60 bias (meters along the normal before a shadow ray), 64 the camera's
// world position (three floats). The scene's geometry is camera-relative, so a probe at world (cell + 0.5) * spacing is traced from that
// point minus the camera position.
// Update record, 32 bytes: 0 slot (uint), 4 blend weight, 8 cell (three ints; the probe sits at (cell + 0.5) * spacing).
//
// Every triangle is opaque to these rays (RAY_FLAG_FORCE_OPAQUE), so the alpha masks of rt_effects.hlsl do not apply here.

ByteAddressBuffer Params : register(t0, space0);
RaytracingAccelerationStructure Scene : register(t1, space0);
ByteAddressBuffer Instances : register(t2, space0);
ByteAddressBuffer Triangles : register(t3, space0);
ByteAddressBuffer Updates : register(t4, space0);
RWByteAddressBuffer History : register(u5, space0);

static const uint kParamSpacing = 0;
static const uint kParamRays = 4;
static const uint kParamUpdateCount = 8;
static const uint kParamShadowDistance = 12;
static const uint kParamSunDirection = 16;
static const uint kParamSunRadiance = 32;
static const uint kParamSkyRadiance = 48;
static const uint kParamBias = 60;
static const uint kParamCameraWorld = 64;

static const uint kUpdateStride = 32;
static const uint kProbeStride = 144;
static const uint kInstanceStride = 64;
static const uint kTriangleStride = 64;
static const uint kThreads = 64;

static const float kPi = 3.14159265358979;
static const float kGoldenAngle = 2.39996322972865;
static const float kFarDistance = 1.0e6;

struct Hit
{
    float t;
    uint instance;
    uint primitive;
    float2 barycentrics;
    float3x4 object_to_world;
};

struct Surface
{
    float3 position;
    float3 normal;  // the geometric normal, turned toward the ray that found the surface
    float3 albedo;
};

// Traces one ray against every triangle as opaque. With any_hit the traversal ends at the first hit (a shadow); otherwise it finds the closest.
bool Trace(float3 origin, float3 direction, float t_max, bool any_hit, out Hit hit)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.0;
    ray.TMax = t_max;

    uint flags = RAY_FLAG_FORCE_OPAQUE;
    if (any_hit)
    {
        flags |= RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH;
    }
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(Scene, flags, 0xFF, ray);
    while (query.Proceed())
    {
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

// The surface at a hit: the world position of its triangle, the normal turned toward the incoming ray, and the albedo of its instance.
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
    return surface;
}

// The radiance a probe receives along one direction: the sky on a miss; on a hit, the surface's diffuse response to the sky and the sun.
float3 Radiance(float3 origin, float3 direction, float3 sun, float3 sun_radiance, float3 sky, float shadow_distance, float bias)
{
    Hit hit;
    if (!Trace(origin, direction, kFarDistance, false, hit))
    {
        return sky;
    }
    Surface surface = SurfaceAt(hit, direction);

    float facing = max(dot(surface.normal, sun), 0.0);
    float lit = 0.0;
    if (facing > 0.0)
    {
        Hit shadow;
        bool blocked = Trace(surface.position + surface.normal * bias, sun, shadow_distance, true, shadow);
        lit = blocked ? 0.0 : 1.0;
    }
    return surface.albedo * (sky + sun_radiance * (facing * lit));
}

// The i-th of count directions on a spherical Fibonacci set: equal areas, so each carries the same solid angle.
float3 SphereDirection(uint index, uint count)
{
    float y = 1.0 - 2.0 * (float(index) + 0.5) / float(count);
    float r = sqrt(max(0.0, 1.0 - y * y));
    float phi = float(index) * kGoldenAngle;
    return float3(r * cos(phi), y, r * sin(phi));
}

// The nine real SH basis functions of probes.cpp's sh_basis, at a unit direction.
void ShBasis(float3 d, out float b[9])
{
    b[0] = 0.282095;
    b[1] = 0.488603 * d.y;
    b[2] = 0.488603 * d.z;
    b[3] = 0.488603 * d.x;
    b[4] = 1.092548 * d.x * d.y;
    b[5] = 1.092548 * d.y * d.z;
    b[6] = 0.315392 * (3.0 * d.z * d.z - 1.0);
    b[7] = 1.092548 * d.x * d.z;
    b[8] = 0.546274 * (d.x * d.x - d.y * d.y);
}

// The diffuse convention's band weight (probes.cpp band_weight): irradiance / pi = sum of weight_i * projection_i * basis_i.
float BandWeight(uint index)
{
    if (index == 0)
    {
        return 1.0;
    }
    return index < 4 ? 2.0 / 3.0 : 0.25;
}

// Replaces a non-finite component with zero, so one bad trace cannot spread NaN through the history.
float3 Finite(float3 value)
{
    return select(isfinite(value), value, float3(0.0, 0.0, 0.0));
}

[numthreads(64, 1, 1)]
void CSDynamicGi(uint3 id : SV_DispatchThreadID)
{
    uint count = Params.Load(kParamUpdateCount);
    if (id.x >= count)
    {
        return;
    }

    uint record = id.x * kUpdateStride;
    uint slot = Updates.Load(record);
    float weight = asfloat(Updates.Load(record + 4));
    int3 cell = asint(Updates.Load3(record + 8));

    float spacing = asfloat(Params.Load(kParamSpacing));
    uint rays = Params.Load(kParamRays);
    float shadow_distance = asfloat(Params.Load(kParamShadowDistance));
    float bias = asfloat(Params.Load(kParamBias));
    float3 sun = asfloat(Params.Load3(kParamSunDirection));
    float3 sun_radiance = asfloat(Params.Load3(kParamSunRadiance));
    float3 sky = asfloat(Params.Load3(kParamSkyRadiance));

    float3 origin = (float3(cell) + 0.5) * spacing - asfloat(Params.Load3(kParamCameraWorld));
    float3 coefficients[9];
    for (uint i = 0; i < 9; ++i)
    {
        coefficients[i] = float3(0.0, 0.0, 0.0);
    }
    for (uint k = 0; k < rays; ++k)
    {
        float3 direction = SphereDirection(k, rays);
        float3 radiance = Finite(Radiance(origin, direction, sun, sun_radiance, sky, shadow_distance, bias));
        float basis[9];
        ShBasis(direction, basis);
        for (uint i = 0; i < 9; ++i)
        {
            coefficients[i] += basis[i] * radiance;
        }
    }

    // Each ray covers 4 pi / rays steradians, the same for every ray of the set.
    float solid_angle = 4.0 * kPi / float(rays);
    uint base = slot * kProbeStride;
    for (uint i = 0; i < 9; ++i)
    {
        float3 estimate = coefficients[i] * (solid_angle * BandWeight(i));
        float3 previous = asfloat(History.Load3(base + i * 16));
        // A weight of one is a fresh trace, so it replaces the history outright; the lerp would keep a stale value through 0 * old.
        float3 next = weight >= 1.0 ? estimate : lerp(previous, estimate, weight);
        History.Store3(base + i * 16, asuint(Finite(next)));
    }
}
