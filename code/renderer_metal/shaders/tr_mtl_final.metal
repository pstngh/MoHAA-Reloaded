// tr_mtl_final.metal -- the last pass of every frame: copies the offscreen scene target into the drawable
// and applies the gamma ramp (GL1 loads the same 256-entry table into the display's hardware gamma).

#include <metal_stdlib>
using namespace metal;

struct MbFinalVertexOut
{
	float4 position [[position]];
};

// One triangle covering the whole target: (-1,-1), (3,-1), (-1,3).
vertex MbFinalVertexOut mb_final_vs( uint vid [[vertex_id]] )
{
	float2 uv = float2( ( vid << 1 ) & 2, vid & 2 );
	MbFinalVertexOut out;
	out.position = float4( uv * 2.0f - 1.0f, 0.0f, 1.0f );
	return out;
}

// The scene target and the drawable have the same size, so the copy is a plain texel read.
// ramp[i].rgb = the gamma table entries for input value i (0..255) per channel.
fragment half4 mb_final_fs( MbFinalVertexOut in [[stage_in]],
	texture2d<half, access::read> scene [[texture(0)]],
	constant uchar4 *ramp [[buffer(0)]] )
{
	half3 c = scene.read( uint2( in.position.xy ) ).rgb;
	uint3 i = uint3( c * 255.0h + 0.5h );
	return half4( half( ramp[i.r].r ), half( ramp[i.g].g ), half( ramp[i.b].b ), 255.0h ) / 255.0h;
}
