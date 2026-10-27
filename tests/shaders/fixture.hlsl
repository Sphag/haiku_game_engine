// Shader toolchain fixture (US-003): compiled to DXIL and SPIR-V by the build.
cbuffer Params : register(b0)
{
    float Scale;
    uint Count;
};

RWStructuredBuffer<float> Output : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x < Count)
    {
        Output[id.x] = float(id.x) * Scale;
    }
}
