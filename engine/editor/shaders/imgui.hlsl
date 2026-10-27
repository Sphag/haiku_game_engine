// Dear ImGui through the RHI (US-061). The CPU expands each draw into vertices that already carry NDC positions and their clip
// rectangle in framebuffer pixels. The RHI has no per-draw scissor, so the pixel stage discards fragments outside that rectangle.
// The font atlas is read with Load at texel coordinates, so the RHI needs no sampler.
//
// Vertex layout (36 bytes, kept in sync with ImGuiVertex in imgui_renderer.hpp): position, uv, packed color, clip rectangle.

Texture2D<float4> FontAtlas : register(t0);

struct VSInput {
    float2 position : TEXCOORD0;
    float2 uv : TEXCOORD1;
    uint color : TEXCOORD2;  // RGBA8 packed in a uint; the RHI takes no 8-bit vertex formats
    float4 clip : TEXCOORD3;
};

struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
    float4 clip : TEXCOORD2;
};

VSOutput VSMain(VSInput input) {
    VSOutput output;
    output.position = float4(input.position, 0.0, 1.0);
    output.uv = input.uv;
    // The bytes of the packed color are R, G, B, A from the low byte up, as ImGui writes them.
    output.color = float4(input.color & 0xFF, (input.color >> 8) & 0xFF, (input.color >> 16) & 0xFF, (input.color >> 24) & 0xFF) / 255.0;
    output.clip = input.clip;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target0 {
    if (input.position.x < input.clip.x || input.position.y < input.clip.y || input.position.x >= input.clip.z ||
        input.position.y >= input.clip.w) {
        discard;
    }
    uint width = 0;
    uint height = 0;
    FontAtlas.GetDimensions(width, height);
    const float2 size = float2(width, height);
    const int2 texel = int2(clamp(floor(input.uv * size), float2(0.0, 0.0), size - 1.0));
    return FontAtlas.Load(int3(texel, 0)) * input.color;
}
