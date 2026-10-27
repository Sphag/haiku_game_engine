// Scene shader (US-033; physically based shading in US-034; direct lights and shadows in US-035; GPU indirect draws and culling in US-037; mesh
// shaders on cooked meshlets in US-038). The image-based part mirrors
// engine/render/src/pbr.cpp term for term, and the GPU tests check it against that CPU code. The direct part is documented in
// docs/lighting.md; its cluster and shadow layouts are written by scene_renderer.cpp, and lighting.cpp holds the cluster and cascade math.
//
// Descriptor sets (the set index is the register space, and the binding is the register number):
//   set 0  FrameData        t0: camera (view-projection, previous view-projection, velocity scale), then one 208-byte record per item, then the
//                           palettes of the skinned items (layout in scene_renderer.cpp)
//   set 1  MaterialData     t0: parameters (64 bytes); t1..t5: base color, metallic-roughness, normal, emissive, occlusion
//   set 2  EnvironmentData  t0: nine SH coefficients (irradiance / pi); t1..t5: prefiltered specular levels 0..4; t6: BRDF table;
//                           t7: the baked probe grid (US-040, GiData); t8: the dynamic probe grid's history (US-044, slot order);
//                           t9: the dynamic grid's sampling block (US-044). Dynamic GI replaces the baked grid when its block is enabled.
//   set 3  LightingData     t0: header, light records, shadow records, cluster counts, cluster indices (offsets in the header);
//                           t1: cascade shadow atlas (R32Float depth values); t2: local shadow atlas; t3: the ray-traced shadow
//                           (US-043, x = sun visibility at the pixel); t4: the ray-traced reflection (rgb, a = 1 where it applies)
//   set 1  (CSCull only)    t0: CullData; u1: CullArgs; u2: CullCounts. CSCull also reads FrameData from set 0 (see GPU culling below).
//   set 0  (mesh path)      t0: FrameData (as above, with the meshlet header); t1: meshlet records; t2: meshlet vertex indices; t3: meshlet
//                           triangle bytes; t4: mesh vertex pool; u5: meshlet visibility counts. These are the meshlet pools (see below).
//
// Textures are read with Load at a nearest texel of fract(uv), so the GPU and the CPU reference pick the same texel. The entry points are
// VSMain, PSMain (opaque and alpha-mask fragments: color, entity ID, depth value, and motion vector), PSBlend (alpha-blended fragments:
// color only), and VSShadow and PSShadow, which draw a shadow tile (depth values into the atlas). VSSkinned and VSSkinnedShadow are the
// skinned variants (US-036): they blend the item's palette by the vertex's joints and weights, and they reuse PSMain, PSBlend, and PSShadow.
// VSIndirect and VSSkinnedIndirect (US-037) are the opaque vertex stages of indirect draws, which take the item from a per-instance input, and
// CSCull is the compute stage that culls the candidates into those draws (see GPU culling below).
// ASMeshlet (amplification) and MSMeshlet (mesh) are the mesh path's stages (US-038). They use PSMain, and they write the same vertex fields as
// VSMain, through MakeVertex. Their culling is described with the mesh path below.
// Motion vectors come from the current and previous clip positions of each vertex, in UV units (x right, y down), so both backends agree.

ByteAddressBuffer FrameData : register(t0, space0);

// Frame buffer layout (written by scene_renderer.cpp). Camera: the current view-projection at 0, the shadow tile's light clip at 64 (shadow
// draws only), the previous view-projection at 128, and the velocity scale (two floats) at 192. The TAA jitter (two floats, NDC) is at 224 and
// the linear-output flag at 232 (US-039). Items start at 256, 208 bytes each: the model
// (0), its normal matrix (64), the tint (112), the pick id (128), the bone count (132), the byte offset of the palette (136), and the previous
// model (144). A skinned palette is bone-count matrices of the current pose, then the same number of the previous pose.
static const uint kItemsOffset = 256;
static const uint kItemStride = 208;
static const uint kRecordBones = 132;
static const uint kRecordPalette = 136;
static const uint kRecordPreviousModel = 144;
static const uint kPreviousViewProjection = 128;
static const uint kVelocityScale = 192;
// The TAA jitter as an NDC offset (two floats) and the linear-output flag (a uint: 1 writes HDR linear color, 0 writes sRGB). Both are in
// the camera slot, after the meshlet header (US-038) and before the items.
static const uint kFrameJitter = 224;
static const uint kFrameLinearOutput = 232;
// The camera's world position (three floats, meters). Fragments are camera-relative, so the probe grid adds this to find its probes (US-040).
static const uint kFrameCameraPosition = 240;

ByteAddressBuffer MaterialData : register(t0, space1);
Texture2D<float4> BaseColorMap : register(t1, space1);
Texture2D<float4> MetallicRoughnessMap : register(t2, space1);
Texture2D<float4> NormalMap : register(t3, space1);
Texture2D<float4> EmissiveMap : register(t4, space1);
Texture2D<float4> OcclusionMap : register(t5, space1);

ByteAddressBuffer EnvironmentData : register(t0, space2);
Texture2D<float4> SpecularLevel0 : register(t1, space2);
Texture2D<float4> SpecularLevel1 : register(t2, space2);
Texture2D<float4> SpecularLevel2 : register(t3, space2);
Texture2D<float4> SpecularLevel3 : register(t4, space2);
Texture2D<float4> SpecularLevel4 : register(t5, space2);
Texture2D<float4> BrdfLut : register(t6, space2);
// The probe grid (US-040), written by scene_renderer.cpp. A 32-byte header holds the origin (three floats), the spacing (one float at byte 12),
// and the dims (three uints at byte 16). Then one 144-byte record per probe, in x-fastest order: nine float4 SH coefficients (irradiance / pi),
// where w is 1 for a valid probe and 0 for a blocked or unbaked one. With no volume the dims are zero and the grid is empty.
ByteAddressBuffer GiData : register(t7, space2);
// The dynamic probe grid (US-044), written by dynamic_gi.cpp. The history is nine float4 per slot (144 bytes), in slot order: the slot of a
// window cell is the residue of the window origin plus the cell's local index, modulo the dims. The sampling block has a 48-byte header
// (the window's origin cell at 0, spacing at 12, dims at 16, enabled at 28, the origin's residue at 32) and then one valid flag per slot.
ByteAddressBuffer DynamicHistory : register(t8, space2);
ByteAddressBuffer DynamicSampling : register(t9, space2);

ByteAddressBuffer LightingData : register(t0, space3);
Texture2D<float> CascadeAtlas : register(t1, space3);
Texture2D<float> LocalAtlas : register(t2, space3);
// The ray-traced targets (US-043) of rt_effects.hlsl, one texel per pixel of the scene target.
Texture2D<float4> RtShadow : register(t3, space3);
Texture2D<float4> RtReflection : register(t4, space3);

static const float kPi = 3.14159265358979323846;
static const uint kAlphaMask = 1;
static const uint kAlphaBlend = 2;

// Lighting layout (written by scene_renderer.cpp; offsets in bytes). The header's words hold the offsets of the arrays below it.
static const uint kHeaderTilesX = 0;
static const uint kHeaderTilesY = 4;
static const uint kHeaderSlices = 8;
static const uint kHeaderMaxPerCluster = 12;
static const uint kHeaderTilePx = 16;
static const uint kHeaderNear = 20;
static const uint kHeaderSliceScale = 28;  // slices / ln(far / near)
static const uint kHeaderForward = 32;
static const uint kHeaderLightCount = 48;
static const uint kHeaderShadowCount = 52;
static const uint kHeaderView = 56;
static const uint kHeaderSigma = 60;
static const uint kHeaderLightBase = 64;
static const uint kHeaderShadowBase = 68;
static const uint kHeaderCountBase = 72;
static const uint kHeaderIndexBase = 76;
static const uint kHeaderCascadeAtlasSize = 80;
static const uint kHeaderLocalAtlasSize = 84;
static const uint kNoShadow = 0xffffffffu;
static const uint kLightDirectional = 0;
static const uint kLightPoint = 1;
static const uint kLightSpot = 2;
static const uint kViewLit = 0;
static const uint kViewClusterCount = 1;
static const uint kViewClusterOverflow = 2;
static const uint kViewCascadeIndex = 3;
static const uint kViewShadowFactor = 4;
static const uint kViewProbeCoverage = 5;

static const uint kGiOrigin = 0;
static const uint kGiSpacing = 12;
static const uint kGiDims = 16;
static const uint kGiHeaderBytes = 32;
static const uint kGiProbeBytes = 144;
static const uint kDynamicSampleOrigin = 0;
static const uint kDynamicSampleSpacing = 12;
static const uint kDynamicSampleDims = 16;
static const uint kDynamicSampleEnabled = 28;
static const uint kDynamicSampleResidue = 32;
static const uint kDynamicSampleHeaderBytes = 48;
static const uint kDynamicProbeBytes = 144;

float4 TransformPoint(uint base, float3 p)
{
    // Column-major: column j is the four floats at base + 16 * j. The point is w = 1, so column 3 is the translation.
    float4 c0 = asfloat(FrameData.Load4(base));
    float4 c1 = asfloat(FrameData.Load4(base + 16));
    float4 c2 = asfloat(FrameData.Load4(base + 32));
    float4 c3 = asfloat(FrameData.Load4(base + 48));
    return c0 * p.x + c1 * p.y + c2 * p.z + c3;
}

// The inverse transpose of the model's upper 3x3, stored as three column vectors at base, base + 16, and base + 32.
float3 TransformNormal(uint base, float3 n)
{
    float3 c0 = asfloat(FrameData.Load4(base)).xyz;
    float3 c1 = asfloat(FrameData.Load4(base + 16)).xyz;
    float3 c2 = asfloat(FrameData.Load4(base + 32)).xyz;
    return c0 * n.x + c1 * n.y + c2 * n.z;
}

float SrgbToLinearChannel(float c)
{
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float LinearToSrgbChannel(float c)
{
    return c <= 0.0031308 ? 12.92 * c : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

float3 SrgbToLinear(float3 c)
{
    return float3(SrgbToLinearChannel(c.x), SrgbToLinearChannel(c.y), SrgbToLinearChannel(c.z));
}

float3 LinearToSrgb(float3 c)
{
    c = saturate(c);
    return float3(LinearToSrgbChannel(c.x), LinearToSrgbChannel(c.y), LinearToSrgbChannel(c.z));
}

// The texel of a repeating coordinate: fract(uv) scaled to the texture size, as the CPU reference computes it.
// True when the frame draws linear HDR color for the post pass (US-039). The pixel stages then skip the sRGB encode.
bool LinearOutput()
{
    return FrameData.Load(kFrameLinearOutput) != 0;
}

float4 FetchTexture(Texture2D<float4> map, float2 uv)
{
    uint width;
    uint height;
    map.GetDimensions(width, height);
    float2 f = frac(uv);
    uint2 texel = min(uint2(floor(f * float2(width, height))), uint2(width, height) - 1);
    return map.Load(int3(texel, 0));
}

// Equirect coordinates of a unit direction; matches direction_to_equirect in pbr.cpp.
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

// Prefiltered radiance at `roughness`: the two nearest levels, blended linearly. Matches sample_specular in pbr.cpp.
float3 SampleSpecular(float3 reflection, float roughness)
{
    float position = saturate(roughness) * 4.0;
    uint low = min((uint)floor(position), 3u);
    float t = position - (float)low;
    return lerp(SpecularAt(low, reflection), SpecularAt(low + 1, reflection), t);
}

// Real SH bands 0 to 2 at a unit direction, in the order of Environment::diffuse_sh.
void ShBasis(float3 n, out float basis[9])
{
    basis[0] = 0.282095;
    basis[1] = 0.488603 * n.y;
    basis[2] = 0.488603 * n.z;
    basis[3] = 0.488603 * n.x;
    basis[4] = 1.092548 * n.x * n.y;
    basis[5] = 1.092548 * n.y * n.z;
    basis[6] = 0.315392 * (3.0 * n.z * n.z - 1.0);
    basis[7] = 1.092548 * n.x * n.z;
    basis[8] = 0.546274 * (n.x * n.x - n.y * n.y);
}

float3 EvaluateDiffuse(float3 n)
{
    float basis[9];
    ShBasis(n, basis);
    float3 sum = float3(0.0, 0.0, 0.0);
    for (uint i = 0; i < 9; ++i)
    {
        sum += asfloat(EnvironmentData.Load4(i * 16)).xyz * basis[i];
    }
    return sum;
}

// The environment's diffuse blended with the probes' sum: `valid` is the weight of the valid probes, and coverage is that weight.
float3 BlendWithEnvironment(float3 sum, float valid, float3 n, out float coverage)
{
    float3 environment = EvaluateDiffuse(n);
    if (valid <= 0.0)
    {
        coverage = 0.0;
        return environment;
    }
    coverage = valid;
    return coverage * (sum / valid) + (1.0 - coverage) * environment;
}

// Diffuse irradiance (irradiance / pi) from the dynamic probe grid (US-044). The probes around the point blend trilinearly, as in ProbeDiffuse,
// over the window's cells that hold a trace. A cell outside the window, or one with no trace, takes no weight.
float3 DynamicProbeDiffuse(float3 camera_relative, float3 n, out float coverage)
{
    float3 world = camera_relative + asfloat(FrameData.Load3(kFrameCameraPosition));
    float spacing = asfloat(DynamicSampling.Load(kDynamicSampleSpacing));
    // Probe (cell + 0.5) * spacing: the grid coordinate puts the probe of cell k at k + 0.5, so the floor is the cell below the point.
    float3 grid = world / spacing - 0.5;
    int3 origin = asint(DynamicSampling.Load3(kDynamicSampleOrigin));
    int3 dims = asint(DynamicSampling.Load3(kDynamicSampleDims));
    uint3 residue = DynamicSampling.Load3(kDynamicSampleResidue);
    int3 base = (int3)floor(grid);
    float3 t = grid - (float3)base;
    float3 sum = float3(0.0, 0.0, 0.0);
    float valid = 0.0;
    float basis[9];
    ShBasis(n, basis);
    for (uint corner = 0; corner < 8; ++corner)
    {
        int3 offset = int3(corner & 1u, (corner >> 1) & 1u, (corner >> 2) & 1u);
        float weight = (offset.x == 1 ? t.x : 1.0 - t.x) * (offset.y == 1 ? t.y : 1.0 - t.y) * (offset.z == 1 ? t.z : 1.0 - t.z);
        int3 local = base + offset - origin;
        if (weight <= 0.0 || any(local < 0) || any(local >= dims))
        {
            continue;
        }
        uint3 wrapped = residue + (uint3)local;
        wrapped = select(wrapped >= (uint3)dims, wrapped - (uint3)dims, wrapped);
        uint index = wrapped.x + (uint)dims.x * (wrapped.y + (uint)dims.y * wrapped.z);
        if (DynamicSampling.Load(kDynamicSampleHeaderBytes + index * 4) == 0)
        {
            continue;  // no trace yet: no weight
        }
        uint record = index * kDynamicProbeBytes;
        valid += weight;
        float3 probe = float3(0.0, 0.0, 0.0);
        for (uint i = 0; i < 9; ++i)
        {
            probe += asfloat(DynamicHistory.Load4(record + i * 16)).xyz * basis[i];
        }
        sum += weight * probe;
    }
    return BlendWithEnvironment(sum, valid, n, coverage);
}

// Diffuse irradiance (irradiance / pi) at a camera-relative point, matching sample_irradiance in engine/gi/src/probes.cpp. The valid probes
// around the point blend trilinearly, and the environment's diffuse fills the rest. `coverage` is the share of the weight that landed on valid
// probes: zero outside the grid, or when the grid is empty, so the result is the environment alone. With the dynamic grid enabled it reads
// that grid instead of the baked one.
float3 ProbeDiffuse(float3 camera_relative, float3 n, out float coverage)
{
    coverage = 0.0;
    if (DynamicSampling.Load(kDynamicSampleEnabled) != 0)
    {
        return DynamicProbeDiffuse(camera_relative, n, coverage);
    }
    uint3 dims = GiData.Load3(kGiDims);
    if (dims.x == 0 || dims.y == 0 || dims.z == 0)
    {
        return EvaluateDiffuse(n);
    }
    float3 world = camera_relative + asfloat(FrameData.Load3(kFrameCameraPosition));
    float3 grid = (world - asfloat(GiData.Load3(kGiOrigin))) / asfloat(GiData.Load(kGiSpacing));
    float3 last = (float3)dims - 1.0;
    if (any(grid < 0.0) || any(grid > last))
    {
        return EvaluateDiffuse(n);
    }
    // The cell whose low corner is the point's floor, kept inside the grid. A grid one probe thick in an axis has no cell there.
    uint3 base = (uint3)min(floor(grid), max(last - 1.0, 0.0));
    float3 t = grid - (float3)base;
    float3 sum = float3(0.0, 0.0, 0.0);
    float valid = 0.0;
    float basis[9];
    ShBasis(n, basis);
    for (uint corner = 0; corner < 8; ++corner)
    {
        uint3 offset = uint3(corner & 1u, (corner >> 1) & 1u, (corner >> 2) & 1u);
        float weight = (offset.x == 1 ? t.x : 1.0 - t.x) * (offset.y == 1 ? t.y : 1.0 - t.y) * (offset.z == 1 ? t.z : 1.0 - t.z);
        if (weight <= 0.0)
        {
            continue;
        }
        uint3 cell = min(base + offset, dims - 1);
        uint index = cell.x + dims.x * (cell.y + dims.y * cell.z);
        uint record = kGiHeaderBytes + index * kGiProbeBytes;
        if (asfloat(GiData.Load(record + 0 * 16 + 12)) < 0.5)
        {
            continue;  // blocked or unbaked: no weight
        }
        valid += weight;
        float3 probe = float3(0.0, 0.0, 0.0);
        for (uint i = 0; i < 9; ++i)
        {
            probe += asfloat(GiData.Load4(record + i * 16)).xyz * basis[i];
        }
        sum += weight * probe;
    }
    return BlendWithEnvironment(sum, valid, n, coverage);
}

// Bilinear between the four nearest table entries, whose centers sit at (i + 0.5) / size. Matches sample_brdf_lut in pbr.cpp.
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

// Tangent frame from screen-space derivatives, so the vertices need no tangent attribute. It solves dp = t * du + b * dv for the screen
// derivatives, which gives t along increasing u and b along increasing v. A degenerate UV mapping falls back to any frame perpendicular to
// the normal.
void TangentFrame(float3 n, float3 p, float2 uv, out float3 t, out float3 b)
{
    float3 dp1 = ddx(p);
    float3 dp2 = ddy(p);
    float2 duv1 = ddx(uv);
    float2 duv2 = ddy(uv);
    float det = duv1.x * duv2.y - duv2.x * duv1.y;
    if (abs(det) < 1e-30)
    {
        t = abs(n.y) < 0.999 ? cross(float3(0.0, 1.0, 0.0), n) : cross(float3(1.0, 0.0, 0.0), n);
        b = cross(n, t);
    }
    else
    {
        t = (dp1 * duv2.y - dp2 * duv1.y) / det;
        b = (dp2 * duv1.x - dp1 * duv2.x) / det;
    }
    t = normalize(t);
    b = normalize(b - n * dot(b, n));
    t = normalize(t - n * dot(t, n));
}

// Clip position of a point through a matrix stored in the lighting buffer (column-major, as the frame buffer's matrices are).
float4 LightingClip(uint base, float3 p)
{
    float4 c0 = asfloat(LightingData.Load4(base));
    float4 c1 = asfloat(LightingData.Load4(base + 16));
    float4 c2 = asfloat(LightingData.Load4(base + 32));
    float4 c3 = asfloat(LightingData.Load4(base + 48));
    return c0 * p.x + c1 * p.y + c2 * p.z + c3;
}

// Visibility of one shadow record at a camera-relative point: 1 is lit, 0 is in shadow, and the values between are the PCF average. The
// receiver moves toward the light by the record's light bias (texels times the map's world texel size) before the lookup. That keeps its
// texel and lowers its depth, so a sloped surface stops comparing with its neighbor. Taps outside the tile read the tile's
// edge, so a filter never reads a neighbor's depth. A point outside the light's volume is lit.
float ShadowRecordVisibility(uint shadow_base, uint index, float3 p)
{
    uint base = shadow_base + 128 * index;
    uint atlas = LightingData.Load(base + 76);
    if (atlas == kNoShadow)
    {
        return 1.0;
    }
    // Records: the light bias in texels (84), the world size of a texel (100), and the anchor (104): the direction toward the light for a
    // cascade (kind 0), or the light's position for a spot (kind 1).
    float offset = asfloat(LightingData.Load(base + 84)) * asfloat(LightingData.Load(base + 100));
    float3 anchor = asfloat(LightingData.Load3(base + 104));
    float3 toward = LightingData.Load(base + 96) == 0 ? anchor : normalize(anchor - p);
    float3 q = p + toward * offset;
    float4 clip = LightingClip(base, q);
    if (clip.w <= 0.0)
    {
        return 1.0;
    }
    float3 ndc = clip.xyz / clip.w;
    if (any(abs(ndc.xy) > 1.0) || ndc.z < 0.0 || ndc.z > 1.0)
    {
        return 1.0;
    }
    float origin_x = asfloat(LightingData.Load(base + 64));
    float origin_y = asfloat(LightingData.Load(base + 68));
    float size = asfloat(LightingData.Load(base + 72));
    float sigma = asfloat(LightingData.Load(kHeaderSigma));
    float bias = asfloat(LightingData.Load(base + 80));
    int radius = (int)LightingData.Load(base + 88);
    // The tile texel of the receiver. Rows follow the backend's sign, as the tile remap does (docs/lighting.md).
    float fx = origin_x + (0.5 + 0.5 * ndc.x) * size;
    float fy = origin_y + (0.5 + 0.5 * sigma * ndc.y) * size;
    int cx = (int)floor(fx);
    int cy = (int)floor(fy);
    int x_lo = (int)origin_x;
    int y_lo = (int)origin_y;
    int x_hi = x_lo + (int)size - 1;
    int y_hi = y_lo + (int)size - 1;
    float depth = ndc.z - bias;
    float lit = 0.0;
    float taps = 0.0;
    [loop]
    for (int dy = -radius; dy <= radius; ++dy)
    {
        [loop]
        for (int dx = -radius; dx <= radius; ++dx)
        {
            int tx = clamp(cx + dx, x_lo, x_hi);
            int ty = clamp(cy + dy, y_lo, y_hi);
            float stored = atlas == 0 ? CascadeAtlas.Load(int3(tx, ty, 0)) : LocalAtlas.Load(int3(tx, ty, 0));
            lit += depth <= stored ? 1.0 : 0.0;
            taps += 1.0;
        }
    }
    return lit / taps;
}

// Visibility of a directional light at a point: the first cascade whose split reaches the point's view depth. `used` is that cascade's
// index, or kNoShadow when no cascade covers the point.
float CascadeVisibility(uint shadow_base, uint first, uint count, float view_depth, float3 p, out uint used)
{
    used = kNoShadow;
    for (uint i = 0; i < count; ++i)
    {
        float split = asfloat(LightingData.Load(shadow_base + 128 * (first + i) + 92));
        if (view_depth <= split)
        {
            used = i;
            return ShadowRecordVisibility(shadow_base, first + i, p);
        }
    }
    return 1.0;
}

// Falloff of a light at a distance, mirroring light_attenuation in lighting.cpp.
float LightScale(uint type, float intensity, float range, float distance, float cos_angle, float cos_inner, float cos_outer)
{
    if (type == kLightDirectional)
    {
        return intensity;
    }
    float ratio = distance / range;
    float ratio4 = ratio * ratio * ratio * ratio;
    float window = saturate(1.0 - ratio4);
    float scale = intensity * window * window / max(distance * distance, 0.01);
    if (type == kLightSpot)
    {
        float cone = cos_angle >= cos_outer ? 1.0 : 0.0;
        if (cos_inner > cos_outer)
        {
            float t = saturate((cos_angle - cos_outer) / (cos_inner - cos_outer));
            cone = t * t;
        }
        scale *= cone;
    }
    return scale;
}

// GGX specular with Smith visibility and Schlick Fresnel, and Lambert diffuse weighted by the energy the specular term leaves. Returns the
// outgoing radiance per unit incoming radiance from a light in the direction L.
float3 DirectBrdf(float3 n, float3 v, float3 L, float3 albedo, float3 f0, float metallic, float roughness)
{
    float3 h = normalize(v + L);
    float n_dot_l = saturate(dot(n, L));
    float n_dot_v = max(dot(n, v), 1e-4);
    float n_dot_h = saturate(dot(n, h));
    float v_dot_h = saturate(dot(v, h));
    float a = max(roughness * roughness, 0.002);
    float a2 = a * a;
    float denom = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    float d = a2 / (kPi * denom * denom);
    float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    float g = (n_dot_l / (n_dot_l * (1.0 - k) + k)) * (n_dot_v / (n_dot_v * (1.0 - k) + k));
    float3 f = f0 + (1.0 - f0) * pow(1.0 - v_dot_h, 5.0);
    float3 specular = d * g * f / max(4.0 * n_dot_l * n_dot_v, 1e-4);
    float3 diffuse = (1.0 - f) * (1.0 - metallic) * albedo / kPi;
    return (diffuse + specular) * n_dot_l;
}

// The debug colors of the cascade index, and black for a point outside every cascade.
float3 CascadeColor(uint used)
{
    if (used == 0) return float3(1.0, 0.0, 0.0);
    if (used == 1) return float3(0.0, 1.0, 0.0);
    if (used == 2) return float3(0.0, 0.0, 1.0);
    if (used == 3) return float3(1.0, 1.0, 0.0);
    return float3(0.0, 0.0, 0.0);
}

// Reads a ray-traced target at a pixel. The dummy targets are 1x1 (US-043), so the coordinate is clamped to the texture's size.
float4 LoadTraced(Texture2D<float4> map, float2 pixel)
{
    uint width;
    uint height;
    map.GetDimensions(width, height);
    return map.Load(int3(min(int2(pixel), int2(width, height) - 1), 0));
}

// Direct lighting of one fragment: every light in the fragment's cluster, with shadows. The outputs feed the debug views: the cluster's
// light count and overflow flag, the cascade used by the last directional light that covered the point, and the product of the
// visibilities of every shadowed light.
float3 ShadeDirectLights(float4 pixel, float3 world, float3 n, float3 v, float3 albedo, float3 f0,
                         float metallic, float roughness, out uint cluster_count, out bool cluster_overflow, out uint cascade_used,
                         out float shadow_product)
{
    uint tiles_x = LightingData.Load(kHeaderTilesX);
    uint tiles_y = LightingData.Load(kHeaderTilesY);
    uint slices = LightingData.Load(kHeaderSlices);
    uint max_per = LightingData.Load(kHeaderMaxPerCluster);
    float tile_px = asfloat(LightingData.Load(kHeaderTilePx));
    float near_plane = asfloat(LightingData.Load(kHeaderNear));
    float slice_scale = asfloat(LightingData.Load(kHeaderSliceScale));
    uint light_base = LightingData.Load(kHeaderLightBase);
    uint shadow_base = LightingData.Load(kHeaderShadowBase);
    uint count_base = LightingData.Load(kHeaderCountBase);
    uint index_base = LightingData.Load(kHeaderIndexBase);
    uint light_count = LightingData.Load(kHeaderLightCount);
    float3 forward = asfloat(LightingData.Load3(kHeaderForward));

    // Cluster of the fragment: its screen tile, then its depth slice (the same formula as ClusterGrid::slice_of).
    float view_depth = dot(world, forward);
    uint tx = min((uint)pixel.x / (uint)tile_px, tiles_x - 1);
    uint ty = min((uint)pixel.y / (uint)tile_px, tiles_y - 1);
    uint slice = 0;
    if (view_depth > near_plane)
    {
        slice = min((uint)floor(log(view_depth / near_plane) * slice_scale), slices - 1);
    }
    uint cluster = (slice * tiles_y + ty) * tiles_x + tx;
    uint word = LightingData.Load(count_base + 4 * cluster);
    cluster_count = word & 0xffffu;
    cluster_overflow = (word & 0x10000u) != 0;

    float3 radiance = float3(0.0, 0.0, 0.0);
    cascade_used = kNoShadow;
    shadow_product = 1.0;
    [loop]
    for (uint i = 0; i < cluster_count; ++i)
    {
        uint light = LightingData.Load(index_base + 4 * (cluster * max_per + i));
        uint base = light_base + 64 * light;
        float4 a = asfloat(LightingData.Load4(base));
        float4 b = asfloat(LightingData.Load4(base + 16));
        float4 c = asfloat(LightingData.Load4(base + 32));
        float4 d = asfloat(LightingData.Load4(base + 48));
        uint type = asuint(b.w);
        float3 position = a.xyz;
        float range = a.w;
        float3 direction = b.xyz;
        float3 color = c.rgb;
        float intensity = c.w;
        float cos_inner = d.x;
        float cos_outer = d.y;
        uint shadow_first = asuint(d.z);
        uint shadow_count = asuint(d.w);

        float3 L;
        float scale;
        if (type == kLightDirectional)
        {
            L = -direction;
            scale = intensity;
        }
        else
        {
            float3 to_light = position - world;
            float distance = length(to_light);
            L = to_light / max(distance, 1e-6);
            float cos_angle = dot(direction, -L);
            scale = LightScale(type, intensity, range, distance, cos_angle, cos_inner, cos_outer);
        }
        if (scale <= 0.0 || dot(n, L) <= 0.0)
        {
            continue;
        }

        float visibility = 1.0;
        if (shadow_first != kNoShadow && shadow_count > 0)
        {
            if (type == kLightDirectional)
            {
                uint used;
                visibility = CascadeVisibility(shadow_base, shadow_first, shadow_count, view_depth, world, used);
                if (used != kNoShadow)
                {
                    cascade_used = used;
                }
            }
            else
            {
                visibility = ShadowRecordVisibility(shadow_base, shadow_first, world);
            }
            shadow_product *= visibility;
        }
        // The traced shadow is the sun's: it applies to a directional light whether or not the light has a shadow map (US-043).
        if (type == kLightDirectional)
        {
            float traced = LoadTraced(RtShadow, pixel.xy).x;
            visibility *= traced;
            shadow_product *= traced;
        }
        radiance += DirectBrdf(n, v, L, albedo, f0, metallic, roughness) * color * scale * visibility;
    }
    return radiance;
}

struct VSInput
{
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    uint instance : SV_InstanceID;
};

// A skinned vertex (US-036): the joints of its four influences packed one byte each (low byte first), and their weights.
struct SkinnedVSInput
{
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    uint joints : TEXCOORD3;
    float4 weights : TEXCOORD4;
    uint instance : SV_InstanceID;
};

struct VSOutput
{
    float4 position : SV_Position;
    nointerpolation float4 tint : COLOR0;
    nointerpolation uint pick : TEXCOORD1;
    float3 world : TEXCOORD2;
    float3 normal : TEXCOORD3;
    float2 uv : TEXCOORD4;
    float4 clip : TEXCOORD5;       // this frame's clip position, for the motion vector
    float4 prev_clip : TEXCOORD6;  // the previous frame's clip position of the same point
    float4 splat : TEXCOORD7;      // terrain splat weights (US-047); zero for every other mesh
};

// A terrain vertex (US-047): the static attributes, then the four normalised splat weights at TEXCOORD3, in the terrain stride.
struct TerrainVSInput
{
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 splat : TEXCOORD3;
    uint instance : SV_InstanceID;
};

// The albedo of the four splat layers (US-047): grass, rock, sand, and snow. They match splat_layer_colour in
// engine/render/src/terrain_mesh.cpp, which the terrain GPU fixture reads back.
float3 SplatAlbedo(float4 weights)
{
    return weights.x * float3(0.25, 0.45, 0.15) + weights.y * float3(0.45, 0.42, 0.40) + weights.z * float3(0.78, 0.70, 0.50) +
           weights.w * float3(0.92, 0.94, 0.96);
}

// Linear blend skinning (US-036): the weighted sum of each influence's palette matrix applied to the point. The palette is at `palette`, one
// 64-byte matrix per joint. Influences with zero weight are skipped, so their joint indices are never read.
float3 BlendPoint(uint palette, uint joints, float4 weights, float3 p)
{
    float3 result = float3(0.0, 0.0, 0.0);
    for (uint i = 0; i < 4; ++i)
    {
        if (weights[i] > 0.0)
        {
            uint base = palette + 64 * ((joints >> (8 * i)) & 0xff);
            float4 c0 = asfloat(FrameData.Load4(base));
            float4 c1 = asfloat(FrameData.Load4(base + 16));
            float4 c2 = asfloat(FrameData.Load4(base + 32));
            float4 c3 = asfloat(FrameData.Load4(base + 48));
            result += weights[i] * (c0.xyz * p.x + c1.xyz * p.y + c2.xyz * p.z + c3.xyz);
        }
    }
    return result;
}

// The same blend for a direction: the palettes' upper 3x3, without translation.
float3 BlendDirection(uint palette, uint joints, float4 weights, float3 n)
{
    float3 result = float3(0.0, 0.0, 0.0);
    for (uint i = 0; i < 4; ++i)
    {
        if (weights[i] > 0.0)
        {
            uint base = palette + 64 * ((joints >> (8 * i)) & 0xff);
            float3 c0 = asfloat(FrameData.Load4(base)).xyz;
            float3 c1 = asfloat(FrameData.Load4(base + 16)).xyz;
            float3 c2 = asfloat(FrameData.Load4(base + 32)).xyz;
            result += weights[i] * (c0 * n.x + c1 * n.y + c2 * n.z);
        }
    }
    return result;
}

// One vertex of an item. `position` and `normal` are in the object space of this frame's pose; `previous_position` is the same vertex in the
// previous frame's pose. Both are then placed by the item's model matrices.
VSOutput MakeVertex(uint record, float3 position, float3 normal, float3 previous_position, float2 uv)
{
    float4 camera_space = TransformPoint(record, position);
    float4 previous_camera_space = TransformPoint(record + kRecordPreviousModel, previous_position);
    VSOutput output;
    output.position = TransformPoint(0, camera_space.xyz);
    output.clip = output.position;
    // The TAA jitter is an NDC offset (zero when TAA is off). It moves only the rasterized position: the motion vector keeps the clip
    // position above, so the jitter never shows up as motion.
    output.position.xy += asfloat(FrameData.Load2(kFrameJitter)) * output.position.w;
    output.prev_clip = TransformPoint(kPreviousViewProjection, previous_camera_space.xyz);
    output.tint = asfloat(FrameData.Load4(record + 112));
    output.pick = FrameData.Load(record + 128);
    output.world = camera_space.xyz;
    output.normal = TransformNormal(record + 64, normal);
    output.uv = uv;
    output.splat = float4(0.0, 0.0, 0.0, 0.0);
    return output;
}

VSOutput VSMain(VSInput input)
{
    uint record = kItemsOffset + input.instance * kItemStride;
    return MakeVertex(record, input.position, input.normal, input.position, input.uv);
}

// A terrain tile (US-047), drawn directly: the splat weights ride along to the pixel stage, which tints the albedo with them.
VSOutput VSTerrain(TerrainVSInput input)
{
    uint record = kItemsOffset + input.instance * kItemStride;
    VSOutput output = MakeVertex(record, input.position, input.normal, input.position, input.uv);
    output.splat = input.splat;
    return output;
}

VSOutput SkinnedVertex(uint record, uint joints, float4 weights, float3 position_in, float3 normal_in, float2 uv)
{
    uint palette = FrameData.Load(record + kRecordPalette);
    uint previous = palette + 64 * FrameData.Load(record + kRecordBones);
    float3 position = BlendPoint(palette, joints, weights, position_in);
    float3 normal = BlendDirection(palette, joints, weights, normal_in);
    float3 previous_position = BlendPoint(previous, joints, weights, position_in);
    return MakeVertex(record, position, normal, previous_position, uv);
}

VSOutput VSSkinned(SkinnedVSInput input)
{
    uint record = kItemsOffset + input.instance * kItemStride;
    return SkinnedVertex(record, input.joints, input.weights, input.position, input.normal, input.uv);
}

// Indirect draws (US-037). The record is chosen by the per-instance attribute `item`, which the draw sets through its
// first_instance on both backends. SV_InstanceID would not carry it on DX12. The frame buffer of an indirect draw holds the candidates in
// order, one record each, so `item` is the candidate's index.
// The item's semantic is the next index after the vertex inputs: TEXCOORD3 for a static mesh and TEXCOORD5 for a skinned one. DXC gives the
// SPIR-V locations in declaration order, so the semantic index is also the location on Vulkan (the renderer's per-instance attribute uses
// the same index; scene_renderer.cpp).
struct IndirectVSInput
{
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    uint item : TEXCOORD3;
};

struct IndirectSkinnedVSInput
{
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    uint joints : TEXCOORD3;
    float4 weights : TEXCOORD4;
    uint item : TEXCOORD5;
};

VSOutput VSIndirect(IndirectVSInput input)
{
    uint record = kItemsOffset + input.item * kItemStride;
    return MakeVertex(record, input.position, input.normal, input.position, input.uv);
}

VSOutput VSSkinnedIndirect(IndirectSkinnedVSInput input)
{
    uint record = kItemsOffset + input.item * kItemStride;
    return SkinnedVertex(record, input.joints, input.weights, input.position, input.normal, input.uv);
}

// GPU culling (US-037). CSCull tests each candidate's bounding sphere against the six frustum planes, with the CPU's test
// (camera.cpp, sphere_visible). A visible candidate takes the next slot of its group's arguments, and the group's count grows by one. The
// CullData buffer (set 1, t0) holds a header, the group table, and each candidate's group. Each record is a DrawIndexedIndirect of 20 bytes,
// with the candidate's index as its first_instance. A count past the group's capacity is kept, and the draw is clamped to the capacity.
// Set 1 binds CullData at t0, CullArgs at u1, and CullCounts at u2: each binding number is the register number (scene_renderer.cpp).
ByteAddressBuffer CullData : register(t0, space1);
RWByteAddressBuffer CullArgs : register(u1, space1);
RWByteAddressBuffer CullCounts : register(u2, space1);

static const uint kCullPlanes = 0;        // six float4 planes, the first at 0
static const uint kCullItems = 96;        // candidate count
static const uint kCullGroups = 100;      // group count
static const uint kCullCapacity = 104;    // records per group in CullArgs
static const uint kCullEnabled = 108;     // zero draws every candidate
static const uint kCullGroupBase = 112;   // byte offset of the group table
static const uint kCullItemBase = 116;    // byte offset of the candidate groups
static const uint kCullGroupStride = 32;  // index count, radius, center, padding
static const uint kDrawRecordStride = 20;

[numthreads(64, 1, 1)]
void CSCull(uint3 id : SV_DispatchThreadID)
{
    uint item = id.x;
    if (item >= CullData.Load(kCullItems))
    {
        return;
    }
    uint group = CullData.Load(CullData.Load(kCullItemBase) + 4 * item);
    uint group_record = CullData.Load(kCullGroupBase) + kCullGroupStride * group;
    uint record = kItemsOffset + item * kItemStride;
    if (CullData.Load(kCullEnabled) != 0)
    {
        // The sphere in camera space: the mesh's center through the item's model, with the radius scaled by its longest column, as the CPU does.
        float radius = asfloat(CullData.Load(group_record + 4));
        float3 local = float3(asfloat(CullData.Load(group_record + 8)), asfloat(CullData.Load(group_record + 12)), asfloat(CullData.Load(group_record + 16)));
        float3 center = TransformPoint(record, local).xyz;
        float3 c0 = asfloat(FrameData.Load4(record)).xyz;
        float3 c1 = asfloat(FrameData.Load4(record + 16)).xyz;
        float3 c2 = asfloat(FrameData.Load4(record + 32)).xyz;
        float scale = max(length(c0), max(length(c1), length(c2)));
        for (uint plane = 0; plane < 6; ++plane)
        {
            float4 p = asfloat(CullData.Load4(kCullPlanes + 16 * plane));
            if (dot(p.xyz, center) + p.w < -radius * scale)
            {
                return;
            }
        }
    }
    uint slot;
    CullCounts.InterlockedAdd(4 * group, 1, slot);
    uint capacity = CullData.Load(kCullCapacity);
    if (slot < capacity)
    {
        uint out_offset = (group * capacity + slot) * kDrawRecordStride;
        CullArgs.Store4(out_offset, uint4(CullData.Load(group_record), 1, 0, 0));
        CullArgs.Store(out_offset + 16, item);
    }
}

// Mesh path (US-038). A static opaque group is drawn by one DispatchMesh of its task groups. The frame slot holds the group's camera (as
// above), its header at 200 (the group's first meshlet record, its meshlet count, its item count, the first visibility counter of its task
// groups, and how many task groups it has), and the group's items. The pool buffers (set 0, t1..t4) hold every attached mesh's cooked meshlets, back to back, so one
// set serves every mesh. The task stage (ASMeshlet) runs one thread per (item, meshlet) pair: a pair that fails the frustum or cone test is
// dropped, and the survivors go to the mesh stage as its payload. MSMeshlet loads one visible meshlet, writes its vertices with the same
// transform as VSMain, and writes its triangles. Each task group also stores how many of its pairs survived (MeshletCounts), so the
// renderer reads the GPU's culling result back without a clear pass. The tests in render_meshlet_gpu_tests.cpp check it against the CPU.
static const uint kFrameMeshletBase = 200;
static const uint kFrameMeshletCount = 204;
static const uint kFrameItemCount = 208;
static const uint kFrameCountsBase = 212;
static const uint kFrameTaskGroups = 216;
static const uint kFrameCullCones = 220;  // nonzero for one-sided groups: a two-sided group's back faces draw, so its cones never cull
static const uint kMeshletStride = 48;     // vertex offset, vertex count, triangle offset (bytes), triangle count, center, radius, cone axis, cone cutoff
static const uint kMeshVertexStride = 32;  // position, normal, texture coordinate
static const uint kTaskThreads = 32;
static const uint kMeshThreads = 128;
static const uint kTaskGroupsX = 65535;    // the flat task group index is group.y * kTaskGroupsX + group.x

ByteAddressBuffer MeshletRecords : register(t1, space0);
ByteAddressBuffer MeshletVertexIndices : register(t2, space0);
ByteAddressBuffer MeshletTriangleBytes : register(t3, space0);
ByteAddressBuffer MeshVertexPool : register(t4, space0);
RWByteAddressBuffer MeshletCounts : register(u5, space0);

struct MeshletPayload
{
    uint count;          // surviving pairs in this task group
    uint pair[kTaskThreads];  // item * meshlet count + meshlet, for each survivor
};

// The survivors of one task group. The count and the pairs are plain groupshared variables, and the payload is assembled from them after the
// barrier: an atomic on a member of the payload struct lost its update in the DXC output (US-038 bring-up).
groupshared MeshletPayload meshlet_payload;
groupshared uint survivor_count;
groupshared uint survivor_pairs[kTaskThreads];

// The six frustum planes of the frame's view-projection, normalized, with the same extraction as camera.cpp (make_frustum).
float4 FrustumPlane(uint plane)
{
    float4 c0 = asfloat(FrameData.Load4(0));
    float4 c1 = asfloat(FrameData.Load4(16));
    float4 c2 = asfloat(FrameData.Load4(32));
    float4 c3 = asfloat(FrameData.Load4(48));
    float4 r0 = float4(c0.x, c1.x, c2.x, c3.x);
    float4 r1 = float4(c0.y, c1.y, c2.y, c3.y);
    float4 r2 = float4(c0.z, c1.z, c2.z, c3.z);
    float4 r3 = float4(c0.w, c1.w, c2.w, c3.w);
    float4 p = r3 + r0;  // left
    if (plane == 1) p = r3 - r0;  // right
    else if (plane == 2) p = r3 + r1;  // bottom
    else if (plane == 3) p = r3 - r1;  // top
    else if (plane == 4) p = r2;  // near: depth 0
    else if (plane == 5) p = r3 - r2;  // far: depth 1
    return p / length(p.xyz);
}

// A meshlet is visible when its bounding sphere is not behind a frustum plane, and its normal cone faces the eye. The meshlet is in camera
// space through the item's model (the model is camera-relative, so the eye is the origin). The cone test is meshoptimizer's sphere form,
// dot(center - eye, axis) >= cutoff * |center - eye| + radius, which rejects the meshlet when every triangle faces away from the eye.
bool MeshletVisible(uint record, uint meshlet_index)
{
    uint address = meshlet_index * kMeshletStride;
    float3 local_center = asfloat(MeshletRecords.Load3(address + 16));
    float radius = asfloat(MeshletRecords.Load(address + 28));
    float3 center = TransformPoint(record, local_center).xyz;
    // The longest column scales the radius, as CSCull does for its groups.
    float3 c0 = asfloat(FrameData.Load4(record)).xyz;
    float3 c1 = asfloat(FrameData.Load4(record + 16)).xyz;
    float3 c2 = asfloat(FrameData.Load4(record + 32)).xyz;
    float scaled_radius = radius * max(length(c0), max(length(c1), length(c2)));
    for (uint plane = 0; plane < 6; ++plane)
    {
        float4 p = FrustumPlane(plane);
        if (dot(p.xyz, center) + p.w < -scaled_radius)
        {
            return false;
        }
    }
    float cutoff = asfloat(MeshletRecords.Load(address + 44));
    if (cutoff < 1.0 && FrameData.Load(kFrameCullCones) != 0)
    {
        float3 axis = normalize(TransformNormal(record + 64, asfloat(MeshletRecords.Load3(address + 32))));
        if (dot(center, axis) >= cutoff * length(center) + scaled_radius)
        {
            return false;
        }
    }
    return true;
}

[numthreads(kTaskThreads, 1, 1)]
void ASMeshlet(uint3 group : SV_GroupID, uint thread : SV_GroupIndex)
{
    if (thread == 0)
    {
        survivor_count = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    uint meshlet_count = FrameData.Load(kFrameMeshletCount);
    uint item_count = FrameData.Load(kFrameItemCount);
    uint group_index = group.y * kTaskGroupsX + group.x;
    uint pair = group_index * kTaskThreads + thread;
    if (meshlet_count > 0 && pair < item_count * meshlet_count)
    {
        uint item = pair / meshlet_count;
        uint meshlet = pair - item * meshlet_count;
        if (MeshletVisible(kItemsOffset + item * kItemStride, FrameData.Load(kFrameMeshletBase) + meshlet))
        {
            uint slot;
            InterlockedAdd(survivor_count, 1, slot);
            survivor_pairs[slot] = pair;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // The payload is assembled after the barrier, from the plain groupshared count and pairs.
    uint survivors = survivor_count;
    meshlet_payload.pair[thread] = survivor_pairs[thread];
    if (thread == 0)
    {
        meshlet_payload.count = survivors;
    }
    GroupMemoryBarrierWithGroupSync();

    // Groups past the draw's task group count hold no pairs, so they store nothing; their counter slots belong to a later draw.
    if (thread == 0 && group_index < FrameData.Load(kFrameTaskGroups))
    {
        MeshletCounts.Store(4 * (FrameData.Load(kFrameCountsBase) + group_index), survivors);
    }
    DispatchMesh(survivors, 1, 1, meshlet_payload);
}

// One payload entry's byte of the pool's triangle data. Byte addresses are not aligned, so the word is shifted down to the byte.
uint MeshletTriangleByte(uint address)
{
    uint word = MeshletTriangleBytes.Load(address & ~3u);
    return (word >> (8 * (address & 3u))) & 0xff;
}

[outputtopology("triangle")]
[numthreads(kMeshThreads, 1, 1)]
void MSMeshlet(uint3 group : SV_GroupID, uint thread : SV_GroupIndex, in payload MeshletPayload visible,
               out vertices VSOutput vertices[64], out indices uint3 triangles[124])
{
    uint meshlet_count = FrameData.Load(kFrameMeshletCount);
    uint pair = visible.pair[group.x];
    uint item = pair / meshlet_count;
    uint meshlet = FrameData.Load(kFrameMeshletBase) + (pair - item * meshlet_count);
    uint record = kItemsOffset + item * kItemStride;
    uint address = meshlet * kMeshletStride;
    uint vertex_offset = MeshletRecords.Load(address);
    uint vertex_count = MeshletRecords.Load(address + 4);
    uint triangle_offset = MeshletRecords.Load(address + 8);
    uint triangle_count = MeshletRecords.Load(address + 12);
    SetMeshOutputCounts(vertex_count, triangle_count);

    for (uint v = thread; v < vertex_count; v += kMeshThreads)
    {
        uint vertex = MeshletVertexIndices.Load(4 * (vertex_offset + v)) * kMeshVertexStride;
        float3 position = asfloat(MeshVertexPool.Load3(vertex));
        float3 normal = asfloat(MeshVertexPool.Load3(vertex + 12));
        float2 uv = asfloat(MeshVertexPool.Load2(vertex + 24));
        vertices[v] = MakeVertex(record, position, normal, position, uv);
    }
    for (uint t = thread; t < triangle_count; t += kMeshThreads)
    {
        uint corner = triangle_offset + 3 * t;
        triangles[t] = uint3(MeshletTriangleByte(corner), MeshletTriangleByte(corner + 1), MeshletTriangleByte(corner + 2));
    }
}

// Everything the shading needs at one fragment, computed the same way as pbr.cpp's shade_reference.
// `surface` and `base` take what the reflection pass needs later (US-040): the shading normal with the roughness, and the Fresnel base with
// the occlusion.
float4 ShadeFragment(VSOutput input, bool front_facing, out uint blend_mode, out float4 surface_out, out float4 base_out)
{
    float4 base_color = asfloat(MaterialData.Load4(0));
    float4 emissive_occlusion = asfloat(MaterialData.Load4(16));
    float4 factors = asfloat(MaterialData.Load4(32));  // metallic, roughness, normal scale, alpha cutoff
    uint4 modes = MaterialData.Load4(48);             // blend mode, two-sided
    blend_mode = modes.x;

    float3 normal = normalize(input.normal);
    float3 t;
    float3 b;
    TangentFrame(normal, input.world, input.uv, t, b);
    if (modes.y != 0 && !front_facing)
    {
        normal = -normal;
        b = -b;
    }

    float3 tangent_normal = FetchTexture(NormalMap, input.uv).rgb * 2.0 - 1.0;
    tangent_normal.xy *= factors.z;
    tangent_normal = length(tangent_normal) > 0.0 ? normalize(tangent_normal) : float3(0.0, 0.0, 1.0);
    float3 n = normalize(t * tangent_normal.x + b * tangent_normal.y + normal * tangent_normal.z);

    float4 base = FetchTexture(BaseColorMap, input.uv);
    float3 albedo = SrgbToLinear(base.rgb) * base_color.rgb * input.tint.rgb;
    // A terrain fragment (US-047) carries normalised splat weights, which sum to one; every other mesh carries zeros.
    if (dot(input.splat, float4(1.0, 1.0, 1.0, 1.0)) > 0.5)
    {
        albedo *= SplatAlbedo(input.splat);
    }
    float alpha = base.a * base_color.a * input.tint.a;

    float4 mr = FetchTexture(MetallicRoughnessMap, input.uv);
    float roughness = saturate(mr.g * factors.y);
    float metallic = saturate(mr.b * factors.x);

    float occlusion = 1.0 + emissive_occlusion.w * (FetchTexture(OcclusionMap, input.uv).r - 1.0);
    float3 emissive = SrgbToLinear(FetchTexture(EmissiveMap, input.uv).rgb) * emissive_occlusion.rgb;

    float3 view = normalize(-input.world);
    float n_dot_v = max(dot(n, view), 1e-4);
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float2 brdf = SampleBrdfLut(n_dot_v, roughness);
    float3 specular_weight = f0 * brdf.x + brdf.y;
    surface_out = float4(n, roughness);
    base_out = float4(f0, occlusion);

    // The baked probes replace the environment's diffuse where they cover the point (US-040); the coverage is the debug view.
    float probe_coverage;
    float3 irradiance = ProbeDiffuse(input.world, n, probe_coverage);
    float3 reflection = reflect(-view, n);
    float3 diffuse = albedo * (1.0 - metallic) * (1.0 - specular_weight) * irradiance * occlusion;
    float3 specular = SampleSpecular(reflection, roughness) * specular_weight * occlusion;

    // Direct lights of the fragment's cluster, and the debug view the lighting header selects.
    uint cluster_count;
    bool cluster_overflow;
    uint cascade_used;
    float shadow_product;
    float3 direct = ShadeDirectLights(input.position, input.world, n, view, albedo, f0, metallic, roughness, cluster_count,
                                      cluster_overflow, cascade_used, shadow_product);
    // The traced reflection (US-043) replaces the environment's specular where the ray hit geometry within its distance; a 0 alpha keeps it.
    float4 traced_reflection = LoadTraced(RtReflection, input.position.xy);
    specular = lerp(specular, traced_reflection.rgb * occlusion, traced_reflection.a);
    uint view_mode = LightingData.Load(kHeaderView);
    float max_per = (float)LightingData.Load(kHeaderMaxPerCluster);
    if (view_mode == kViewClusterCount)
    {
        return float4(float3(1.0, 1.0, 1.0) * ((float)cluster_count / max(max_per, 1.0)), 1.0);
    }
    if (view_mode == kViewClusterOverflow)
    {
        return float4(cluster_overflow ? float3(1.0, 0.0, 0.0) : float3(0.0, 0.0, 0.0), 1.0);
    }
    if (view_mode == kViewCascadeIndex)
    {
        return float4(CascadeColor(cascade_used), 1.0);
    }
    if (view_mode == kViewShadowFactor)
    {
        return float4(float3(1.0, 1.0, 1.0) * shadow_product, 1.0);
    }
    if (view_mode == kViewProbeCoverage)
    {
        // Red where no probe covers the point (the environment alone), green where all eight corners are valid.
        return float4(lerp(float3(1.0, 0.0, 0.0), float3(0.0, 1.0, 0.0), probe_coverage), 1.0);
    }
    return float4(diffuse + specular + emissive + direct, alpha);
}

struct PSOutput
{
    float4 color : SV_Target0;
    uint pick : SV_Target1;
    float depth : SV_Target2;
    float2 velocity : SV_Target3;
    // The reflection pass's inputs (US-040): the shading normal with the roughness, and the Fresnel base with the occlusion.
    float4 surface : SV_Target4;
    float4 base : SV_Target5;
};

PSOutput PSMain(VSOutput input, bool front_facing : SV_IsFrontFace)
{
    uint blend_mode;
    float4 surface_data;
    float4 base_data;
    float4 shaded = ShadeFragment(input, front_facing, blend_mode, surface_data, base_data);
    // The cutoff is the fourth float of the factors row (byte 44).
    if (blend_mode == kAlphaMask && shaded.a < asfloat(MaterialData.Load(44)))
    {
        discard;
    }
    PSOutput output;
    output.color = float4(LinearOutput() ? shaded.rgb : LinearToSrgb(shaded.rgb), 1.0);
    output.pick = input.pick;
    // NDC depth of the fragment. The depth buffer tests it, and this target keeps it so the host can read it back.
    output.depth = input.position.z;
    // Motion vector: the change in the point's screen position since the previous frame, in UV units (x right, y down). The scale is set per
    // backend, because Vulkan's NDC y points down.
    float2 now = input.clip.xy / input.clip.w;
    float2 before = input.prev_clip.xy / input.prev_clip.w;
    output.velocity = (now - before) * asfloat(FrameData.Load2(kVelocityScale));
    output.surface = surface_data;
    output.base = base_data;
    return output;
}

struct PSBlendOutput
{
    float4 color : SV_Target0;
};

PSBlendOutput PSBlend(VSOutput input, bool front_facing : SV_IsFrontFace)
{
    uint blend_mode;
    float4 surface_data;
    float4 base_data;
    float4 shaded = ShadeFragment(input, front_facing, blend_mode, surface_data, base_data);
    PSBlendOutput output;
    output.color = float4(LinearOutput() ? shaded.rgb : LinearToSrgb(shaded.rgb), saturate(shaded.a));
    return output;
}

// Shadow tiles (US-035). A shadow draw uses the frame buffer of its tile: the camera slot holds the tile's clip matrix (position) and the
// light's clip matrix, unremapped (at byte 64), so the pixel stage can discard what lies outside the light's frustum. Those fragments
// would otherwise land in a neighboring tile of the atlas.
struct ShadowVSOutput
{
    float4 position : SV_Position;
    float4 light_clip : TEXCOORD0;
};

ShadowVSOutput ShadowVertex(uint record, float3 position)
{
    float4 camera_space = TransformPoint(record, position);
    ShadowVSOutput output;
    output.position = TransformPoint(0, camera_space.xyz);
    output.light_clip = TransformPoint(64, camera_space.xyz);
    return output;
}

ShadowVSOutput VSShadow(VSInput input)
{
    return ShadowVertex(kItemsOffset + input.instance * kItemStride, input.position);
}

// A terrain tile's caster (US-047): the same tile as VSShadow, over the terrain stride. The splat is not needed for depth.
ShadowVSOutput VSTerrainShadow(TerrainVSInput input)
{
    return ShadowVertex(kItemsOffset + input.instance * kItemStride, input.position);
}

ShadowVSOutput VSSkinnedShadow(SkinnedVSInput input)
{
    uint record = kItemsOffset + input.instance * kItemStride;
    uint palette = FrameData.Load(record + kRecordPalette);
    return ShadowVertex(record, BlendPoint(palette, input.joints, input.weights, input.position));
}

float PSShadow(ShadowVSOutput input) : SV_Target0
{
    if (input.light_clip.w <= 0.0)
    {
        discard;
    }
    float3 ndc = input.light_clip.xyz / input.light_clip.w;
    if (any(abs(ndc.xy) > 1.0) || ndc.z < 0.0 || ndc.z > 1.0)
    {
        discard;
    }
    return ndc.z;
}

