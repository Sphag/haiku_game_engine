// Hit/miss fixture for the DX12 ray tracing foundation (US-041). Built as a DXIL library (lib_6_3); the GPU suite loads it through
// rhi_dx12_gpu_tests.cpp. Each output element is one ray cast from the near plane (z = -1) along +z. The fixture quad covers
// x in [-1, 0] and y in [-1, 1] at z = 0, so a ray hits it when its x is negative. The closest-hit shader writes 1, and the miss
// shader writes 0. Binding layout: set 0 binding 0 is the top-level structure, set 0 binding 1 is the output (keep in step with the test).

RaytracingAccelerationStructure scene : register(t0, space0);
RWByteAddressBuffer output : register(u1, space0);

struct Payload {
    uint hit;
};

[shader("raygeneration")]
void RayGen() {
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 size = DispatchRaysDimensions().xy;

    RayDesc ray;
    ray.Origin = float3((pixel.x + 0.5f) / size.x * 2.0f - 1.0f, 1.0f - (pixel.y + 0.5f) / size.y * 2.0f, -1.0f);
    ray.Direction = float3(0.0f, 0.0f, 1.0f);
    ray.TMin = 0.0f;
    ray.TMax = 10.0f;

    Payload payload;
    payload.hit = 0;
    TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray, payload);
    // The output is a raw view (write_buffer), so the element is addressed in bytes.
    output.Store((pixel.y * size.x + pixel.x) * 4, payload.hit);
}

[shader("miss")]
void Miss(inout Payload payload) {
    payload.hit = 0;
}

[shader("closesthit")]
void ClosestHit(inout Payload payload, in BuiltInTriangleIntersectionAttributes attributes) {
    payload.hit = 1;
}
