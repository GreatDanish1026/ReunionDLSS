// Motion vectors for DLSS, in render pixels, pointing from the current pixel to where it was in
// the previous frame.
//
// The engine's velocity texture only has values for moving objects (x > 0 when written). For
// everything else the motion is the camera's: the pixel's clip position (screen xy, device z)
// through View.ClipToPrevClip gives its previous clip position. That's what TSR does itself.

cbuffer Params : register(b0) {
    float4 Row0, Row1, Row2, Row3;  // View.ClipToPrevClip, one row each (UE: row vector * matrix)
    float2 Size;                    // render size in pixels
    float2 InvSize;
};

Texture2D<float> Depth : register(t0);      // device z, reversed
Texture2D<float4> Velocity : register(t1);  // UE encoded velocity (rgba16unorm)
RWTexture2D<float2> Motion : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (any(float2(id) >= Size)) return;
    const float z = Depth[id];
    const float2 uv = (float2(id) + 0.5) * InvSize;
    const float2 screen = float2(uv.x * 2 - 1, 1 - uv.y * 2);
    const float4 prev = screen.x * Row0 + screen.y * Row1 + z * Row2 + Row3;
    float2 v = screen - prev.xy / prev.w;  // UE velocity: current minus previous, screen units
    const float4 e = Velocity[id];
    if (e.x > 0) {
        const float inv = 1.0 / (0.499 * 0.5);
        v = e.xy * inv - (32767.0 / 65535.0) * inv;
    }
    Motion[id] = -v * float2(0.5 * Size.x, -0.5 * Size.y);
}
