// Froxel volumetric fog (US-045): the two compute passes that VolumetricFog (engine/render/src/volumetric_fog.cpp) dispatches.
// CSFogInject lights each froxel (the sun and up to 16 local lights, each scaled by a Henyey-Greenstein phase and by a visibility that an
// inline ray query decides), then blends the result with the reprojected previous grid. CSFogIntegrate walks each column front to back and
// accumulates the radiance and the transmittance. inject_reference, integrate_reference in volumetric_fog.cpp run the same steps. Keep the
// two in step.
//
// Inject descriptor set (the binding is the register number):
//   t0  Params       the frame's parameters (layout below)
//   t1  Scene        the top-level acceleration structure (opaque triangles; the alpha masks do not apply)
//   t2  PrevGrid     the previous injected grid, 16 bytes per froxel
//   u3  NextGrid     the injected grid this frame writes
// Integrate descriptor set:
//   t0  Params
//   t4  Grid         the injected grid
//   u5  Integrated   the accumulated radiance (rgb) and the transmittance (a)
// Apply descriptor set (CSFogApply, US-045):
//   t0  Params
//   t6  Lit          the lit frame (rgba, HDR); its colour is attenuated and the fog added
//   t7  SceneDepth   the scene's NDC depth (R32F); 0 where nothing was drawn, which maps to the camera's far plane
//   t8  Integrated   the integrated grid (read as raw bytes)
//   u10 Output       the frame with the fog (same size as Lit)
//
// Froxel (x, y, z) is at ((z * height + y) * width + x) * 16. A froxel's centre is at eye + d * (forward + ndc_x * right + ndc_y * up) with
// d = near * (far / near)^((z + 0.5) / depth), ndc_x = 2 (x + 0.5) / width - 1, and ndc_y = 1 - 2 (y + 0.5) / height.
//
// Parameters, in bytes (float unless noted): 0 eye, 16 forward, 32 right, 48 up, 64 previous eye, 80 previous forward, 96 previous right,
// 112 previous up (each three floats and a pad), 128 sun direction (toward the sun), 144 sun radiance, 160 albedo, 172 density, 176 height
// falloff, 180 anisotropy, 184 near, 188 far, 192 width (uint), 196 height (uint), 200 depth (uint), 204 flags (uint: bit 0 reset, bit 1 the
// previous grid is valid, bit 2 shadows), 208 history weight, 212 max age (uint), 216 light count (uint). Light i at 224 + 32 i: position
// (three floats), range at +12, radiance at +16 (three floats), shadowed (uint) at +28. 736 camera near, 740 camera far (the apply pass).

RWByteAddressBuffer NextGrid : register(u3, space0);
RWByteAddressBuffer Integrated : register(u5, space0);
#ifdef __spirv__
[[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> Output : register(u10, space0);

ByteAddressBuffer Params : register(t0, space0);
RaytracingAccelerationStructure Scene : register(t1, space0);
ByteAddressBuffer PrevGrid : register(t2, space0);
ByteAddressBuffer Grid : register(t4, space0);
Texture2D<float4> Lit : register(t6, space0);
Texture2D<float> SceneDepth : register(t7, space0);
ByteAddressBuffer IntegratedRead : register(t8, space0);

static const uint kParamEye = 0;
static const uint kParamForward = 16;
static const uint kParamRight = 32;
static const uint kParamUp = 48;
static const uint kParamPreviousEye = 64;
static const uint kParamPreviousForward = 80;
static const uint kParamPreviousRight = 96;
static const uint kParamPreviousUp = 112;
static const uint kParamSunDirection = 128;
static const uint kParamSunRadiance = 144;
static const uint kParamAlbedo = 160;
static const uint kParamDensity = 172;
static const uint kParamHeightFalloff = 176;
static const uint kParamAnisotropy = 180;
static const uint kParamNear = 184;
static const uint kParamFar = 188;
static const uint kParamWidth = 192;
static const uint kParamHeight = 196;
static const uint kParamDepth = 200;
static const uint kParamFlags = 204;
static const uint kParamHistoryWeight = 208;
static const uint kParamMaxAge = 212;
static const uint kParamLightCount = 216;
static const uint kParamLights = 224;
static const uint kLightStride = 32;
static const uint kLightRange = 12;
static const uint kLightRadiance = 16;
static const uint kParamCameraNear = 736;
static const uint kParamCameraFar = 740;
static const uint kLightShadowed = 28;

static const uint kFlagReset = 1;
static const uint kFlagHistoryValid = 2;
static const uint kFlagShadows = 4;

static const uint kFroxelBytes = 16;
static const float kSunDistance = 1.0e5;
static const float kPi = 3.14159265358979;

float LoadFloat(uint offset)
{
    return asfloat(Params.Load(offset));
}

float3 LoadFloat3(uint offset)
{
    return asfloat(Params.Load3(offset));
}

// The view depth of the boundary of slice `slice`, as fog_slice_edge in volumetric_fog.cpp.
float SliceEdge(float near_plane, float far_plane, uint depth, float slice)
{
    return near_plane * pow(far_plane / near_plane, slice / (float)depth);
}

float FogDensity(float density, float height_falloff, float height)
{
    return density * exp(-height_falloff * height);
}

float HenyeyGreenstein(float cosine, float g)
{
    float g2 = g * g;
    float denominator = max(1.0 + g2 - 2.0 * g * cosine, 1.0e-4);
    return (1.0 - g2) / (4.0 * kPi * denominator * sqrt(denominator));
}

// The world position of a froxel's centre, as fog_froxel_center in volumetric_fog.cpp.
float3 FroxelCenter(uint3 id, uint width, uint height, uint depth, float near_plane, float far_plane, float3 eye, float3 forward, float3 right,
                    float3 up)
{
    float view_depth = SliceEdge(near_plane, far_plane, depth, (float)id.z + 0.5);
    float ndc_x = 2.0 * ((float)id.x + 0.5) / (float)width - 1.0;
    float ndc_y = 1.0 - 2.0 * ((float)id.y + 0.5) / (float)height;
    return eye + view_depth * (forward + ndc_x * right + ndc_y * up);
}

// One visibility ray from `from` toward `to`: 1 when nothing opaque is on the segment, 0 when something is.
float Visibility(float3 from, float3 to)
{
    RayDesc ray;
    ray.Origin = from;
    ray.Direction = normalize(to - from);
    ray.TMin = 1.0e-3;
    ray.TMax = length(to - from);

    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(Scene, RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, ray);
    while (query.Proceed())
    {
    }
    return query.CommittedStatus() == COMMITTED_NOTHING ? 1.0 : 0.0;
}

[numthreads(8, 8, 4)]
void CSFogInject(uint3 id : SV_DispatchThreadID)
{
    uint width = Params.Load(kParamWidth);
    uint height = Params.Load(kParamHeight);
    uint depth = Params.Load(kParamDepth);
    if (id.x >= width || id.y >= height || id.z >= depth)
    {
        return;
    }

    float3 eye = LoadFloat3(kParamEye);
    float3 forward = LoadFloat3(kParamForward);
    float3 right = LoadFloat3(kParamRight);
    float3 up = LoadFloat3(kParamUp);
    float near_plane = LoadFloat(kParamNear);
    float far_plane = LoadFloat(kParamFar);
    float3 sun_direction = LoadFloat3(kParamSunDirection);
    float3 sun_radiance = LoadFloat3(kParamSunRadiance);
    float3 albedo = LoadFloat3(kParamAlbedo);
    float density = LoadFloat(kParamDensity);
    float height_falloff = LoadFloat(kParamHeightFalloff);
    float anisotropy = LoadFloat(kParamAnisotropy);
    uint flags = Params.Load(kParamFlags);
    float history_weight = LoadFloat(kParamHistoryWeight);
    uint max_age = Params.Load(kParamMaxAge);
    uint light_count = Params.Load(kParamLightCount);
    bool shadows = (flags & kFlagShadows) != 0;

    float3 centre = FroxelCenter(id, width, height, depth, near_plane, far_plane, eye, forward, right, up);
    float3 ray = normalize(centre - eye);

    // Lighting: the sun, then each local light. Each term is scaled by its phase and by its visibility.
    float3 lighting = float3(0, 0, 0);
    if (any(sun_radiance > 0.0))
    {
        float visible = shadows ? Visibility(centre, centre + sun_direction * kSunDistance) : 1.0;
        lighting += sun_radiance * HenyeyGreenstein(dot(sun_direction, ray), anisotropy) * visible;
    }
    for (uint i = 0; i < light_count; ++i)
    {
        uint base = kParamLights + i * kLightStride;
        float3 position = LoadFloat3(base);
        float range = LoadFloat(base + kLightRange);
        float3 radiance = LoadFloat3(base + kLightRadiance);
        bool light_shadowed = Params.Load(base + kLightShadowed) != 0;
        float3 to_light = position - centre;
        float distance_to_light = length(to_light);
        if (!(distance_to_light > 1.0e-4))
        {
            continue;
        }
        float3 direction = to_light / distance_to_light;
        float t = saturate(distance_to_light / range);
        float attenuation = (1.0 - t) * (1.0 - t) / (1.0 + distance_to_light * distance_to_light);
        float visible = (light_shadowed && shadows) ? Visibility(centre, position) : 1.0;
        lighting += radiance * attenuation * HenyeyGreenstein(dot(direction, ray), anisotropy) * visible;
    }
    float3 scatter = FogDensity(density, height_falloff, centre.y) * albedo * lighting;

    // Temporal stabilisation: the previous grid at the reprojected centre, when the centre is inside the previous frustum and the history is
    // not reset.
    float4 cell = float4(scatter, 1.0);
    bool use_history = (flags & kFlagHistoryValid) != 0 && (flags & kFlagReset) == 0;
    if (use_history)
    {
        float3 previous_eye = LoadFloat3(kParamPreviousEye);
        float3 previous_forward = LoadFloat3(kParamPreviousForward);
        float3 previous_right = LoadFloat3(kParamPreviousRight);
        float3 previous_up = LoadFloat3(kParamPreviousUp);
        float3 offset = centre - previous_eye;
        float view_depth = dot(offset, previous_forward);
        if (view_depth > 0.0)
        {
            float ndc_x = dot(offset, previous_right) / (view_depth * dot(previous_right, previous_right));
            float ndc_y = dot(offset, previous_up) / (view_depth * dot(previous_up, previous_up));
            float slice = (float)depth * log(view_depth / near_plane) / log(far_plane / near_plane);
            if (abs(ndc_x) <= 1.0 && abs(ndc_y) <= 1.0 && slice >= 0.0 && slice < (float)depth)
            {
                uint px = min((uint)((ndc_x + 1.0) * 0.5 * (float)width), width - 1);
                uint py = min((uint)((1.0 - ndc_y) * 0.5 * (float)height), height - 1);
                uint pz = (uint)slice;
                uint previous_cell = (pz * height + py) * width + px;
                float4 prior = asfloat(PrevGrid.Load4(previous_cell * kFroxelBytes));
                if (all(isfinite(prior)))
                {
                    float age = min(prior.a + 1.0, (float)max_age);
                    float alpha = max(1.0 - history_weight, 1.0 / (age + 1.0));
                    cell = float4(lerp(prior.rgb, scatter, alpha), age);
                }
            }
        }
    }

    uint cell_index = (id.z * height + id.y) * width + id.x;
    NextGrid.Store4(cell_index * kFroxelBytes, asuint(cell));
}

[numthreads(8, 8, 1)]
void CSFogIntegrate(uint3 id : SV_DispatchThreadID)
{
    uint width = Params.Load(kParamWidth);
    uint height = Params.Load(kParamHeight);
    uint depth = Params.Load(kParamDepth);
    if (id.x >= width || id.y >= height)
    {
        return;
    }

    float3 eye = LoadFloat3(kParamEye);
    float3 forward = LoadFloat3(kParamForward);
    float3 right = LoadFloat3(kParamRight);
    float3 up = LoadFloat3(kParamUp);
    float near_plane = LoadFloat(kParamNear);
    float far_plane = LoadFloat(kParamFar);
    float density = LoadFloat(kParamDensity);
    float height_falloff = LoadFloat(kParamHeightFalloff);

    // One thread per column: the transmittance and the radiance run from the near plane outward.
    float3 radiance = float3(0, 0, 0);
    float transmittance = 1.0;
    for (uint z = 0; z < depth; ++z)
    {
        float near_edge = SliceEdge(near_plane, far_plane, depth, (float)z);
        float far_edge = SliceEdge(near_plane, far_plane, depth, (float)z + 1.0);
        float length_of_slice = far_edge - near_edge;
        uint3 froxel = uint3(id.x, id.y, z);
        float3 centre = FroxelCenter(froxel, width, height, depth, near_plane, far_plane, eye, forward, right, up);
        float extinction = FogDensity(density, height_falloff, centre.y);
        uint cell_index = (z * height + id.y) * width + id.x;
        float3 scatter = asfloat(Grid.Load4(cell_index * kFroxelBytes)).rgb;
        float3 slice = extinction > 1.0e-8 ? scatter * (1.0 - exp(-extinction * length_of_slice)) / extinction : scatter * length_of_slice;
        radiance += transmittance * slice;
        transmittance *= exp(-extinction * length_of_slice);
        Integrated.Store4(cell_index * kFroxelBytes, asuint(float4(radiance, transmittance)));
    }
}

// The view depth of a scene's NDC depth, for the camera's range. NDC 0 is the cleared background, so it maps to the far plane. Matches
// fog_scene_view_depth in volumetric_fog.cpp.
float SceneViewDepth(float ndc_depth, float camera_near, float camera_far)
{
    if (!(ndc_depth > 0.0))
    {
        return camera_far;
    }
    float ndc = min(ndc_depth, 1.0);
    return camera_near * camera_far / (camera_far - ndc * (camera_far - camera_near));
}

// One thread per pixel: the pixel's grid column (the grid may be smaller than the frame) is read at the slice of its scene depth, and the
// fog is applied as apply_fog does: the colour is attenuated and the accumulated radiance is added.
[numthreads(8, 8, 1)]
void CSFogApply(uint3 id : SV_DispatchThreadID)
{
    uint out_width;
    uint out_height;
    Output.GetDimensions(out_width, out_height);
    if (id.x >= out_width || id.y >= out_height)
    {
        return;
    }
    uint width = Params.Load(kParamWidth);
    uint height = Params.Load(kParamHeight);
    uint depth = Params.Load(kParamDepth);
    float near_plane = LoadFloat(kParamNear);
    float far_plane = LoadFloat(kParamFar);

    float view_depth = SceneViewDepth(SceneDepth.Load(int3(id.xy, 0)), LoadFloat(kParamCameraNear), LoadFloat(kParamCameraFar));
    float slice = (float)depth * log(view_depth / near_plane) / log(far_plane / near_plane);
    uint z = (uint)clamp(slice, 0.0, (float)depth - 1.0);
    uint column_x = min((uint)(((float)id.x + 0.5) * (float)width / (float)out_width), width - 1);
    uint column_y = min((uint)(((float)id.y + 0.5) * (float)height / (float)out_height), height - 1);
    uint cell_index = (z * height + column_y) * width + column_x;
    float4 integrated = asfloat(IntegratedRead.Load4(cell_index * kFroxelBytes));

    float4 color = Lit.Load(int3(id.xy, 0));
    Output[id.xy] = float4(color.rgb * integrated.a + integrated.rgb, color.a);
}
