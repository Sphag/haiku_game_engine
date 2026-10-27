// Shader toolchain diagnostic fixture (US-003): must fail to compile on line 7.
RWStructuredBuffer<float> Output : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    Output[id.x] = undefined_symbol;
}
