// Screen-space reflections (US-040): the compute pass that adds traced specular to the lit scene. It runs after the opaque pass, reads the lit
// color, and writes the corrected color into the reflected target, which the post pass reads. The march is trace_screen_reflection in engine/gi/src/screen_reflections.cpp, step for step,
// and the fallback is the environment specular the scene pixel shader adds (scene.hlsl, ShadeFragment), so a miss leaves the pixel as it was.
//
// Descriptor sets (the set index is the register space):
//   set 0  ReflectionParams  t0: the frame's parameters (layout below); t1: the lit color; t2: the scene's depth values (NDC);
//                            t3: the surface targets (normal and roughness); t4: the base targets (Fresnel base and occlusion);
//                            u5: the reflected color, the lit color with the traced specular added
//   set 1  EnvironmentData   t0: SH coefficients; t1..t5: specular levels 0..4; t6: BRDF table (the layout of scene.hlsl's set 2)
//
// Space: camera-relative world, as in scene.hlsl. The frame's view-projection maps it to clip space, and clip w is the depth along the eye.
// The surface and depth targets hold the same pixels as the lit color, so the trace reads them at the same texels.

ByteAddressBuffer ReflectionParams : register(t0, space0);
Texture2D<float4> SceneColor : register(t1, space0);
Texture2D<float> DepthValues : register(t2, space0);
Texture2D<float4> SurfaceData : register(t3, space0);
Texture2D<float4> BaseData : register(t4, space0);
RWTexture2D<float4> Reflected : register(u5, space0);

ByteAddressBuffer EnvironmentData : register(t0, space1);
Texture2D<float4> SpecularLevel0 : register(t1, space1);
Texture2D<float4> SpecularLevel1 : register(t2, space1);
Texture2D<float4> SpecularLevel2 : register(t3, space1);
Texture2D<float4> SpecularLevel3 : register(t4, space1);
Texture2D<float4> SpecularLevel4 : register(t5, space1);
Texture2D<float4> BrdfLut : register(t6, space1);

static const float kPi = 3.14159265358979323846;

// Parameter layout, in bytes (written by scene_renderer.cpp). The matrices are column-major, as in the frame buffer.
static const uint kParamViewProjection = 0;
static const uint kParamInverseViewProjection = 64;
static const uint kParamScale = 128;      // UV units per NDC unit (x, y); the backend's y sign
static const uint kParamSize = 136;       // width and height in pixels
static const uint kParamNearFar = 144;    // the camera's near and far planes
static const uint kParamSteps = 152;      // uniform steps along the reflected ray
static const uint kParamStep = 156;       // meters per step
static const uint kParamThickness = 160;  // meters
static const uint kParamEdgeFade = 164;   // NDC distance from the edge over which hits fade
static const uint kParamRoughnessCutoff = 168;

float4 Transform4(uint base, float4 p)
{
    float4 c0 = asfloat(ReflectionParams.Load4(base));
    float4 c1 = asfloat(ReflectionParams.Load4(base + 16));
    float4 c2 = asfloat(ReflectionParams.Load4(base + 32));
    float4 c3 = asfloat(ReflectionParams.Load4(base + 48));
    return c0 * p.x + c1 * p.y + c2 * p.z + c3 * p.w;
}

float2 EquirectUv(float3 d)
{
    return float2(0.5 + atan2(d.z, d.x) / (2.0 * kPi), acos(clamp(d.y, -1.0, 1.0)) / kPi);
}

float3 SampleEquirect(Texture2D<float4> image, float3 d)
{
    uint width;
    uint height;
    image.GetDimensions(width, height);
    float2 uv = EquirectUv(d);
    uint2 texel = min(uint2(floor(uv * float2(width, height))), uint2(width, height) - 1);
    return image.Load(int3(texel, 0)).rgb;
}

float3 SpecularAt(uint level, float3 d)
{
    switch (level)
    {
        case 0: return SampleEquirect(SpecularLevel0, d);
        case 1: return SampleEquirect(SpecularLevel1, d);
        case 2: return SampleEquirect(SpecularLevel2, d);
        case 3: return SampleEquirect(SpecularLevel3, d);
        default: return SampleEquirect(SpecularLevel4, d);
    }
}

// Matches SampleSpecular in scene.hlsl and sample_specular in pbr.cpp.
float3 SampleSpecular(float3 reflection, float roughness)
{
    float position = saturate(roughness) * 4.0;
    uint low = min((uint)floor(position), 3u);
    float t = position - (float)low;
    return lerp(SpecularAt(low, reflection), SpecularAt(low + 1, reflection), t);
}

// Matches SampleBrdfLut in scene.hlsl and sample_brdf_lut in pbr.cpp.
float2 SampleBrdfLut(float n_dot_v, float roughness)
{
    uint size;
    uint rows;
    BrdfLut.GetDimensions(size, rows);
    float fx = saturate(n_dot_v) * (float)size - 0.5;
    float fy = saturate(roughness) * (float)size - 0.5;
    float tx = fx - floor(fx);
    float ty = fy - floor(fy);
    int x0 = clamp((int)floor(fx), 0, (int)size - 1);
    int x1 = clamp((int)floor(fx) + 1, 0, (int)size - 1);
    int y0 = clamp((int)floor(fy), 0, (int)size - 1);
    int y1 = clamp((int)floor(fy) + 1, 0, (int)size - 1);
    float2 top = lerp(BrdfLut.Load(int3(x0, y0, 0)).rg, BrdfLut.Load(int3(x1, y0, 0)).rg, tx);
    float2 bottom = lerp(BrdfLut.Load(int3(x0, y1, 0)).rg, BrdfLut.Load(int3(x1, y1, 0)).rg, tx);
    return lerp(top, bottom, ty);
}

// Linear depth (distance along the eye) of an NDC depth, for the perspective with the camera's near and far planes (depth in [0, 1]).
float LinearDepth(float ndc_depth, float near_plane, float far_plane)
{
    return near_plane * far_plane / (far_plane - ndc_depth * (far_plane - near_plane));
}

[numthreads(8, 8, 1)]
void CSReflections(uint3 id : SV_DispatchThreadID)
{
    uint width = ReflectionParams.Load(kParamSize);
    uint height = ReflectionParams.Load(kParamSize + 4);
    if (id.x >= width || id.y >= height)
    {
        return;
    }
    int3 texel = int3(id.xy, 0);
    float4 lit = SceneColor.Load(texel);

    // A pixel no opaque draw covered has a zero normal, and a rough surface uses the environment alone: both keep the lit color.
    float4 surface = SurfaceData.Load(texel);
    float roughness = surface.w;
    float cutoff = asfloat(ReflectionParams.Load(kParamRoughnessCutoff));
    if (dot(surface.xyz, surface.xyz) < 0.25 || roughness > cutoff)
    {
        Reflected[id.xy] = lit;
        return;
    }
    float2 scale = asfloat(ReflectionParams.Load2(kParamScale));
    float2 near_far = asfloat(ReflectionParams.Load2(kParamNearFar));
    float2 uv = (float2(id.xy) + 0.5) / float2(width, height);
    float2 ndc = (uv - 0.5) / scale;
    float4 origin_clip = Transform4(kParamInverseViewProjection, float4(ndc, DepthValues.Load(texel).r, 1.0));
    float3 origin = origin_clip.xyz / origin_clip.w;

    float3 n = surface.xyz;
    float3 view = normalize(-origin);
    float n_dot_v = max(dot(n, view), 1e-4);
    float3 reflection = reflect(-view, n);
    float3 environment = SampleSpecular(reflection, roughness);
    float4 base = BaseData.Load(texel);
    float2 brdf = SampleBrdfLut(n_dot_v, roughness);
    float3 specular_weight = base.rgb * brdf.x + brdf.y;

    uint steps = ReflectionParams.Load(kParamSteps);
    float step = asfloat(ReflectionParams.Load(kParamStep));
    float thickness = asfloat(ReflectionParams.Load(kParamThickness));
    float edge_fade = asfloat(ReflectionParams.Load(kParamEdgeFade));
    float3 hit_color = float3(0.0, 0.0, 0.0);
    float confidence = 0.0;
    for (uint i = 1; i <= steps; ++i)
    {
        float3 point_position = origin + reflection * (step * (float)i);
        float4 clip = Transform4(kParamViewProjection, float4(point_position, 1.0));
        if (clip.w <= 0.0)
        {
            break;
        }
        float2 sample_ndc = clip.xy / clip.w;
        if (abs(sample_ndc.x) > 1.0 || abs(sample_ndc.y) > 1.0)
        {
            break;  // the ray left the screen
        }
        float2 sample_uv = sample_ndc * scale + 0.5;
        uint2 sample_texel = min(uint2(floor(sample_uv * float2(width, height))), uint2(width, height) - 1);
        float4 surface_here = SurfaceData.Load(int3(sample_texel, 0));
        if (dot(surface_here.xyz, surface_here.xyz) < 0.25)
        {
            continue;  // nothing drawn there: infinitely far, so no hit
        }
        float surface_depth = LinearDepth(DepthValues.Load(int3(sample_texel, 0)).r, near_far.x, near_far.y);
        if (clip.w > surface_depth && clip.w - surface_depth < thickness)
        {
            hit_color = SceneColor.Load(int3(sample_texel, 0)).rgb;
            if (edge_fade > 0.0)
            {
                float fade_x = saturate((1.0 - abs(sample_ndc.x)) / edge_fade);
                float fade_y = saturate((1.0 - abs(sample_ndc.y)) / edge_fade);
                confidence = fade_x * fade_y;
            }
            else
            {
                confidence = 1.0;
            }
            break;
        }
    }

    // reflection_radiance: the hit weighted by its confidence, the environment for the rest. The pass adds only the difference from the
    // environment, because the lit color already holds the environment's specular.
    float3 radiance = lerp(environment, hit_color, confidence);
    float3 delta = (radiance - environment) * specular_weight * base.a;
    Reflected[id.xy] = float4(lit.rgb + delta, lit.a);
}
