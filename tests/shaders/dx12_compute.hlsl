// DX12 resource fixture (US-029). Each register is the binding index of its descriptor in the test's layout: the constant buffer
// is binding 0 (b0), the input is binding 1 (t1), and the output is binding 2 (u2). Set 0 is register space 0.
cbuffer Params : register(b0)
{
    float Scale;
    float Offset;
    uint Count;
    uint Pad;
};

ByteAddressBuffer Input : register(t1);
RWByteAddressBuffer Output : register(u2);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x < Count)
    {
        float value = asfloat(Input.Load(id.x * 4));
        Output.Store(id.x * 4, asuint(value * Scale + Offset));
    }
}
