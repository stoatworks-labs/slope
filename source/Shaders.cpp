#include "Shaders.h"

namespace slopefx::shaders
{

const char* const kVertex = R"(#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
)";

//---------------------------------------------------------------------------
// sample: one texel per sample of each scan line.
//
// Line l, position k along the scan (both counted from the picture's top
// left) is host texel ( k, InH - 1 - l ) horizontally and ( l, InH - 1 - k )
// vertically. Sample n is the box average of pixels n Pitch .. n Pitch +
// Pitch - 1, short at the end of the scan. Each coder's signal is taken per
// pixel and then averaged: luma, the three primaries, or Y with Cb and Cr
// -- the last two averaged over the PAIR of samples they will be coded on,
// at half the rate. The sum is accumulated in scan order and divided once;
// on dyadic input with a power-of-two Pitch it is exact whatever the order.
//---------------------------------------------------------------------------
const char* const kSample = R"(#version 410 core

uniform sampler2D InputTexture;
uniform int InW;        //the picture's columns
uniform int InH;        //the picture's rows
uniform int Vertical;   //1: lines are columns, the scan runs down
uniform int Pitch;      //pixels per sample
uniform int Channels;   //0 luma, 1 RGB, 2 Y+C
uniform int Perturb;

out vec4 fragColor;

const vec3 kLuma = vec3( 0.299, 0.587, 0.114 );

int scanLength()
{
	return Vertical == 1 ? InH : InW;
}

ivec2 texelOf( int l, int k )
{
	if( Vertical == 1 )
	{
		//Perturb 32: the vertical scan runs up the column (a negative control).
		int row = ( Perturb & 32 ) != 0 ? k : InH - 1 - k;
		return ivec2( l, row );
	}
	return ivec2( k, InH - 1 - l );
}

vec3 signalOf( vec3 rgb )
{
	if( Channels == 1 )
		return rgb;
	float y = dot( rgb, kLuma );
	if( Channels == 0 )
		return vec3( y, 0.0, 0.0 );
	return vec3( y, 0.5 + ( rgb.b - y ) * 0.564334085778781, 0.5 + ( rgb.r - y ) * 0.713266761768902 );
}

vec3 boxAverage( int l, int first, int count )
{
	vec3 sum = vec3( 0.0 );
	for( int k = 0; k < count; ++k )
		sum += signalOf( texelFetch( InputTexture, texelOf( l, first + k ), 0 ).rgb );
	return sum / float( count );
}

void main()
{
	int n = int( gl_FragCoord.x );
	int l = int( gl_FragCoord.y );

	int len   = scanLength();
	int first = n * Pitch;
	int count = max( 1, min( Pitch, len - first ) );
	vec3 s    = boxAverage( l, first, count );
	if( Channels == 2 )
	{
		int pairFirst = ( n & ~1 ) * Pitch;
		int pairCount = max( 1, min( 2 * Pitch, len - pairFirst ) );
		s.gb          = boxAverage( l, pairFirst, pairCount ).gb;
	}
	fragColor = vec4( s, 1.0 );
}
)";

//---------------------------------------------------------------------------
// coder: the recurrence, in chunks.
//
// Row = line x Coders + coder. The draw for the chunk starting at ChunkStart
// covers samples ChunkStart .. ChunkStart + kChunk - 1 (the viewport is
// offset, so gl_FragCoord.x IS the sample index). A fragment reads the
// state at ChunkStart - 1 from the other buffer pair -- the previous
// draw's output -- or the initial state for the first chunk, and re-runs
// the recurrence up to its own sample. Every fragment of a chunk does the
// same arithmetic in the same order on the same numbers, so the state it
// writes for sample n is the state the next chunk's fragments start from:
// the recurrence is serial in fact and only parallel in execution.
//
// One step, encoder and decoder alike:
//   hist = ( hist << 1 | bit ) & RunMask;  run = hist == 0 || hist == RunMask
//   step = clamp( LeakS step + ( run ? StepAdd : 0 ), StepMin, StepMax )
//   y    = rest + leak ( y - rest ) + ( bit ? step : -step )
// written as rest + leak * ( y - rest ) + d on purpose: with leak = 1 and
// dyadic values every operation is exact, under any contraction or
// reassociation a compiler may apply.
//
// The bit the decoder receives is the encoder's, flipped where an integer
// hash of ( frame, line, sample, coder ) falls under ErrThreshold, or at
// the one position the harness forces. The decoder's output goes through
// the one-pole reconstruction, r = Alpha r + ( 1 - Alpha ) y_d, which at
// Alpha = 0 is exactly y_d.
//
// Y+C chroma coders (coder 1 and 2 with Channels = 2) step on even samples
// only and hold on odd ones: half the sample rate.
//---------------------------------------------------------------------------
const char* const kCoder = R"(#version 410 core

uniform sampler2D Samples;  //samples x lines: each coder's signal
uniform sampler2D PrevE;    //the other pair: the encoder state at every sample so far
uniform sampler2D PrevD;    //the other pair: the decoder state
uniform int ChunkStart;     //first sample of this draw's chunk
uniform int Coders;         //coders per line, 1 or 3
uniform int Channels;       //0 luma, 1 RGB, 2 Y+C
uniform float StepMin;
uniform float StepMax;
uniform float StepAdd;
uniform float LeakI;        //beta_i, 1 for none
uniform float LeakS;        //beta_s, 1 for none
uniform float Alpha;        //reconstruction, 0 for none
uniform int RunMask;        //( 1 << J ) - 1
uniform uint ErrThreshold;  //bit error rate x 2^32
uniform uint FrameSeed;
uniform int ForcedLine;     //a harness hook: one forced bit error, -1 for none
uniform int ForcedSample;
uniform int ForcedCoder;
uniform int Perturb;

layout( location = 0 ) out vec4 outE;
layout( location = 1 ) out vec4 outD;

const float kStartLevel = 0.0;
const float kStartChroma = 0.5;
const int kStartHistory = 5;

uint hashInt( uint v )
{
	uint s = v * 747796405u + 2891336453u;
	uint w = ( ( s >> ( ( s >> 28u ) + 4u ) ) ^ s ) * 277803737u;
	return ( w >> 22u ) ^ w;
}

bool errorAt( int l, int n, int c )
{
	if( ForcedLine >= 0 )
		return l == ForcedLine && n == ForcedSample && c == ForcedCoder;
	uint threshold = ( Perturb & 64 ) != 0 ? ErrThreshold / 2u : ErrThreshold;
	if( threshold == 0u )
		return false;
	uint h = hashInt( hashInt( hashInt( FrameSeed * 3u + uint( c ) ) ^ ( uint( l ) * 2654435769u ) ) ^ uint( n ) );
	return h < threshold;
}

void integrate( inout float y, inout float step, inout int hist, int bit, float leak, float rest, float scale )
{
	hist      = ( ( hist << 1 ) | bit ) & RunMask;
	bool run  = ( hist == 0 || hist == RunMask ) && ( Perturb & 2 ) == 0;
	float add = run ? StepAdd : 0.0;
	step      = clamp( LeakS * step + add, StepMin, StepMax );
	float d   = ( bit == 1 ? step : -step ) * scale;
	y         = rest + leak * ( y - rest ) + d;
}

void main()
{
	int n   = int( gl_FragCoord.x );
	int row = int( gl_FragCoord.y );
	int l   = row / Coders;
	int c   = row - l * Coders;

	float rest     = ( Perturb & 8 ) != 0 ? 0.0 : 0.5;
	float scale    = ( Perturb & 16 ) != 0 ? 1.25 : 1.0;
	float leakDec  = ( Perturb & 4 ) != 0 ? 1.0 : LeakI;
	bool halfRate  = Channels == 2 && c > 0 && ( Perturb & 128 ) == 0;

	//A chroma coder starts at zero colour difference, which is coded 0.5:
	//blanking is black WITH no colour, and a line that started its chroma
	//at 0 would open with a green ramp on every line (found on footage).
	bool chroma    = Channels == 2 && c > 0;
	float start    = chroma ? kStartChroma : kStartLevel;
	float ye = start, se = StepMin, yd = start, sd = StepMin, r = start;
	int he = kStartHistory & RunMask, hd = kStartHistory & RunMask;
	int rx = 0;
	if( ChunkStart > 0 && ( Perturb & 1 ) == 0 )
	{
		vec4 e = texelFetch( PrevE, ivec2( ChunkStart - 1, row ), 0 );
		vec4 d = texelFetch( PrevD, ivec2( ChunkStart - 1, row ), 0 );
		ye = e.x; se = e.y; he = int( e.z ); rx = int( e.w );
		yd = d.x; sd = d.y; hd = int( d.z ); r = d.w;
	}

	for( int i = ChunkStart; i <= n; ++i )
	{
		if( halfRate && ( i & 1 ) == 1 )
			continue;
		float x = texelFetch( Samples, ivec2( i, l ), 0 )[ c ];
		int bit = x >= ye ? 1 : 0;
		integrate( ye, se, he, bit, LeakI, rest, scale );
		int err = errorAt( l, i, c ) ? 1 : 0;
		rx      = bit ^ err;
		integrate( yd, sd, hd, rx, leakDec, rest, scale );
		r = Alpha * r + ( 1.0 - Alpha ) * yd;
	}

	outE = vec4( ye, se, float( he ), float( rx ) );
	outD = vec4( yd, sd, float( hd ), r );
}
)";

//---------------------------------------------------------------------------
// display: the decoded signal back onto the host's framebuffer.
//
// Output pixel ( X, Y ), top-first, shows input pixel ( col, rowT ) under its
// centre; its line and scan position give its sample n, and the chunk's
// parity says which buffer pair holds it. Luma mode adds the coded luma's
// difference from the input's to every primary, so the input's colour
// difference is carried through uncoded; RGB shows the three decoded
// signals; Y+C rebuilds RGB from Y, Cb and Cr. Show Bits draws the received
// bitstream instead, one primary per coder. Mix 1 returns early so that the
// coded picture is never a mix's rounding of itself.
//---------------------------------------------------------------------------
const char* const kDisplay = R"(#version 410 core

uniform sampler2D InputTexture;
uniform sampler2D StateE0;
uniform sampler2D StateD0;
uniform sampler2D StateE1;
uniform sampler2D StateD1;
uniform int InW;
uniform int InH;
uniform int Vertical;
uniform int Pitch;
uniform int Channels;
uniform int Coders;
uniform int Chunk;
uniform int VpX;
uniform int VpY;
uniform int VpW;
uniform int VpH;
uniform int ShowBits;
uniform float MixAmount;

in vec2 uv;
out vec4 fragColor;

const vec3 kLuma = vec3( 0.299, 0.587, 0.114 );

vec4 stateE( int pair, ivec2 at )
{
	return pair == 0 ? texelFetch( StateE0, at, 0 ) : texelFetch( StateE1, at, 0 );
}

vec4 stateD( int pair, ivec2 at )
{
	return pair == 0 ? texelFetch( StateD0, at, 0 ) : texelFetch( StateD1, at, 0 );
}

void main()
{
	int X = int( gl_FragCoord.x ) - VpX;
	int Y = VpH - 1 - ( int( gl_FragCoord.y ) - VpY );

	int col  = clamp( ( ( 2 * X + 1 ) * InW ) / ( 2 * VpW ), 0, InW - 1 );
	int rowT = clamp( ( ( 2 * Y + 1 ) * InH ) / ( 2 * VpH ), 0, InH - 1 );
	vec4 x   = texelFetch( InputTexture, ivec2( col, InH - 1 - rowT ), 0 );

	int l    = Vertical == 1 ? col : rowT;
	int k    = Vertical == 1 ? rowT : col;
	int n    = k / Pitch;
	int pair = ( n / Chunk ) & 1;

	vec3 v    = vec3( 0.0 );
	vec3 bits = vec3( 0.0 );
	for( int c = 0; c < Coders; ++c )
	{
		ivec2 at = ivec2( n, l * Coders + c );
		v[ c ]    = stateD( pair, at ).w;
		bits[ c ] = stateE( pair, at ).w;
	}

	vec3 rgb;
	if( ShowBits == 1 )
		rgb = Channels == 0 ? vec3( bits.r ) : bits;
	else if( Channels == 0 )
		rgb = x.rgb + ( v.r - dot( x.rgb, kLuma ) );
	else if( Channels == 1 )
		rgb = v;
	else
	{
		float yy = v.r;
		float cb = v.g - 0.5;
		float cr = v.b - 0.5;
		float rr = yy + 1.402 * cr;
		float bb = yy + 1.772 * cb;
		float gg = ( yy - 0.299 * rr - 0.114 * bb ) * 1.70357751277683;
		rgb      = vec3( rr, gg, bb );
	}
	vec4 coded = vec4( clamp( rgb, 0.0, 1.0 ), x.a );

	if( MixAmount >= 1.0 )
	{
		fragColor = coded;
		return;
	}
	fragColor = mix( x, coded, MixAmount );
}
)";

} // namespace slopefx::shaders
