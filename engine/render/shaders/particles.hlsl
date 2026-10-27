// GPU particles (US-051): the simulation, the indirect-argument, and the billboard entries that ParticleSystem (engine/render/src/particles.cpp)
// dispatches and draws. pack_particle_parameters in particles.cpp writes the parameter block; keep the two in step.
//
// Descriptor sets (the binding is the register number; the register numbers are distinct across the file's globals):
//   simulate (CSParticleSimulate):  t0 Params, u1 ParticlePool (32 bytes a slot), u2 DrawList (one index per live slot), u3 LiveCounter
//   args (CSParticleArgs):          t4 LiveCounterRead, u5 IndirectArgs (DrawIndexedIndirect: 6, live count, 0, 0, 0)
//   render (VSParticle, PSParticle): t0 Params, t1 ParticlePoolRead, t2 DrawListRead, t3 Sprite (the billboard texture)
//
// Every buffer is raw (ByteAddressBuffer or RWByteAddressBuffer), because the DX12 buffer descriptors are raw views.
//
// Particle record (32 bytes at index * 32): float4 position_age (xyz position, w age), float4 velocity_life (xyz velocity, w life). A slot is
// alive while age < life, so a zeroed slot is dead.
//
// Parameters, in bytes (floats unless noted):
//   0    emitter position (xyz), spawn radius (w)
//   16   direction (xyz, unit), cos(spread) (w)
//   32   speed min, speed max, life min, life max
//   48   gravity (xyz)
//   64   bounds min (xyz)
//   80   bounds max (xyz)
//   96   colour key times (8 floats)
//   128  colour keys (8 float4, rgba)
//   256  size key times (8 floats)
//   288  size keys (8 float4; x is the size in meters)
//   416  uint: colour key count, size key count, additive (1) or alpha (0), 0
//   432  uint: capacity, spawn count, ring head, spawn serial base
//   448  float: dt
//   464  uint: kill all (1), seed, 0, 0
//   480  camera right (xyz)
//   496  camera up (xyz)
//   512  view-projection (four float4 columns, glm column-major)

RWByteAddressBuffer ParticlePool : register(u1, space0);
ByteAddressBuffer ParticlePoolRead : register(t1, space0);
RWByteAddressBuffer DrawListWrite : register(u2, space0);
ByteAddressBuffer DrawListRead : register(t2, space0);
RWByteAddressBuffer LiveCounter : register(u3, space0);
ByteAddressBuffer LiveCounterRead : register(t4, space0);
RWByteAddressBuffer IndirectArgs : register(u5, space0);
ByteAddressBuffer Params : register(t0, space0);
Texture2D<float4> Sprite : register(t3, space0);

static const uint kParamEmitter = 0;
static const uint kParamDirection = 16;
static const uint kParamSpeedLife = 32;
static const uint kParamGravity = 48;
static const uint kParamBoundsMin = 64;
static const uint kParamBoundsMax = 80;
static const uint kParamColorTimes = 96;
static const uint kParamColors = 128;
static const uint kParamSizeTimes = 256;
static const uint kParamSizes = 288;
static const uint kParamCounts = 416;
static const uint kParamSim = 432;
static const uint kParamTiming = 448;
static const uint kParamFlags = 464;
static const uint kParamCameraRight = 480;
static const uint kParamCameraUp = 496;
static const uint kParamViewProjection = 512;

static const uint kParticleBytes = 32;
static const float kPi = 3.14159265358979323846;

float ParamFloat(uint offset)
{
    return asfloat(Params.Load(offset));
}

uint ParamUint(uint offset)
{
    return Params.Load(offset);
}

float3 ParamVec3(uint offset)
{
    return asfloat(Params.Load3(offset));
}

float4 ParamVec4(uint offset)
{
    return asfloat(Params.Load4(offset));
}

// The curve at age fraction t: the first value before its first key, the last after its last, linear between.
float4 EvalCurve(uint timesOffset, uint valuesOffset, uint count, uint stride, float t)
{
    if (count == 0)
    {
        return float4(1.0, 1.0, 1.0, 1.0);
    }
    if (t <= ParamFloat(timesOffset))
    {
        return ParamVec4(valuesOffset);
    }
    [loop]
    for (uint i = 0; i + 1 < count; ++i)
    {
        float t0 = ParamFloat(timesOffset + 4 * i);
        float t1 = ParamFloat(timesOffset + 4 * (i + 1));
        if (t <= t1)
        {
            float w = saturate((t - t0) / max(t1 - t0, 1e-6));
            return lerp(ParamVec4(valuesOffset + stride * i), ParamVec4(valuesOffset + stride * (i + 1)), w);
        }
    }
    return ParamVec4(valuesOffset + stride * (count - 1));
}

// A 32-bit hash (the finalizer of MurmurHash3). Its output is the same on every call for the same input.
uint Hash32(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// A value in [0, 1) for one channel of one spawn. The seed and the spawn serial pick the sequence.
float Random(uint seed, uint serial, uint channel)
{
    uint h = Hash32(seed ^ Hash32(serial * 8u + channel + 0x9e3779b9u));
    return float(h >> 8) * (1.0 / 16777216.0);
}

// Writes a fresh particle for spawn `serial` into slot `index`.
void SpawnParticle(uint index, uint serial, uint seed)
{
    float3 emitter = ParamVec3(kParamEmitter);
    float radius = ParamFloat(kParamEmitter + 12);
    float3 offset = (float3(Random(seed, serial, 0), Random(seed, serial, 1), Random(seed, serial, 2)) * 2.0 - 1.0) * radius;

    float3 axis = normalize(ParamVec3(kParamDirection));
    float cosSpread = ParamFloat(kParamDirection + 12);
    float cosTheta = lerp(cosSpread, 1.0, Random(seed, serial, 3));
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    float phi = 2.0 * kPi * Random(seed, serial, 4);
    float3 helper = abs(axis.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float3 tangent = normalize(cross(helper, axis));
    float3 bitangent = cross(axis, tangent);
    float3 direction = sinTheta * cos(phi) * tangent + sinTheta * sin(phi) * bitangent + cosTheta * axis;

    float speed = lerp(ParamFloat(kParamSpeedLife), ParamFloat(kParamSpeedLife + 4), Random(seed, serial, 5));
    float life = lerp(ParamFloat(kParamSpeedLife + 8), ParamFloat(kParamSpeedLife + 12), Random(seed, serial, 6));

    uint offset_bytes = index * kParticleBytes;
    ParticlePool.Store4(offset_bytes, asuint(float4(emitter + offset, 0.0)));
    ParticlePool.Store4(offset_bytes + 16, asuint(float4(direction * speed, life)));
}

// Simulate: one thread per ring slot. A slot in this frame's spawn range is re-initialised; a reset kills the slot; otherwise a live slot
// integrates and is appended to the draw list. Each thread writes only its own slot, so the pool needs no synchronisation.
[numthreads(64, 1, 1)]
void CSParticleSimulate(uint3 id : SV_DispatchThreadID)
{
    uint capacity = ParamUint(kParamSim);
    if (id.x >= capacity)
    {
        return;
    }
    uint spawnCount = ParamUint(kParamSim + 4);
    uint head = ParamUint(kParamSim + 8);
    uint serialBase = ParamUint(kParamSim + 12);
    uint seed = ParamUint(kParamFlags + 4);
    uint index = id.x;
    uint ordinal = (index + capacity - head) % capacity;
    uint offset = index * kParticleBytes;
    if (ordinal < spawnCount)
    {
        // A spawned particle is alive (its lifetime is positive), so it is appended to the draw list below.
        SpawnParticle(index, serialBase + ordinal, seed);
    }
    else
    {
        if (ParamUint(kParamFlags) != 0)
        {
            ParticlePool.Store4(offset, uint4(0, 0, 0, 0));
            ParticlePool.Store4(offset + 16, uint4(0, 0, 0, 0));
            return;
        }
        float4 positionAge = asfloat(ParticlePool.Load4(offset));
        float4 velocityLife = asfloat(ParticlePool.Load4(offset + 16));
        if (!(positionAge.w < velocityLife.w))
        {
            return;
        }

        float dt = ParamFloat(kParamTiming);
        float3 velocity = velocityLife.xyz + ParamVec3(kParamGravity) * dt;
        float3 position = positionAge.xyz + velocity * dt;
        float age = positionAge.w + dt;
        bool inside = all(position >= ParamVec3(kParamBoundsMin)) && all(position <= ParamVec3(kParamBoundsMax));
        if (!inside || !(age < velocityLife.w))
        {
            ParticlePool.Store4(offset, uint4(0, 0, 0, 0));
            ParticlePool.Store4(offset + 16, uint4(0, 0, 0, 0));
            return;
        }
        ParticlePool.Store4(offset, asuint(float4(position, age)));
        ParticlePool.Store4(offset + 16, asuint(float4(velocity, velocityLife.w)));
    }

    uint slot;
    LiveCounter.InterlockedAdd(0, 1, slot);
    DrawListWrite.Store(slot * 4, index);
}

// Args: one thread writes the indirect record of the indexed draw from the live count. Six indices (a quad) per instance.
[numthreads(1, 1, 1)]
void CSParticleArgs()
{
    uint live = LiveCounterRead.Load(0);
    IndirectArgs.Store(0, 6);
    IndirectArgs.Store(4, live);
    IndirectArgs.Store(8, 0);
    IndirectArgs.Store(12, 0);
    IndirectArgs.Store(16, 0);
}

struct VSOut
{
    float4 position : SV_Position;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
};

// Billboard: vertex 0 to 3 are the corners of a quad (the index buffer is 0 1 2 0 2 3), and instance i draws the i-th live slot.
VSOut VSParticle(uint vertex : SV_VertexID, uint instance : SV_InstanceID)
{
    uint particle = DrawListRead.Load(instance * 4);
    float4 positionAge = asfloat(ParticlePoolRead.Load4(particle * kParticleBytes));
    float4 velocityLife = asfloat(ParticlePoolRead.Load4(particle * kParticleBytes + 16));
    float t = saturate(positionAge.w / max(velocityLife.w, 1e-6));

    float size = EvalCurve(kParamSizeTimes, kParamSizes, ParamUint(kParamCounts + 4), 16, t).x;
    float4 color = EvalCurve(kParamColorTimes, kParamColors, ParamUint(kParamCounts), 16, t);

    float2 corner = vertex == 0 ? float2(-1.0, -1.0) : (vertex == 1 ? float2(1.0, -1.0) : (vertex == 2 ? float2(1.0, 1.0) : float2(-1.0, 1.0)));
    float3 right = ParamVec3(kParamCameraRight);
    float3 up = ParamVec3(kParamCameraUp);
    float3 world = positionAge.xyz + (right * corner.x + up * corner.y) * (0.5 * size);

    float4 c0 = ParamVec4(kParamViewProjection);
    float4 c1 = ParamVec4(kParamViewProjection + 16);
    float4 c2 = ParamVec4(kParamViewProjection + 32);
    float4 c3 = ParamVec4(kParamViewProjection + 48);

    VSOut o;
    o.position = c0 * world.x + c1 * world.y + c2 * world.z + c3;
    o.color = color;
    // Texture coordinates run from the top-left, as the texture's rows do.
    o.uv = float2(corner.x * 0.5 + 0.5, 0.5 - corner.y * 0.5);
    return o;
}

float4 PSParticle(VSOut i) : SV_Target0
{
    uint width;
    uint height;
    Sprite.GetDimensions(width, height);
    uint2 texel = uint2(min(i.uv * float2(width, height), float2(width - 1, height - 1)));
    float4 color = Sprite.Load(int3(texel, 0)) * i.color;
    // The additive mode adds the premultiplied colour (the pipeline adds it with ONE and ONE).
    if (ParamUint(kParamCounts + 8) == 1)
    {
        color.rgb *= color.a;
    }
    return color;
}
