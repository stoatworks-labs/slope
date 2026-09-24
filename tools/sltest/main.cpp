/**
	sltest -- render Slope offline, and read the coder back out of it.

	How long a step edge takes to climb, what a flat field's idle pattern
	looks like, how the step grows on a run of bits, how a bit error fades:
	each has one right answer in the step size, the leaks and the run
	length. Every check here drives the REAL plugin class through a headless
	GL context and measures the answer out of the picture it made:

		sltest --out /tmp/frame.png     a picture, on the moving test card
		sltest --list                   every parameter, its kind and default
		sltest --overload               a step of height h climbs in ceil( h / step )
		                                samples, at whole- and part-sample edges
		sltest --granular               a flat field's idle pattern: period 2, a
		                                peak-to-peak of one step
		sltest --adapt                  on a run of bits the step grows by the
		                                stated law; the climb shortens by the
		                                predicted count
		sltest --tradeoff               over Adaptation Rate, the climb shortens
		                                and the post-edge hunt's energy rises
		sltest --leak                   a single forced bit error fades on the
		                                integrator's time constant
		sltest --reference              every pixel against a serial double run
		                                of the same recurrence
		sltest --vertical               a vertical scan of the transposed picture
		                                is the horizontal one, transposed, exactly
		sltest --errors                 the received bits are flipped at the
		                                stated rate, differently every frame
		sltest --negative               every check above can FAIL
		sltest --offline                the checks that need no GL
		sltest --bench                  the render cost
		sltest --dump-shaders DIR       the exact GLSL the plugin compiles
		sltest --pipe                   raw frames in, raw frames out

	The control laws, the recurrence, the hash and the colour transforms are
	stated HERE, from their definitions (Controls.h's comments and Model.h's
	description), and never read out of the plugin: a constant typed wrong
	there has to show up as a failed check, not as an agreement. The two
	numbers read from the plugin are kChunk (for the bench's account of the
	draws) and the parameter table (for --list and --set). AGENTS.md has one
	line per check on where each tolerance comes from.
*/

#include "Controls.h"
#include "Model.h"
#include "Shaders.h"
#include "Slope.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{
namespace model = slopefx::model;

int g_checks   = 0;
int g_failures = 0;

//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}
	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );
	ihdr.push_back( 6 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// The control laws, stated from their definitions (Controls.h's comments,
// which are the spec of each control). A check converts the FLOAT it hands
// the plugin, so the stated value is exactly what the plugin was asked for.
//---------------------------------------------------------------------------
double unit( float v )
{
	return std::clamp( static_cast< double >( v ), 0.0, 1.0 );
}
double pow2( double e )
{
	const double whole = std::floor( e );
	return std::ldexp( std::exp2( e - whole ), static_cast< int >( whole ) );
}
double statedStepMin( float v )
{
	return pow2( -8.0 + 4.0 * unit( v ) );
}
double statedStepMax( float v, double stepMin )
{
	return std::max( stepMin, pow2( -6.0 + 6.0 * unit( v ) ) );
}
double statedStepAdd( float v, double stepMax )
{
	return unit( v ) * stepMax / 8.0;
}
int statedRunLength( float v )
{
	return std::clamp( static_cast< int >( std::lround( v ) ), 0, 1 ) == 0 ? 3 : 4;
}
double statedTauI( float v )
{
	return unit( v ) <= 0.0 ? 0.0 : 4.0 * pow2( 10.0 * ( 1.0 - unit( v ) ) );
}
double statedLeakI( float v )
{
	const double tau = statedTauI( v );
	return tau <= 0.0 ? 1.0 : std::exp( -1.0 / tau );
}
double statedLeakS( float v )
{
	const double tau = statedTauI( v ) / 4.0;
	return tau <= 0.0 ? 1.0 : std::exp( -1.0 / tau );
}
int statedPitch( float v )
{
	return std::clamp( static_cast< int >( std::lround( v ) ), 1, 16 );
}
double statedErrorRate( float v )
{
	return unit( v ) <= 0.0 ? 0.0 : std::pow( 10.0, -5.0 + 4.0 * unit( v ) );
}
unsigned int statedErrorThreshold( double rate )
{
	return static_cast< unsigned int >( std::llround( std::clamp( rate, 0.0, 0.5 ) * 4294967296.0 ) );
}
double statedTauR( float v )
{
	return pow2( 6.0 * ( 1.0 - unit( v ) ) ) - 1.0;
}
double statedAlpha( float v )
{
	const double tau = statedTauR( v );
	return tau <= 0.0 ? 0.0 : std::exp( -1.0 / tau );
}
/// The slider for an integrator time constant of tau samples.
float sliderForTauI( double tau )
{
	return static_cast< float >( 1.0 - std::log2( tau / 4.0 ) / 10.0 );
}
float sliderForErrorRate( double p )
{
	return static_cast< float >( ( std::log10( p ) + 5.0 ) / 4.0 );
}

/// Every control a check can move, as the sliders the plugin sees.
struct Knobs
{
	float stepMin  = 0.5f;//1/64
	float stepMax  = 0.5f;//1/8
	float adapt    = 0.5f;
	int runLength  = 0;   //3 bits
	float leak     = 0.4f;//256 samples
	int pitch      = 2;
	int scan       = 0;
	int channels   = 0;
	float errors   = 0.0f;
	float recon    = 0.75f;
	bool showBits  = false;
	float mix      = 1.0f;
};

/// What the shader is handed: every stated law, then rounded to float as the
/// plugin's uniforms are. `exact` says the setting is one in which every
/// operation of the recurrence is exact in float (see exactBecause()).
struct Law
{
	double stepMin = 0, stepMax = 0, stepAdd = 0, leakI = 1, leakS = 1, alpha = 0, errorRate = 0;
	unsigned int errThreshold = 0;
	int J = 3, mask = 7, pitch = 1, channels = 0, coders = 1;
	bool vertical = false;
	bool exact    = false;
};

double asShader( double v )
{
	return static_cast< double >( static_cast< float >( v ) );
}

Law statedFrom( const Knobs& k )
{
	Law L;
	const double stepMin = statedStepMin( k.stepMin );
	const double stepMax = statedStepMax( k.stepMax, stepMin );
	L.stepMin      = asShader( stepMin );
	L.stepMax      = asShader( stepMax );
	L.stepAdd      = asShader( statedStepAdd( k.adapt, stepMax ) );
	L.leakI        = asShader( statedLeakI( k.leak ) );
	L.leakS        = asShader( statedLeakS( k.leak ) );
	L.alpha        = asShader( statedAlpha( k.recon ) );
	L.errorRate    = statedErrorRate( k.errors );
	L.errThreshold = statedErrorThreshold( L.errorRate );
	L.J            = statedRunLength( static_cast< float >( k.runLength ) );
	L.mask         = ( 1 << L.J ) - 1;
	L.pitch        = statedPitch( static_cast< float >( k.pitch ) );
	L.channels     = k.channels;
	L.coders       = k.channels == 0 ? 1 : 3;
	L.vertical     = k.scan == 1;
	return L;
}

bool isPowerOfTwo( int v )
{
	return v > 0 && ( v & ( v - 1 ) ) == 0;
}
bool dyadic12( double v )
{
	const double s = v * 4096.0;
	return s == std::floor( s );
}

/// Why a setting is exact in float, or why not (empty). Exact means: no leak
/// (beta = 1, so rest + 1 ( y - rest ) + d is a sum of dyadics), no filter
/// (alpha = 0, so r = y), RGB (no luma dot, no colour transform), a
/// power-of-two Pitch (the box average divides exactly) and steps on a
/// 2^-12 grid. With inputs on a 1/1024 grid every value the recurrence
/// forms then has at most 14 significant bits, which no order of summation
/// or contraction can round.
std::string exactBecause( const Law& L )
{
	if( L.leakI != 1.0 || L.leakS != 1.0 )
		return "";
	if( L.alpha != 0.0 )
		return "";
	if( L.channels != 1 )
		return "";
	if( !isPowerOfTwo( L.pitch ) )
		return "";
	if( !dyadic12( L.stepMin ) || !dyadic12( L.stepMax ) || !dyadic12( L.stepAdd ) )
		return "";
	return "no leak, no filter, RGB, power-of-two pitch, dyadic steps";
}

//---------------------------------------------------------------------------
// The hash the channel flips bits with, stated: a PCG output mix, exact in
// 32 bits, so the same on both sides. Not physics; the statistics of the
// flips are what --errors measures.
//---------------------------------------------------------------------------
uint32_t hashInt( uint32_t v )
{
	uint32_t state = v * 747796405u + 2891336453u;
	uint32_t word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

bool statedErrorAt( uint32_t seed, int l, int n, int c, unsigned int threshold )
{
	if( threshold == 0u )
		return false;
	const uint32_t h = hashInt( hashInt( hashInt( seed * 3u + static_cast< uint32_t >( c ) ) ^ ( static_cast< uint32_t >( l ) * 2654435769u ) ) ^ static_cast< uint32_t >( n ) );
	return h < threshold;
}

//---------------------------------------------------------------------------
// Pictures, float RGBA, top-first, on a 1/1024 grid so every value is exact.
//---------------------------------------------------------------------------
using Picture = std::vector< float >;

Picture flat( int W, int H, double level )
{
	Picture p( static_cast< size_t >( W ) * H * 4 );
	for( size_t i = 0; i < p.size(); i += 4 )
	{
		p[ i ] = p[ i + 1 ] = p[ i + 2 ] = static_cast< float >( level );
		p[ i + 3 ] = 1.0f;
	}
	return p;
}

/// Level a left of column `edge`, b from it on, grey.
Picture stepEdge( int W, int H, int edge, double a, double b )
{
	Picture p = flat( W, H, a );
	for( int y = 0; y < H; ++y )
		for( int x = edge; x < W; ++x )
		{
			float* px = p.data() + ( static_cast< size_t >( y ) * W + x ) * 4;
			px[ 0 ] = px[ 1 ] = px[ 2 ] = static_cast< float >( b );
		}
	return p;
}

/// Random colour on the 1/1024 grid.
Picture noise( int W, int H, uint32_t seed, double scale = 1.0, double offset = 0.0 )
{
	Picture p( static_cast< size_t >( W ) * H * 4 );
	for( int y = 0; y < H; ++y )
		for( int x = 0; x < W; ++x )
		{
			float* px = p.data() + ( static_cast< size_t >( y ) * W + x ) * 4;
			for( int c = 0; c < 3; ++c )
			{
				const uint32_t h = hashInt( seed * 0x9E3779B9u ^ hashInt( static_cast< uint32_t >( ( y * W + x ) * 3 + c ) ) );
				px[ c ]          = static_cast< float >( offset + scale * ( h % 1024u ) / 1024.0 );
			}
			px[ 3 ] = 1.0f;
		}
	return p;
}

Picture transposed( const Picture& p, int W, int H )
{
	Picture t( p.size() );
	for( int y = 0; y < H; ++y )
		for( int x = 0; x < W; ++x )
			for( int c = 0; c < 4; ++c )
				t[ ( static_cast< size_t >( x ) * H + y ) * 4 + c ] = p[ ( static_cast< size_t >( y ) * W + x ) * 4 + c ];
	return t;
}

float at( const std::vector< float >& img, int W, int r, int c, int ch = 0 )
{
	return img[ ( static_cast< size_t >( r ) * W + c ) * 4 + ch ];
}

/// A measured value on the output's floor or ceiling was clipped by the
/// display and says nothing about the coder: a check that reads one is invalid.
bool clipped( double v )
{
	return v <= 1e-5 || v >= 1.0 - 1e-5;
}

//---------------------------------------------------------------------------
// The serial reference: the recurrence sample by sample, in double, in scan
// order, from the description in Model.h -- and beside it a running bound
// on what float arithmetic in the same order can be off by, per sample,
// which is what the comparison against the GPU is judged with.
//
// The bound (u = 2^-24, M = 2.5 the largest |y - rest| the state can reach
// with a whole-range step): per step,
//   step   clamp( LeakS step + add ): the product's rounding u stepMax, the
//          sum's 1.2 u stepMax, carried through LeakS:  e_s' = LeakS e_s +
//          2.2 u stepMax; 0 when the unclamped value is clearly outside the
//          clamp on either side (the clamp returns an exact constant).
//   y      rest + leak ( y - rest ) + d: four roundings of at most M, the
//          old error through leak, the step's error through d:
//          e_y' = leak e_y + e_s + 4 u M
//   r      alpha r + ( 1 - alpha ) y: e_r' = alpha e_r + ( 1 - alpha ) e_y + 4 u M
//   x      the box average: ( pitch + 1 ) u, plus 8 u for the luma dot or
//          the colour transform when there is one
// A comparator decision is undetermined when | x - y | <= e_x + e_y; from
// that sample on, THAT line's coder is excluded from the comparison (a
// different decision is a different orbit), and the exclusion is counted.
// In an exact setting every bound is 0 and nothing is excluded.
//---------------------------------------------------------------------------
constexpr double kU = 1.0 / 16777216.0;
constexpr double kM = 2.5;

struct CoderState
{
	double y    = model::kStartLevel;
	double step = 0.0;
	int hist    = 0;
	double eY   = 0.0;//float error bound on y
	double eS   = 0.0;//float error bound on step
};

struct LineRun
{
	std::vector< double > x, ye, yd, r;
	std::vector< int > tx, rx, run;
	std::vector< double > eX, eYe, eYd, eR, stepE, stepD;
	int tie = -1;///< first sample whose comparator decision is undetermined; -1 none
};

struct Reference
{
	Law L;
	int W = 0, H = 0;
	uint32_t seed    = 0;
	int forcedLine   = -1, forcedSample = -1, forcedCoder = -1;
	std::vector< LineRun > lines;///< [ l * coders + c ]

	int lineCount() const
	{
		return L.vertical ? W : H;
	}
	int scanLength() const
	{
		return L.vertical ? H : W;
	}
	int sampleCount() const
	{
		return ( scanLength() + L.pitch - 1 ) / L.pitch;
	}
	const LineRun& line( int l, int c ) const
	{
		return lines[ static_cast< size_t >( l ) * L.coders + c ];
	}

	/// Pixel ( line l, position k along the scan ), top-first picture coords.
	const float* pixel( const Picture& pic, int l, int k ) const
	{
		const int row = L.vertical ? k : l;
		const int col = L.vertical ? l : k;
		return pic.data() + ( static_cast< size_t >( row ) * W + col ) * 4;
	}

	/// Each coder's signal of one pixel: luma; the primaries; Y, Cb, Cr.
	void signalOf( const float* px, double out[ 3 ] ) const
	{
		if( L.channels == 1 )
		{
			out[ 0 ] = px[ 0 ];
			out[ 1 ] = px[ 1 ];
			out[ 2 ] = px[ 2 ];
			return;
		}
		const double y = model::kLumaR * px[ 0 ] + model::kLumaG * px[ 1 ] + model::kLumaB * px[ 2 ];
		out[ 0 ] = y;
		out[ 1 ] = L.channels == 0 ? 0.0 : 0.5 + ( px[ 2 ] - y ) / model::kCbScale;
		out[ 2 ] = L.channels == 0 ? 0.0 : 0.5 + ( px[ 0 ] - y ) / model::kCrScale;
	}

	double sampleOf( const Picture& pic, int l, int c, int n ) const
	{
		const int len = scanLength();
		int first = n * L.pitch, count = std::min( L.pitch, len - first );
		if( L.channels == 2 && c > 0 )
		{
			first = ( n & ~1 ) * L.pitch;
			count = std::min( 2 * L.pitch, len - first );
		}
		count = std::max( 1, count );
		double sum = 0.0;
		for( int k = 0; k < count; ++k )
		{
			double s[ 3 ];
			signalOf( pixel( pic, l, first + k ), s );
			sum += s[ c ];
		}
		return sum / count;
	}

	double sampleBound() const
	{
		if( L.exact )
			return 0.0;
		return kU * ( L.pitch + 1 ) + ( L.channels == 1 ? 0.0 : 8.0 * kU );
	}

	void integrate( CoderState& s, int bit, double leak ) const
	{
		s.hist         = ( ( s.hist << 1 ) | bit ) & L.mask;
		const bool run = s.hist == 0 || s.hist == L.mask;
		const double unclamped = L.leakS * s.step + ( run ? L.stepAdd : 0.0 );
		s.step                 = std::clamp( unclamped, L.stepMin, L.stepMax );
		double eS = L.leakS * s.eS + 2.2 * kU * L.stepMax;
		if( unclamped < L.stepMin - eS || unclamped > L.stepMax + eS )
			eS = 0.0;
		s.eS = L.exact ? 0.0 : eS;
		const double d = bit ? s.step : -s.step;
		s.y            = model::kRestLevel + leak * ( s.y - model::kRestLevel ) + d;
		s.eY           = L.exact ? 0.0 : leak * s.eY + s.eS + 4.0 * kU * kM;
	}

	void runLine( const Picture& pic, int l, int c, LineRun& out ) const
	{
		const int N = sampleCount();
		out.x.assign( N, 0.0 );
		out.ye.assign( N, 0.0 );
		out.yd.assign( N, 0.0 );
		out.r.assign( N, 0.0 );
		out.tx.assign( N, 0 );
		out.rx.assign( N, 0 );
		out.run.assign( N, 0 );
		out.eX.assign( N, 0.0 );
		out.eYe.assign( N, 0.0 );
		out.eYd.assign( N, 0.0 );
		out.eR.assign( N, 0.0 );
		out.stepE.assign( N, 0.0 );
		out.stepD.assign( N, 0.0 );
		out.tie = -1;

		CoderState enc, dec;
		enc.step = dec.step = L.stepMin;
		enc.hist = dec.hist = model::kStartHistory & L.mask;
		double r = model::kStartLevel, eR = 0.0;
		const bool halfRate = L.channels == 2 && c > 0;
		const double eX     = sampleBound();
		int rx = 0;
		for( int n = 0; n < N; ++n )
		{
			if( halfRate && ( n & 1 ) )
			{
				//Held.
				out.x[ n ]  = out.x[ n - 1 ];
				out.tx[ n ] = out.tx[ n - 1 ];
				out.rx[ n ] = rx;
				out.run[ n ] = out.run[ n - 1 ];
			}
			else
			{
				const double x = sampleOf( pic, l, c, n );
				out.x[ n ]     = x;
				//An exact tie in an exact setting is decided the same way on both
				//sides ( >= ); only a tie inside a nonzero bound is undetermined.
				if( !L.exact && out.tie < 0 && std::fabs( x - enc.y ) <= eX + enc.eY )
					out.tie = n;
				const int bit = x >= enc.y ? 1 : 0;
				integrate( enc, bit, L.leakI );
				const bool forced = forcedLine >= 0;
				const int err     = forced ? ( l == forcedLine && n == forcedSample && c == forcedCoder ) : statedErrorAt( seed, l, n, c, L.errThreshold );
				rx                = bit ^ err;
				integrate( dec, rx, L.leakI );
				r  = L.alpha * r + ( 1.0 - L.alpha ) * dec.y;
				eR = L.exact ? 0.0 : L.alpha * eR + ( 1.0 - L.alpha ) * dec.eY + 4.0 * kU * kM;
				out.tx[ n ]  = bit;
				out.rx[ n ]  = rx;
				out.run[ n ] = ( enc.hist == 0 || enc.hist == L.mask ) ? 1 : 0;
			}
			out.ye[ n ]    = enc.y;
			out.yd[ n ]    = dec.y;
			out.r[ n ]     = r;
			out.eX[ n ]    = eX;
			out.eYe[ n ]   = enc.eY;
			out.eYd[ n ]   = dec.eY;
			out.eR[ n ]    = eR;
			out.stepE[ n ] = enc.step;
			out.stepD[ n ] = dec.step;
		}
	}

	void run( const Picture& pic, uint32_t frameSeed )
	{
		seed = frameSeed;
		lines.assign( static_cast< size_t >( lineCount() ) * L.coders, LineRun() );
		for( int l = 0; l < lineCount(); ++l )
			for( int c = 0; c < L.coders; ++c )
				runLine( pic, l, c, lines[ static_cast< size_t >( l ) * L.coders + c ] );
	}

	/// Output pixel ( row, col ), top-first: the decoded signals back to RGB
	/// as the display does, with a bound per channel and whether any coder
	/// it reads is past a tie. Mix 1, Show Bits off.
	bool expected( const Picture& pic, int row, int col, double rgb[ 3 ], double bound[ 3 ] ) const
	{
		const int l = L.vertical ? col : row;
		const int k = L.vertical ? row : col;
		const int n = k / L.pitch;
		const float* x = pic.data() + ( static_cast< size_t >( row ) * W + col ) * 4;
		double v[ 3 ] = { 0, 0, 0 }, e[ 3 ] = { 0, 0, 0 };
		bool trusted = true;
		for( int c = 0; c < L.coders; ++c )
		{
			const LineRun& ln = line( l, c );
			v[ c ]            = ln.r[ n ];
			e[ c ]            = ln.eR[ n ];
			if( ln.tie >= 0 && n >= ln.tie )
				trusted = false;
		}
		if( L.channels == 0 )
		{
			const double luma = model::kLumaR * x[ 0 ] + model::kLumaG * x[ 1 ] + model::kLumaB * x[ 2 ];
			for( int ch = 0; ch < 3; ++ch )
			{
				rgb[ ch ]   = std::clamp( x[ ch ] + ( v[ 0 ] - luma ), 0.0, 1.0 );
				bound[ ch ] = e[ 0 ] + 7.0 * kU;
			}
		}
		else if( L.channels == 1 )
		{
			for( int ch = 0; ch < 3; ++ch )
			{
				rgb[ ch ]   = std::clamp( v[ ch ], 0.0, 1.0 );
				bound[ ch ] = e[ ch ];
			}
		}
		else
		{
			const double yy = v[ 0 ], cb = v[ 1 ] - 0.5, cr = v[ 2 ] - 0.5;
			const double rr = yy + model::kCrScale * cr;
			const double bb = yy + model::kCbScale * cb;
			const double gg = ( yy - model::kLumaR * rr - model::kLumaB * bb ) / model::kLumaG;
			rgb[ 0 ]        = std::clamp( rr, 0.0, 1.0 );
			rgb[ 1 ]        = std::clamp( gg, 0.0, 1.0 );
			rgb[ 2 ]        = std::clamp( bb, 0.0, 1.0 );
			const double eRr = e[ 0 ] + model::kCrScale * e[ 2 ] + 3.0 * kU;
			const double eBb = e[ 0 ] + model::kCbScale * e[ 1 ] + 3.0 * kU;
			bound[ 0 ]       = eRr;
			bound[ 2 ]       = eBb;
			bound[ 1 ]       = ( e[ 0 ] + model::kLumaR * eRr + model::kLumaB * eBb + 5.0 * kU ) / model::kLumaG + kU;
		}
		return trusted;
	}
};

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, GLint internalFormat, GLenum type, const void* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, internalFormat, width, height, 0, GL_RGBA, type, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
}

template< typename T >
std::vector< T > flipRows( const std::vector< T >& image, int width, int height )
{
	std::vector< T > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::copy( image.begin() + static_cast< long >( ( height - 1 - y ) * stride ),
		           image.begin() + static_cast< long >( ( height - y ) * stride ),
		           flipped.begin() + static_cast< long >( y * stride ) );
	return flipped;
}

//---------------------------------------------------------------------------
// Parameters by display name.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	unsigned int type;
	float value;
	float low;
	float high;
};

const char* kindName( const NamedParameter& p )
{
	if( p.index >= Slope::PT_ABOUT_FIRST )
		return "about";
	switch( p.type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_INTEGER: return "integer";
	case FF_TYPE_BUFFER: return "buffer";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_STANDARD: return "standard";
	default: return "other";
	}
}

std::vector< NamedParameter > listParameters( Slope& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < Slope::PT_COUNT; ++i )
	{
		const char* const name = plugin.GetParamName( i );
		NamedParameter p;
		p.name  = name ? name : "?";
		p.index = i;
		p.type  = plugin.GetParamType( i );
		p.value = plugin.GetFloatParameter( i );
		p.low   = 0.0f;
		p.high  = 1.0f;
		//An option's range reads back 0..1 whatever its element count, so
		//the element count is the range; an integer's range is real.
		if( p.type == FF_TYPE_OPTION )
			p.high = static_cast< float >( std::max( 1u, plugin.GetNumParamElements( i ) ) - 1u );
		else if( p.type == FF_TYPE_INTEGER )
		{
			const RangeStruct range = plugin.GetParamRange( i );
			p.low                   = range.min;
			p.high                  = range.max;
		}
		list.push_back( p );
	}
	return list;
}

int indexOfParameter( Slope& plugin, const std::string& name )
{
	for( const NamedParameter& p : listParameters( plugin ) )
		if( p.name == name )
			return static_cast< int >( p.index );
	return -1;
}

bool applySetting( Slope& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.rfind( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const std::string name = assignment.substr( 0, equals );
	const int index        = indexOfParameter( plugin, name );
	if( index < 0 )
	{
		error = "no parameter called '" + name + "'";
		return false;
	}
	plugin.SetFloatParameter( static_cast< unsigned int >( index ), std::strtof( assignment.substr( equals + 1 ).c_str(), nullptr ) );
	return true;
}

bool set( Slope& plugin, const char* name, float value )
{
	std::string error;
	char buffer[ 64 ];
	std::snprintf( buffer, sizeof( buffer ), "%.9g", value );
	if( applySetting( plugin, std::string( name ) + "=" + buffer, error ) )
		return true;
	std::fprintf( stderr, "%s\n", error.c_str() );
	return false;
}

void apply( Slope& p, const Knobs& k )
{
	set( p, "Step Size", k.stepMin );
	set( p, "Max Step", k.stepMax );
	set( p, "Adaptation Rate", k.adapt );
	set( p, "Run Length", static_cast< float >( k.runLength ) );
	set( p, "Leak", k.leak );
	set( p, "Pixels/Sample", static_cast< float >( k.pitch ) );
	set( p, "Scan", static_cast< float >( k.scan ) );
	set( p, "Channels", static_cast< float >( k.channels ) );
	set( p, "Bit Errors", k.errors );
	set( p, "Reconstruction", k.recon );
	set( p, "Show Bits", k.showBits ? 1.0f : 0.0f );
	set( p, "Mix", k.mix );
}

//---------------------------------------------------------------------------
// A session: the plugin, its input and output. No clock: the plugin has
// none, and the bit errors' seed is the plugin's own frame counter.
//---------------------------------------------------------------------------
struct Session
{
	Slope plugin;
	int width        = 0;
	int height       = 0;
	bool floatOutput = true;

	GLuint sourceTexture = 0;
	GLuint outputTexture = 0;
	GLuint outputFBO     = 0;
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };
	ProcessOpenGLStruct process    = {};

	void makeTargets()
	{
		sourceTexture = makeTexture( width, height, GL_RGBA32F, GL_FLOAT, nullptr );
		outputTexture = floatOutput ? makeTexture( width, height, GL_RGBA32F, GL_FLOAT, nullptr )
		                            : makeTexture( width, height, GL_RGBA8, GL_UNSIGNED_BYTE, nullptr );
		outputFBO     = makeFramebuffer( outputTexture );

		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;

		process.numInputTextures = 1;
		process.inputTextures    = inputs;
		process.HostFBO          = outputFBO;
	}

	void dropTargets()
	{
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}

	bool begin( int w, int h )
	{
		width  = w;
		height = h;
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( width );
		viewport.height             = static_cast< FFUInt32 >( height );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
			return false;
		}
		makeTargets();
		return true;
	}

	/// What a host does when the clip changes size: the SAME instance handed
	/// a differently sized input. No DeInitGL.
	void resize( int w, int h )
	{
		dropTargets();
		width  = w;
		height = h;
		makeTargets();
	}

	/// The seed the next render will use.
	uint32_t nextSeed() const
	{
		return plugin.FrameIndexForTest();
	}

	bool renderNow()
	{
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		const bool ok = plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
		if( !ok )
			std::fprintf( stderr, "ProcessOpenGL failed on frame %u\n", plugin.FrameIndexForTest() );
		return ok;
	}

	bool render( const std::vector< unsigned char >& pixels )
	{
		const std::vector< unsigned char > flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
		return renderNow();
	}

	bool render( const Picture& pixels )
	{
		const std::vector< float > flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
		return renderNow();
	}

	std::vector< unsigned char > readBack()
	{
		std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
		return flipRows( pixels, width, height );
	}

	std::vector< float > readBackFloat()
	{
		std::vector< float > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return flipRows( pixels, width, height );
	}

	void end()
	{
		plugin.DeInitGL();
		dropTargets();
	}
};

void prepare( Session& s, const Knobs& k, int perturb )
{
	apply( s.plugin, k );
	s.plugin.SetPerturbForTest( perturb );
}

/// One frame of one picture through a fresh session, read back as floats.
bool renderOnce( const Knobs& k, int perturb, int W, int H, const Picture& pic, std::vector< float >& out, int forcedLine = -1, int forcedSample = -1, int forcedCoder = -1 )
{
	Session s;
	prepare( s, k, perturb );
	s.plugin.SetForcedErrorForTest( forcedLine, forcedSample, forcedCoder );
	if( !s.begin( W, H ) || !s.render( pic ) )
		return false;
	out = s.readBackFloat();
	s.end();
	return true;
}

const char* verdict( bool ok )
{
	return ok ? "ok" : "FAIL";
}

int report( bool ok, bool quiet, const char* format, ... ) __attribute__( ( format( printf, 3, 4 ) ) );
int report( bool ok, bool quiet, const char* format, ... )
{
	++g_checks;
	if( !ok )
		++g_failures;
	//Quiet is a negative control's run: its failures are the point, and the
	//summary line says so. --perturb runs the same thing verbosely.
	if( !quiet )
	{
		va_list args;
		va_start( args, format );
		std::vprintf( format, args );
		va_end( args );
		std::printf( "  %s\n", verdict( ok ) );
	}
	return ok ? 0 : 1;
}

/// The value the picture shows for sample n of line l (grey pictures,
/// horizontal scan, channel 0), at the first pixel of the sample.
double sampleValue( const std::vector< float >& out, int W, int row, int n, int pitch = 1, int ch = 0 )
{
	return at( out, W, row, n * pitch, ch );
}

/// Consecutive received 1-bits from the first 1 at or after sample `from`.
int runFrom( const std::vector< float >& bits, int W, int row, int from, int N, int pitch, int& start )
{
	start = -1;
	for( int n = from; n < N; ++n )
		if( sampleValue( bits, W, row, n, pitch ) >= 0.5 )
		{
			start = n;
			break;
		}
	if( start < 0 )
		return 0;
	int m = 0;
	for( int n = start; n < N && sampleValue( bits, W, row, n, pitch ) >= 0.5; ++n )
		++m;
	return m;
}

//---------------------------------------------------------------------------
// The idle pattern of a flat level a, with no leak, in closed form: the
// coder climbs from 0 in steps of D, y_n = n D, until y_n > a. The first n
// at which that happens is n_lock = floor( a / D ) + 1, with y = hi =
// floor( a / D ) D + D; from then on y alternates hi ( n - n_lock even )
// and lo = hi - D ( odd ). A tie ( x == y ) is + by the comparator's rule,
// which is why an a on the step grid still gives hi = a + D.
//---------------------------------------------------------------------------
struct Idle
{
	int lock;
	double hi, lo;
	double at( int n ) const
	{
		return ( ( n - lock ) & 1 ) == 0 ? hi : lo;
	}
};

Idle idleOf( double a, double D )
{
	Idle i;
	const double k = std::floor( a / D );
	i.lock        = static_cast< int >( k ) + 1;
	i.hi          = k * D + D;
	i.lo          = i.hi - D;
	return i;
}

//---------------------------------------------------------------------------
// --overload
//
// No leak, no adaptation, no filter: a step of height h at column e. The
// coder is hunting round a in the idle pattern; at the edge sample its guess
// y_e is hi or lo by parity, and the climb runs m = ceil( ( b - y_s ) / D )
// samples, y_s the guess before the first +, which is y_e when the edge
// sample's own box average x_e >= y_e and y_e - D otherwise (the edge sample
// read as a - bit first). With h chosen off the step grid, that is
// ceil( h / D ) when the guess was at a and ceil( h / D ) - 1 when it had a
// step's head start -- the spec's statement, with the phase made explicit.
// Whole-sample edges ( e a multiple of Pitch ) and fractional ones ( x_e
// between a and b, an exact box average of dyadic levels ) are separate
// cases; the fractional prediction depends on x_e against y_e, and the
// check asserts that comparison has a margin of at least D / 8 so no
// rounding can turn it. The run is counted from the Show Bits picture, and
// the staircase is read from the plain one: y_s + ( i + 1 ) D at the i-th
// sample of the run, exactly.
//---------------------------------------------------------------------------
int runOverload( int W, int H, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	Knobs k;
	k.channels = 1;
	k.leak     = 0.0f;
	k.adapt    = 0.0f;
	k.recon    = 1.0f;
	k.stepMin  = 0.5f;
	for( int pitch : { 1, 4 } )
	{
		k.pitch = pitch;
		const Law L = statedFrom( k );
		if( exactBecause( L ).empty() )
			return report( false, quiet, "overload: the setting is not exact" );
		const double D = L.stepMin;
		const double a = 0.5;
		const int N    = ( W + pitch - 1 ) / pitch;
		const Idle idle = idleOf( a, D );
		const int rows[ 3 ] = { 0, H / 2, H - 1 };

		for( double steps : { 8.5, 13.5, 27.5 } )
			for( int f = 0; f < pitch; ++f )
			{
				const double h  = steps * D;
				const double b  = a + h;
				const int nE    = N * 3 / 5;
				const int edge  = nE * pitch + f;
				if( edge >= W || nE + static_cast< int >( steps ) + 3 >= N )
					continue;
				const Picture pic = stepEdge( W, H, edge, a, b );

				//Prediction.
				const double yE = idle.at( nE );
				const double xE = ( f * a + ( pitch - f ) * b ) / pitch;
				const bool up   = xE >= yE;
				const double yS = up ? yE : yE - D;
				const int m     = static_cast< int >( std::ceil( ( b - yS ) / D ) );
				const int start = up ? nE : nE + 1;
				const double margin = std::fabs( xE - yE );
				const bool phaseAtA = yE == idle.lo;

				k.showBits = true;
				std::vector< float > bits, plain;
				if( !renderOnce( k, perturb, W, H, pic, bits ) )
					return report( false, quiet, "overload: could not render" );
				k.showBits = false;
				if( !renderOnce( k, perturb, W, H, pic, plain ) )
					return report( false, quiet, "overload: could not render" );

				bool ok = margin >= D / 8.0;
				int measured = -1, measuredStart = -1;
				double worstStair = 0.0;
				for( int row : rows )
				{
					int st = -1;
					const int mm = runFrom( bits, W, row, nE, N, pitch, st );
					if( measured < 0 )
					{
						measured      = mm;
						measuredStart = st;
					}
					ok = ok && mm == m && st == start;
					for( int i = 0; i < mm && st + i < N; ++i )
						worstStair = std::max( worstStair, std::fabs( sampleValue( plain, W, row, st + i, pitch ) - ( yS + ( i + 1 ) * D ) ) );
				}
				ok = ok && worstStair == 0.0;
				failures += report( ok, quiet,
				                    "overload pitch %d h = %4.1f steps, edge %s (f = %d), guess at %s: climb of %2d samples predicted from sample %d, measured %2d from %d; staircase off by %.3g",
				                    pitch, steps, f == 0 ? "whole" : "part ", f, phaseAtA ? "a  " : "a+D", m, start, measured, measuredStart, worstStair );
			}
	}
	return failures;
}

//---------------------------------------------------------------------------
// --granular
//
// (a) No leak: on a flat field at 0.25, 0.5 and 0.75 the picture from
//     n_lock - 1 on IS the idle pattern of idleOf(), sample for sample and
//     exactly: period 2, peak-to-peak one step.
// (b) A leak of 64 samples, the field at the rest level (0.5), so the leak
//     has nothing to pull against: with e = y - rest, e <- beta e + b D, and
//     the 2-cycle is e = +-D / ( 1 + beta ) -- a peak-to-peak of
//     2 D / ( 1 + beta ), which is D as beta -> 1. From the sample the coder
//     first crosses the level, the orbit contracts onto that cycle by beta a
//     sample, so sample n is within D beta^( n - lock ) of it, plus the float
//     bound. Measured on every row, over the last half of the line.
//---------------------------------------------------------------------------
int runGranular( int W, int H, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	Knobs k;
	k.channels = 1;
	k.adapt    = 0.0f;
	k.recon    = 1.0f;
	k.stepMin  = 0.5f;
	k.pitch    = 1;
	const int N = W;

	//(a)
	k.leak = 0.0f;
	{
		const Law L = statedFrom( k );
		if( exactBecause( L ).empty() )
			return report( false, quiet, "granular: the setting is not exact" );
		const double D = L.stepMin;
		for( double a : { 0.25, 0.5, 0.75 } )
		{
			const Idle idle = idleOf( a, D );
			std::vector< float > out;
			if( !renderOnce( k, perturb, W, H, flat( W, H, a ), out ) )
				return report( false, quiet, "granular: could not render" );
			double worst = 0.0;
			int compared = 0;
			for( int row = 0; row < H; row += std::max( 1, H / 8 ) )
				for( int n = idle.lock - 1; n < N; ++n )
				{
					//The picture shows the decoder's guess AFTER sample n's bit: y_{n+1}.
					worst = std::max( worst, std::fabs( sampleValue( out, W, row, n, 1 ) - idle.at( n + 1 ) ) );
					++compared;
				}
			failures += report( worst == 0.0, quiet,
			                    "granular no leak, level %.2f: from sample %d the idle pattern alternates %.6f / %.6f (period 2, peak-to-peak %.6f = one step), %d samples exact, worst %.3g",
			                    a, idle.lock - 1, idle.hi, idle.lo, D, compared, worst );
		}
	}

	//(b)
	k.leak = sliderForTauI( 64.0 );
	{
		const Law L      = statedFrom( k );
		const double D   = L.stepMin;
		const double beta = L.leakI;
		const double pp  = 2.0 * D / ( 1.0 + beta );
		//The stated run-up from 0 to 0.5: e_n = beta^n e_0 + D ( 1 - beta^n ) / ( 1 - beta ); lock at the first n with e_n > 0.
		int lock = -1;
		double e = model::kStartLevel - model::kRestLevel;
		for( int n = 0; n < N; ++n )
		{
			e = beta * e + D;
			if( e > 0.0 )
			{
				lock = n + 1;
				break;
			}
		}
		if( lock < 0 || D / ( 1.0 - beta ) <= 0.5 )
			return report( false, quiet, "granular: the coder cannot reach the level with this leak" );

		Reference ref;
		ref.L = L;
		ref.W = W;
		ref.H = H;
		const Picture pic = flat( W, H, 0.5 );
		ref.run( pic, 0 );
		std::vector< float > out;
		if( !renderOnce( k, perturb, W, H, pic, out ) )
			return report( false, quiet, "granular: could not render" );

		double worstPeriod = 0.0, worstPP = 0.0, meanPP = 0.0;
		int compared = 0;
		bool ok = true;
		for( int row = 0; row < H; row += std::max( 1, H / 8 ) )
			for( int n = std::max( lock, N / 2 ); n + 2 < N; ++n )
			{
				const double y0 = sampleValue( out, W, row, n, 1 ), y1 = sampleValue( out, W, row, n + 1, 1 ), y2 = sampleValue( out, W, row, n + 2, 1 );
				const double eY = ref.line( 0, 0 ).eYe[ n + 2 ];
				const double tol = D * std::pow( beta, n - lock ) + 4.0 * eY;
				const double period = std::fabs( y2 - y0 ), swing = std::fabs( y1 - y0 );
				worstPeriod = std::max( worstPeriod, period / tol );
				worstPP     = std::max( worstPP, std::fabs( swing - pp ) / tol );
				meanPP += swing;
				++compared;
				ok = ok && period <= tol && std::fabs( swing - pp ) <= tol;
			}
		meanPP /= std::max( 1, compared );
		failures += report( ok, quiet,
		                    "granular leak %g samples, level 0.50: period 2 within %.3f of tolerance, peak-to-peak %.6f (stated 2D/(1+beta) = %.6f, D = %.6f) within %.3f, over %d samples",
		                    64.0, worstPeriod, meanPP, pp, D, worstPP, compared );
	}
	return failures;
}

//---------------------------------------------------------------------------
// The stated climb: from a guess y with step s and history hist, bits all +
// until y > b, the step growing by the law each sample. Returns the count.
//---------------------------------------------------------------------------
int statedClimb( const Law& L, double y, double s, int hist, double b, int limit )
{
	int m = 0;
	while( y <= b && m < limit )
	{
		hist           = ( ( hist << 1 ) | 1 ) & L.mask;
		const bool run = hist == L.mask;
		s              = std::clamp( L.leakS * s + ( run ? L.stepAdd : 0.0 ), L.stepMin, L.stepMax );
		y              = model::kRestLevel + L.leakI * ( y - model::kRestLevel ) + s;
		++m;
	}
	return m;
}

//---------------------------------------------------------------------------
// --adapt
//
// (1) The law per run, with the leaks on. A picture at 0.5 that ramps by
//     0.06 a sample to 0.98: the coder falls behind, every bit is +, and
//     from the third the run detector fires each sample. The picture gives
//     the decoder's y at every sample (no filter), so each sample's step is
//     recovered as ( y_n - rest ) - beta_i ( y_{n-1} - rest ), and the law
//     says step_n = beta_s step_{n-1} + step_add while it runs (clamped at
//     Max Step), and beta_s step_{n-1} before the run is detected. Each
//     increment is checked against the law applied to the MEASURED previous
//     step, within the float bounds of the two samples.
// (2) The climb shortens by the predicted count, in exact arithmetic (no
//     leak): a step edge from 0.5 to 0.9 with adaptation off and on. With no
//     syllabic leak the run-up from 0 leaves the step where it grew to and
//     the idle pattern is that step wide, so the state at the edge comes
//     from the stated run-up (statedClimb over the same law), and the two
//     climbs from it are predicted with tolerance 0.
//---------------------------------------------------------------------------
int runAdapt( int W, int H, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	//(1)
	{
		Knobs k;
		k.channels = 1;
		k.pitch    = 1;
		k.recon    = 1.0f;
		k.leak     = sliderForTauI( 64.0 );
		k.adapt    = 0.5f;
		const Law L  = statedFrom( k );
		const int N  = W;
		const int e  = N / 3;
		Picture pic  = flat( W, H, 0.5 );
		for( int y = 0; y < H; ++y )
			for( int x = e; x < W; ++x )
			{
				//The top leaves room for the overshoot of an adapted step: a guess
				//past 1.0 is clipped by the display and reads as a smaller step.
				const double v = std::min( 0.88, 0.5 + 0.06 * ( x - e ) );
				float* px      = pic.data() + ( static_cast< size_t >( y ) * W + x ) * 4;
				px[ 0 ] = px[ 1 ] = px[ 2 ] = static_cast< float >( std::round( v * 1024.0 ) / 1024.0 );
			}
		Reference ref;
		ref.L = L;
		ref.W = W;
		ref.H = H;
		ref.run( pic, 0 );

		k.showBits = true;
		std::vector< float > bits, plain;
		if( !renderOnce( k, perturb, W, H, pic, bits ) )
			return report( false, quiet, "adapt: could not render" );
		k.showBits = false;
		if( !renderOnce( k, perturb, W, H, pic, plain ) )
			return report( false, quiet, "adapt: could not render" );

		const int row = H / 2;
		int st = -1;
		const int m = runFrom( bits, W, row, e, N, 1, st );
		//Measured steps along the run, from the picture alone.
		int growth = 0, checked = 0;
		double worst = 0.0, first = 0.0, last = 0.0;
		bool clampedSeen = false;
		std::string seq;
		char worstNote[ 160 ] = "";
		bool clip = false;
		auto stepAt = [ & ]( int n ) {
			const double yn = sampleValue( plain, W, row, n ), yp = sampleValue( plain, W, row, n - 1 );
			clip            = clip || clipped( yn ) || clipped( yp );
			return ( yn - model::kRestLevel ) - L.leakI * ( yp - model::kRestLevel );
		};
		for( int i = 1; i < m; ++i )
		{
			const int n           = st + i;
			const double prev     = stepAt( n - 1 );
			const double now      = stepAt( n );
			//Whether the run detector fires at n is read from the bits picture:
			//the hunt's last bit before the ramp may itself be a +, so the run
			//can fire a sample before the counted run's J-th bit.
			bool running = true;
			for( int j = 0; j < L.J; ++j )
				running = running && n - j >= 0 && sampleValue( bits, W, row, n - j ) >= 0.5;
			const double lawNow   = std::clamp( L.leakS * prev + ( running ? L.stepAdd : 0.0 ), L.stepMin, L.stepMax );
			const LineRun& ln     = ref.line( row, 0 );
			const double tol      = 2.0 * ( ln.eYe[ n ] + ln.eYe[ n - 1 ] + ln.eYe[ n - 2 ] ) + 2.0 * L.leakS * ( ln.eYe[ n - 1 ] + ln.eYe[ n - 2 ] );
			if( std::fabs( now - lawNow ) / tol > worst )
			{
				worst = std::fabs( now - lawNow ) / tol;
				std::snprintf( worstNote, sizeof( worstNote ), " (worst at run sample %d: measured %.6f, law %.6f, tolerance %.2g%s)", i, now, lawNow, tol, running ? ", running" : "" );
			}
			if( running && now > prev + tol )
				++growth;
			if( now >= L.stepMax - tol )
				clampedSeen = true;
			++checked;
			if( i == 1 )
				first = prev;
			last = now;
			if( i < 12 )
			{
				char buf[ 32 ];
				std::snprintf( buf, sizeof( buf ), "%s%.4f", i == 1 ? "" : " ", now );
				seq += buf;
			}
		}
		failures += report( !clip && checked >= 8 && growth >= 6 && worst <= 1.0, quiet,
		                    "adapt law: a run of %d + bits; %d steps recovered from the picture, %d of them grown, each beta_s x previous + %.5f (run) within %.3f of tolerance%s; %.4f -> %.4f%s [%s ...]%s",
		                    m, checked, growth, L.stepAdd, worst, worstNote, first, last, clampedSeen ? " (reached Max Step)" : "", seq.c_str(), clip ? " -- CLIPPED" : "" );
	}

	//(2)
	{
		Knobs k;
		k.channels = 1;
		k.pitch    = 1;
		k.recon    = 1.0f;
		k.leak     = 0.0f;
		const int N = W;
		const int nE = N * 3 / 5;
		const double a = 0.5, b = 0.8;
		const Picture pic = stepEdge( W, H, nE, a, b );
		int measured[ 2 ] = { 0, 0 }, predicted[ 2 ] = { 0, 0 };
		bool ok = true;
		for( int adaptive = 0; adaptive < 2; ++adaptive )
		{
			k.adapt = adaptive ? 0.5f : 0.0f;
			const Law L = statedFrom( k );
			if( exactBecause( L ).empty() )
				return report( false, quiet, "adapt: the setting is not exact" );
			//The stated run-up from 0 to a, then the idle pattern at whatever
			//step it left, then the climb from the phase at nE.
			double y = model::kStartLevel, s = L.stepMin;
			int hist = model::kStartHistory & L.mask, n = 0;
			while( y <= a )
			{
				hist = ( ( hist << 1 ) | 1 ) & L.mask;
				s    = std::clamp( L.leakS * s + ( hist == L.mask ? L.stepAdd : 0.0 ), L.stepMin, L.stepMax );
				y += s;
				++n;
			}
			//n is now lock: y = hi > a; the hunt alternates hi / hi - s with no runs.
			const double hi = y, lo = y - s;
			const double yE = ( ( nE - n ) & 1 ) == 0 ? hi : lo;
			//The history at the edge alternates, so it cannot hold a run; the
			//climb's own bits make one after J of them. Start it as alternating.
			const int histE = ( ( nE - n ) & 1 ) == 0 ? 0x2 : 0x5;
			predicted[ adaptive ] = statedClimb( L, yE, s, histE & L.mask, b, N );

			k.showBits = true;
			std::vector< float > bits;
			if( !renderOnce( k, perturb, W, H, pic, bits ) )
				return report( false, quiet, "adapt: could not render" );
			k.showBits = false;
			int st = -1;
			measured[ adaptive ] = runFrom( bits, W, H / 2, nE, N, 1, st );
			ok = ok && measured[ adaptive ] == predicted[ adaptive ] && st == nE;
		}
		ok = ok && measured[ 1 ] < measured[ 0 ];
		failures += report( ok, quiet, "adapt climb: a 0.3 edge takes %d samples with a fixed step and %d adapting (predicted %d and %d): %d samples shorter, as predicted",
		                    measured[ 0 ], measured[ 1 ], predicted[ 0 ], predicted[ 1 ], measured[ 0 ] - measured[ 1 ] );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --tradeoff
//
// Over Adaptation Rate 0, 1/8, 1/4, 1/2, 1 -- the increment doubling each
// time -- a step edge from 0.5 to 0.9 with the leaks on (64 samples): the
// climb (Show Bits run) must never lengthen and must shorten overall, and
// the post-edge granular energy, sum ( y - b )^2 over the 48 samples after
// the climb, must rise at every step. Measured; nothing predicted.
//---------------------------------------------------------------------------
int runTradeoff( int W, int H, int perturb = 0, bool quiet = false )
{
	Knobs k;
	k.channels = 1;
	k.pitch    = 1;
	k.recon    = 1.0f;
	k.leak     = sliderForTauI( 64.0 );
	const int N  = W;
	const int nE = N * 2 / 5;
	//0.8: the adapted step reaches 1/8 and the overshoot must stay under the
	//display's ceiling, or the energy is measured through a clip.
	const double a = 0.5, b = 0.8;
	const Picture pic = stepEdge( W, H, nE, a, b );
	const float rates[] = { 0.0f, 0.125f, 0.25f, 0.5f, 1.0f };
	std::vector< int > climbs;
	std::vector< double > energies;
	bool clip = false;
	for( float rate : rates )
	{
		k.adapt    = rate;
		k.showBits = true;
		std::vector< float > bits, plain;
		if( !renderOnce( k, perturb, W, H, pic, bits ) )
			return report( false, quiet, "tradeoff: could not render" );
		k.showBits = false;
		if( !renderOnce( k, perturb, W, H, pic, plain ) )
			return report( false, quiet, "tradeoff: could not render" );
		int st = -1;
		const int m = runFrom( bits, W, H / 2, nE, N, 1, st );
		double energy = 0.0;
		for( int n = st + m; n < std::min( N, st + m + 48 ); ++n )
		{
			const double v = sampleValue( plain, W, H / 2, n );
			clip           = clip || clipped( v );
			const double d = v - b;
			energy += d * d;
		}
		climbs.push_back( m );
		energies.push_back( energy );
	}
	bool ok = true;
	std::string line;
	for( size_t i = 0; i < climbs.size(); ++i )
	{
		if( i > 0 )
			ok = ok && climbs[ i ] <= climbs[ i - 1 ] && energies[ i ] > energies[ i - 1 ];
		char buf[ 64 ];
		std::snprintf( buf, sizeof( buf ), "%s%g: %d / %.4f", i ? ", " : "", rates[ i ], climbs[ i ], energies[ i ] );
		line += buf;
	}
	ok = ok && climbs.back() < climbs.front() && !clip;
	return report( ok, quiet, "tradeoff over Adaptation Rate (climb samples / post-edge energy): %s -- climb never longer and shorter overall, energy rising at every step%s", line.c_str(), clip ? " -- CLIPPED" : "" );
}

//---------------------------------------------------------------------------
// --leak
//
// Adaptation off, no filter, a flat field at the rest level: one bit is
// forced wrong at sample s of the middle line. The decoder's guess is then
// 2 D the wrong way and, with the step fixed, the two decoders apply the
// same bits from then on, so their difference decays as -2 b_s D beta^k --
// the integrator's time constant and nothing else. Read out of two
// pictures, with and without the forced error, at 64 and 256 samples; the
// time constant is fitted from the first and last usable point. Also: the
// encoder does not know -- the received bitstreams differ in exactly one
// sample.
//---------------------------------------------------------------------------
int runLeak( int W, int H, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	for( double tau : { 64.0, 256.0 } )
	{
		Knobs k;
		k.channels = 1;
		k.pitch    = 1;
		k.recon    = 1.0f;
		k.adapt    = 0.0f;
		k.leak     = sliderForTauI( tau );
		const Law L   = statedFrom( k );
		const double D = L.stepMin, beta = L.leakI;
		const int N = W, row = H / 2, s = N / 2;
		const Picture pic = flat( W, H, 0.5 );
		Reference ref;
		ref.L = L;
		ref.W = W;
		ref.H = H;
		ref.run( pic, 0 );

		std::vector< float > plain, err, plainBits, errBits;
		if( !renderOnce( k, perturb, W, H, pic, plain ) || !renderOnce( k, perturb, W, H, pic, err, row, s, 0 ) )
			return report( false, quiet, "leak: could not render" );
		k.showBits = true;
		if( !renderOnce( k, perturb, W, H, pic, plainBits ) || !renderOnce( k, perturb, W, H, pic, errBits, row, s, 0 ) )
			return report( false, quiet, "leak: could not render" );

		const int bS     = sampleValue( plainBits, W, row, s ) >= 0.5 ? 1 : -1;
		const double d0  = -2.0 * bS * D;
		double worst = 0.0, lastD = 0.0;
		int usable = 0, lastK = 0;
		for( int kk = 0; s + kk < N; ++kk )
		{
			const int n      = s + kk;
			const double d   = sampleValue( err, W, row, n ) - sampleValue( plain, W, row, n );
			const double pred = d0 * std::pow( beta, kk );
			const double tol  = 2.0 * ref.line( row, 0 ).eYd[ n ] + 8.0 * kU * kM;
			worst             = std::max( worst, std::fabs( d - pred ) / tol );
			if( std::fabs( pred ) > 20.0 * tol )
			{
				usable = kk + 1;
				lastK  = kk;
				lastD  = d;
			}
		}
		const double fitted = lastK > 0 ? lastK / std::log( d0 / lastD ) : 0.0;
		int differ = 0;
		for( int r = 0; r < H; ++r )
			for( int n = 0; n < N; ++n )
				if( ( sampleValue( plainBits, W, r, n ) >= 0.5 ) != ( sampleValue( errBits, W, r, n ) >= 0.5 ) )
					++differ;
		failures += report( usable >= 20 && worst <= 1.0 && differ == 1, quiet,
		                    "leak tau = %3g samples: one bit forced at sample %d moves the guess %+.6f and it fades on beta^k; %d points on the curve (%d clear of the bound), worst %.3f of tolerance; fitted tau %.4g; received bitstreams differ in %d sample",
		                    tau, s, d0, N - s, usable, worst, fitted, differ );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --reference
//
// Every output pixel of three frames of noise against the serial double
// run, over five settings: two exact ones (tolerance 0) and three with
// leaks, filter, adaptation, errors, Y+C, a vertical scan, judged with the
// running float bound and the comparator's tie exclusion.
//---------------------------------------------------------------------------
struct Setting
{
	const char* name;
	Knobs knobs;
};

std::vector< Setting > referenceSettings()
{
	std::vector< Setting > list;
	{
		Setting s{ "RGB, no leak, fixed step, no filter, pitch 4 (exact)", {} };
		s.knobs.channels = 1;
		s.knobs.leak     = 0.0f;
		s.knobs.adapt    = 0.0f;
		s.knobs.recon    = 1.0f;
		s.knobs.pitch    = 4;
		list.push_back( s );
	}
	{
		Setting s{ "RGB, no leak, adapting, errors 1e-2, 4 bits, pitch 2 (exact)", {} };
		s.knobs.channels  = 1;
		s.knobs.leak      = 0.0f;
		s.knobs.adapt     = 0.5f;
		s.knobs.recon     = 1.0f;
		s.knobs.pitch     = 2;
		s.knobs.runLength = 1;
		s.knobs.errors    = sliderForErrorRate( 1e-2 );
		list.push_back( s );
	}
	{
		Setting s{ "Luma, leak 64, adapting, filter, errors 1e-3, pitch 1", {} };
		s.knobs.channels = 0;
		s.knobs.leak     = sliderForTauI( 64.0 );
		s.knobs.adapt    = 0.5f;
		s.knobs.recon    = 0.75f;
		s.knobs.pitch    = 1;
		s.knobs.errors   = sliderForErrorRate( 1e-3 );
		list.push_back( s );
	}
	{
		Setting s{ "Y+C, leak 64, adapting, filter, 4 bits, pitch 2", {} };
		s.knobs.channels  = 2;
		s.knobs.leak      = sliderForTauI( 64.0 );
		s.knobs.adapt     = 0.5f;
		s.knobs.recon     = 0.5f;
		s.knobs.pitch     = 2;
		s.knobs.runLength = 1;
		list.push_back( s );
	}
	{
		Setting s{ "RGB vertical, leak 256, adapting, filter, errors 1e-3, pitch 3", {} };
		s.knobs.channels = 1;
		s.knobs.scan     = 1;
		s.knobs.leak     = 0.4f;
		s.knobs.adapt    = 0.25f;
		s.knobs.recon    = 0.75f;
		s.knobs.pitch    = 3;
		s.knobs.errors   = sliderForErrorRate( 1e-3 );
		list.push_back( s );
	}
	return list;
}

int runReference( int W, int H, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	for( const Setting& setting : referenceSettings() )
	{
		Law L       = statedFrom( setting.knobs );
		L.exact     = !exactBecause( L ).empty();
		Session s;
		prepare( s, setting.knobs, perturb );
		Reference ref;
		ref.L = L;
		ref.W = W;
		ref.H = H;
		if( !s.begin( W, H ) )
			return report( false, quiet, "reference: could not render" );

		double worst = 0.0, worstRatio = 0.0;
		size_t compared = 0, excluded = 0;
		int ties = 0;
		for( int f = 0; f < 3; ++f )
		{
			const Picture pic = noise( W, H, static_cast< uint32_t >( 17 + f ) );
			const uint32_t seed = s.nextSeed();
			if( !s.render( pic ) )
				return report( false, quiet, "reference: could not render" );
			ref.run( pic, seed );
			for( const LineRun& ln : ref.lines )
				if( ln.tie >= 0 )
					++ties;
			const std::vector< float > out = s.readBackFloat();
			for( int r = 0; r < H; ++r )
				for( int c = 0; c < W; ++c )
				{
					double rgb[ 3 ], bound[ 3 ];
					if( !ref.expected( pic, r, c, rgb, bound ) )
					{
						excluded += 3;
						continue;
					}
					for( int ch = 0; ch < 3; ++ch )
					{
						const double d = std::fabs( at( out, W, r, c, ch ) - rgb[ ch ] );
						worst          = std::max( worst, d );
						if( bound[ ch ] > 0.0 )
							worstRatio = std::max( worstRatio, d / bound[ ch ] );
						else if( d > 0.0 )
							worstRatio = std::max( worstRatio, 1e9 );
						++compared;
					}
				}
		}
		s.end();
		const double excludedFraction = static_cast< double >( excluded ) / static_cast< double >( compared + excluded );
		const bool ok = ( L.exact ? worst == 0.0 : worstRatio <= 1.0 ) && excludedFraction < 0.5;
		failures += report( ok, quiet, "reference %-64s %zu values, worst %.3g%s; %d coder lines hit a comparator tie, %.1f%% of values past one excluded",
		                    setting.name, compared, worst, L.exact ? " (exact: must be 0)" : ( std::string( " = " ) + std::to_string( worstRatio ).substr( 0, 5 ) + " of the running bound" ).c_str(), ties,
		                    100.0 * excludedFraction );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --vertical
//
// The transposed picture under a vertical scan, against the picture under a
// horizontal one, transposed back: the same lines, the same samples, the
// same hash indices, the same arithmetic in the same order -- so equal
// exactly, in every setting, including Y+C with leaks, filter, adaptation
// and errors.
//---------------------------------------------------------------------------
int runVertical( int W, int H, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	for( int channels : { 0, 2 } )
	{
		Knobs k;
		k.channels = channels;
		k.pitch    = 2;
		k.errors   = sliderForErrorRate( 1e-3 );
		const Picture pic = noise( W, H, 99u + channels );
		const Picture picT = transposed( pic, W, H );
		std::vector< float > outH, outV;
		k.scan = 0;
		if( !renderOnce( k, perturb, W, H, pic, outH ) )
			return report( false, quiet, "vertical: could not render" );
		k.scan = 1;
		if( !renderOnce( k, perturb, H, W, picT, outV ) )
			return report( false, quiet, "vertical: could not render" );
		size_t differ = 0;
		double worst = 0.0;
		for( int r = 0; r < W; ++r )//outV is H wide, W tall
			for( int c = 0; c < H; ++c )
				for( int ch = 0; ch < 4; ++ch )
				{
					const double d = std::fabs( at( outV, H, r, c, ch ) - at( outH, W, c, r, ch ) );
					if( d != 0.0 )
						++differ;
					worst = std::max( worst, d );
				}
		failures += report( differ == 0, quiet, "vertical %-4s %dx%d scanned down its columns equals the %dx%d picture scanned along its rows, transposed: %zu of %d values differ, worst %.3g",
		                    model::kChannelNames[ channels ], H, W, W, H, differ, W * H * 4, worst );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --errors
//
// An exact setting with Bit Errors at 1e-2: the received bits (Show Bits)
// against the reference's transmitted ones count the flips, which must be
// Binomial( n, p ) to four sigma on each of two frames; and the two frames'
// flip masks must differ in 2 n p ( 1 - p ) positions to four sigma, which
// is the per-frame seed doing its job.
//---------------------------------------------------------------------------
int runErrors( int W, int H, int perturb = 0, bool quiet = false )
{
	Knobs k;
	k.channels = 1;
	k.leak     = 0.0f;
	k.adapt    = 0.0f;
	k.recon    = 1.0f;
	k.pitch    = 1;
	k.errors   = sliderForErrorRate( 1e-2 );
	k.showBits = true;
	Law L      = statedFrom( k );
	L.exact    = !exactBecause( L ).empty();
	const double p = L.errorRate;
	Session s;
	prepare( s, k, perturb );
	if( !s.begin( W, H ) )
		return report( false, quiet, "errors: could not render" );
	Reference ref;
	ref.L = L;
	ref.W = W;
	ref.H = H;
	std::vector< std::vector< unsigned char > > masks;
	long counts[ 2 ] = { 0, 0 };
	const long n = static_cast< long >( W ) * H * 3;
	for( int f = 0; f < 2; ++f )
	{
		const Picture pic = noise( W, H, 300u + f );
		const uint32_t seed = s.nextSeed();
		if( !s.render( pic ) )
			return report( false, quiet, "errors: could not render" );
		ref.run( pic, seed );
		const std::vector< float > out = s.readBackFloat();
		std::vector< unsigned char > mask( static_cast< size_t >( n ), 0 );
		for( int r = 0; r < H; ++r )
			for( int c = 0; c < W; ++c )
				for( int ch = 0; ch < 3; ++ch )
				{
					const int rx = at( out, W, r, c, ch ) >= 0.5f ? 1 : 0;
					const int tx = ref.line( r, ch ).tx[ c ];
					const unsigned char flipped = rx != tx;
					mask[ ( static_cast< size_t >( r ) * W + c ) * 3 + ch ] = flipped;
					counts[ f ] += flipped;
				}
		masks.push_back( mask );
	}
	s.end();
	const double expected = n * p, sigma = std::sqrt( n * p * ( 1.0 - p ) );
	long differ = 0;
	for( size_t i = 0; i < masks[ 0 ].size(); ++i )
		differ += masks[ 0 ][ i ] != masks[ 1 ][ i ];
	const double expectedDiffer = 2.0 * n * p * ( 1.0 - p ), sigmaDiffer = std::sqrt( expectedDiffer );
	const bool ok = std::fabs( counts[ 0 ] - expected ) <= 4.0 * sigma && std::fabs( counts[ 1 ] - expected ) <= 4.0 * sigma
	                && std::fabs( differ - expectedDiffer ) <= 4.0 * sigmaDiffer;
	return report( ok, quiet, "errors p = %g: %ld and %ld of %ld received bits flipped (expected %.0f +- %.0f); the two frames' flips differ in %ld positions (expected %.0f +- %.0f)",
	               p, counts[ 0 ], counts[ 1 ], n, expected, 4.0 * sigma, differ, expectedDiffer, 4.0 * sigmaDiffer );
}

//---------------------------------------------------------------------------
// --laws (no GL): every conversion in Controls.cpp against the statements
// above, at 21 points, and the dyadic promises at the sliders the checks use.
//---------------------------------------------------------------------------
int runLaws( bool quiet = false )
{
	namespace controls = slopefx::controls;
	int failures = 0;
	double worst = 0.0;
	auto rel     = []( double a, double b ) { return b == 0.0 ? std::fabs( a ) : std::fabs( a - b ) / std::fabs( b ); };
	for( int i = 0; i <= 20; ++i )
	{
		const float v      = static_cast< float >( i ) / 20.0f;
		const double smin  = statedStepMin( v );
		const double smax  = statedStepMax( v, smin );
		worst = std::max( { worst, rel( controls::StepMin( v ), smin ), rel( controls::StepMax( v, smin ), smax ),
		                    rel( controls::StepAdd( v, smax ), statedStepAdd( v, smax ) ), rel( controls::IntegratorTau( v ), statedTauI( v ) ),
		                    rel( controls::IntegratorLeak( v ), statedLeakI( v ) ), rel( controls::SyllabicLeak( v ), statedLeakS( v ) ),
		                    rel( controls::ErrorRate( v ), statedErrorRate( v ) ), rel( controls::ReconstructionAlpha( v ), statedAlpha( v ) ),
		                    rel( controls::ReconstructionTau( v ), statedTauR( v ) ),
		                    rel( controls::ErrorThreshold( statedErrorRate( v ) ), statedErrorThreshold( statedErrorRate( v ) ) ) } );
		if( controls::RunLength( v ) != statedRunLength( v ) || controls::Pitch( v * 16.0f ) != statedPitch( v * 16.0f ) )
			worst = 1.0;
	}
	failures += report( worst < 1e-12, quiet, "laws: Step Size, Max Step, Adaptation Rate, Leak (both time constants), Bit Errors, Reconstruction, Run Length and Pixels/Sample as stated at 21 points, worst %.2g relative", worst );

	const bool dyadic = controls::StepMin( 0.5f ) == 1.0 / 64.0 && controls::StepMax( 0.5f, 1.0 / 64.0 ) == 1.0 / 8.0
	                    && controls::StepAdd( 0.5f, 1.0 / 8.0 ) == 1.0 / 128.0 && controls::StepMin( 0.0f ) == 1.0 / 256.0
	                    && controls::StepMax( 1.0f, 1.0 / 256.0 ) == 1.0 && controls::IntegratorLeak( 0.0f ) == 1.0
	                    && controls::SyllabicLeak( 0.0f ) == 1.0 && controls::ReconstructionAlpha( 1.0f ) == 0.0;
	//0.6f is not 0.6, so the 64-sample leak the checks ask for is 63.99999
	//samples, on both sides: the float slider is the statement, not the decimal.
	const double tau64 = controls::IntegratorTau( sliderForTauI( 64.0 ) );
	failures += report( dyadic && std::fabs( tau64 - 64.0 ) < 1e-4, quiet, "laws: the dyadic promises hold exactly -- 1/64, 1/8, 1/128 at the middle sliders; no leak and no filter are exactly 1 and 0; the 64-sample slider gives %.6f", tau64 );
	return failures;
}

int runNames( bool quiet = false )
{
	Slope plugin;
	std::set< std::string > seen;
	int bad = 0;
	for( const NamedParameter& p : listParameters( plugin ) )
		if( p.name.size() > 16 || !seen.insert( p.name ).second )
			++bad;

	//The name the host reads: plugMain's info block, 16 bytes, not
	//null-terminated.
	const FFMixed info            = plugMain( FF_GET_INFO, FFMixed{ 0 }, 0 );
	const PluginInfoStruct* block = static_cast< const PluginInfoStruct* >( info.PointerValue );
	std::string name, id;
	if( block )
	{
		name.assign( block->PluginName, strnlen( block->PluginName, 16 ) );
		id.assign( block->PluginUniqueID, 4 );
	}
	return report( bad == 0 && block && name == "SW Slope" && id == "SL01" && block->PluginType == FF_EFFECT, quiet,
	               "names: %zu parameters, unique and within 16 characters; the host reads '%s' / %s / %s", seen.size(), name.c_str(),
	               id.c_str(), block && block->PluginType == FF_EFFECT ? "effect" : "not an effect" );
}

//---------------------------------------------------------------------------
// --negative: every check above can fail.
//---------------------------------------------------------------------------
struct NegativeControl
{
	const char* what;
	int failuresSeen;
};

int summariseNegatives( const std::vector< NegativeControl >& controls )
{
	int failures = 0;
	for( const NegativeControl& c : controls )
	{
		const bool ok = c.failuresSeen > 0;
		++g_checks;
		std::printf( "negative %-64s %s  %s\n", c.what, ok ? "it failed" : "it PASSED", verdict( ok ) );
		if( !ok )
		{
			++failures;
			++g_failures;
		}
	}
	std::printf( "%s\n", failures == 0 ? "negative: every perturbed coder is caught" : "negative: FAILURES -- a check cannot fail" );
	return failures;
}

/// Run a check against a perturbation quietly, and report whether it failed
/// -- without counting its failures as the run's.
template< typename F >
int caught( F&& check )
{
	const int checks = g_checks, failures = g_failures;
	const int seen   = check();
	g_checks         = checks;
	g_failures       = failures;
	return seen;
}

int runNegative( int W, int H )
{
	return summariseNegatives( {
		{ "overload: the integrators add 1.25 steps per bit", caught( [ & ] { return runOverload( W, H, model::kPerturbStepBias, true ); } ) },
		{ "granular: the integrators leak toward black", caught( [ & ] { return runGranular( W, H, model::kPerturbLeakToBlack, true ); } ) },
		{ "adapt: the run detector removed, a fixed step (the spec's)", caught( [ & ] { return runAdapt( W, H, model::kPerturbFixedStep, true ); } ) },
		{ "tradeoff: the run detector removed, a fixed step", caught( [ & ] { return runTradeoff( W, H, model::kPerturbFixedStep, true ); } ) },
		{ "leak: the decoder's integrator does not leak", caught( [ & ] { return runLeak( W, H, model::kPerturbNoDecoderLeak, true ); } ) },
		{ "reference: every chunk restarts from the fixed state (the spec's)", caught( [ & ] { return runReference( W, H, model::kPerturbRestart, true ); } ) },
		{ "reference: Y+C chroma at the full sample rate", caught( [ & ] { return runReference( W, H, model::kPerturbChromaFullRate, true ); } ) },
		{ "vertical: the vertical scan runs up the columns", caught( [ & ] { return runVertical( W, H, model::kPerturbVerticalFlip, true ); } ) },
		{ "errors: the error rate halved", caught( [ & ] { return runErrors( W, H, model::kPerturbHalfErrors, true ); } ) },
	} );
}

//---------------------------------------------------------------------------
// The moving card, for --out, the sweep, the bench and a default --pipe:
// colour bars (hard edges in every channel), a ramp, a block that cuts, a
// moving bar.
//---------------------------------------------------------------------------
std::vector< unsigned char > buildCard( int width, int height, int64_t frame )
{
	std::vector< unsigned char > img( static_cast< size_t >( width ) * height * 4 );
	const double t    = static_cast< double >( frame ) / 60.0;
	const bool flash  = std::fmod( t, 0.4 ) < 0.2;
	const double barX = std::fmod( 40.0 + 240.0 * t, static_cast< double >( width ) );
	const unsigned char bars[ 7 ][ 3 ] = { { 191, 191, 191 }, { 191, 191, 0 }, { 0, 191, 191 }, { 0, 191, 0 },
		                                   { 191, 0, 191 },   { 191, 0, 0 },   { 0, 0, 191 } };
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double fx = ( x + 0.5 ) / width, fy = ( y + 0.5 ) / height;
			double r = 16, g = 16, b = 16;
			if( fy < 0.3 )
			{
				const unsigned char* c = bars[ std::min( 6, x * 7 / width ) ];
				r = c[ 0 ];
				g = c[ 1 ];
				b = c[ 2 ];
			}
			else if( fy < 0.45 )
				r = g = b = 255.0 * fx;//a ramp
			else if( fx > 0.3 && fx < 0.7 && fy > 0.5 && fy < 0.85 )
				r = g = b = flash ? 235.0 : 16.0;//the block that cuts
			else if( fy > 0.9 )
			{
				r = 200;
				g = 60;
				b = 30;
			}
			if( std::fabs( x + 0.5 - barX ) < std::max( 2.0, width / 120.0 ) )
				r = g = b = 250.0;
			unsigned char* px = img.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
			px[ 0 ] = static_cast< unsigned char >( r );
			px[ 1 ] = static_cast< unsigned char >( g );
			px[ 2 ] = static_cast< unsigned char >( b );
			px[ 3 ] = 255;
		}
	return img;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
double benchAt( const std::vector< std::string >& settings, int width, int height, int frames )
{
	Session session;
	session.floatOutput = false;
	for( const std::string& setting : settings )
	{
		std::string error;
		applySetting( session.plugin, setting, error );
	}
	if( !session.begin( width, height ) )
		return -1.0;

	std::vector< std::vector< unsigned char > > loop;
	for( int i = 0; i < 4; ++i )
		loop.push_back( buildCard( width, height, i * 7 ) );

	const int warmup = 20;
	for( int frame = 0; frame < warmup; ++frame )
		session.render( loop[ static_cast< size_t >( frame ) % loop.size() ] );
	glFinish();

	//Best of three: the GPU is shared with other builds on this machine.
	double best = 1e9;
	for( int run = 0; run < 3; ++run )
	{
		const auto start = std::chrono::steady_clock::now();
		for( int i = 0; i < frames; ++i )
			session.renderNow();
		glFinish();
		const double seconds = std::chrono::duration< double >( std::chrono::steady_clock::now() - start ).count();
		best                 = std::min( best, seconds * 1000.0 / frames );
	}
	session.end();
	return best;
}

int runBench( const std::vector< std::string >& settings, int frames )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	const Size sizes[] = { { "1280x720  ", 1280, 720 }, { "1920x1080 ", 1920, 1080 }, { "3840x2160 ", 3840, 2160 } };
	std::printf( "%d frames each, best of three runs, after a 20-frame warm-up, glFinish both sides.\n\n", frames );
	std::printf( "resolution     ms/frame   %% of a 60fps frame   RGB, 1 pixel/sample\n" );
	std::vector< std::string > heavy = settings;
	heavy.push_back( "Channels=1" );
	heavy.push_back( "Pixels/Sample=1" );
	for( const Size& size : sizes )
	{
		const double ms = benchAt( settings, size.width, size.height, frames );
		const double hv = benchAt( heavy, size.width, size.height, frames );
		std::printf( "%s    %7.3f        %5.1f%%             %7.3f\n", size.name, ms, ms / 16.667 * 100.0, hv );
	}
	std::printf( "\nEach frame: one sample pass, one coder draw per %d samples of the scan, the display.\n", model::kChunk );
	return 0;
}

//---------------------------------------------------------------------------
// --dump-shaders
//---------------------------------------------------------------------------
int dumpShaders( const std::string& dir )
{
	namespace sh = slopefx::shaders;
	const std::pair< const char*, const char* > files[] = {
		{ "vertex.vert", sh::kVertex }, { "sample.frag", sh::kSample }, { "coder.frag", sh::kCoder }, { "display.frag", sh::kDisplay },
	};
	for( const auto& f : files )
	{
		std::ofstream out( dir + "/" + f.first );
		if( !out )
		{
			std::fprintf( stderr, "cannot write %s/%s\n", dir.c_str(), f.first );
			return 1;
		}
		out << f.second;
	}
	std::printf( "wrote %zu shaders to %s\n", sizeof( files ) / sizeof( files[ 0 ] ), dir.c_str() );
	return 0;
}

//---------------------------------------------------------------------------
// --pipe cue sheet: one 'frame Name Value' per line, the fleet's format.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}
	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );
		int frame = 0;
		if( !( in >> frame ) )
			continue;
		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}
		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];
		tracks[ name ].emplace_back( frame, value );
	}
	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

float valueAt( const Track& track, int frame )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 1; i < track.size(); ++i )
		if( frame <= track[ i ].first )
		{
			const auto& a    = track[ i - 1 ];
			const auto& b    = track[ i ];
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? static_cast< float >( frame - a.first ) / span : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	return track.back().second;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"sltest -- render and measure the Slope delta modulator\n"
		"\n"
		"  --out PATH          render the moving card through the plugin (default /tmp/slope.png)\n"
		"  --size WxH          raster (default 1280x720); --width N / --height N also accepted\n"
		"  --frames N          frames to render before reading back (default 40)\n"
		"  --fps N             accepted for the fleet's --pipe contract; this plugin has no clock\n"
		"  --set \"Name=V\"      set a parameter by its display name (element index for options,\n"
		"                      the integer for Pixels/Sample). Repeatable.\n"
		"  --list              every parameter, its kind, default and range\n"
		"\n"
		"  checks that render, at --size:\n"
		"  --overload          a step of height h climbs in ceil( h / step ) samples, whole- and part-sample edges\n"
		"  --granular          a flat field's idle pattern has period 2 and a peak-to-peak of one step\n"
		"  --adapt             the step grows by the stated law per run; the climb shortens by the predicted count\n"
		"  --tradeoff          over Adaptation Rate the climb shortens and the post-edge hunt's energy rises\n"
		"  --leak              a forced bit error fades on the integrator's time constant\n"
		"  --reference         every pixel against a serial double run of the recurrence\n"
		"  --vertical          a vertical scan of the transposed picture is the horizontal one transposed, exactly\n"
		"  --errors            received bits are flipped at the stated rate, differently every frame\n"
		"  --negative          every check above can fail\n"
		"  --perturb BITS      run the checks verbosely against a perturbed coder (bits in Model.h)\n"
		"\n"
		"  checks that need no GL:\n"
		"  --laws              every control law against its statement; the dyadic promises\n"
		"  --names             nothing the host will silently truncate; the host reads SW Slope / SL01\n"
		"  --offline           both; says loudly what it skipped. For CI.\n"
		"  --allow-no-gl       with the rendering checks: SKIP loudly, not FAIL, when no GL 4.1 context exists\n"
		"\n"
		"  --bench             time ProcessOpenGL at 720p, 1080p and 4K\n"
		"  --dump-shaders DIR  write the exact GLSL the plugin compiles\n"
		"  --pipe              raw RGBA frames on stdin, raw RGBA frames on stdout\n"
		"  --script PATH       parameter cues for --pipe: 'frame Name Value'\n"
		"  --help\n" );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outPath = "/tmp/slope.png";
	std::string scriptPath;
	std::string dumpDir;
	int width      = 1280;
	int height     = 720;
	int frames     = 40;
	int failRender = -1;
	int perturb    = 0;
	double fps     = 60.0;
	bool wantList  = false;
	bool wantBench = false;
	bool wantPipe  = false;
	bool allowNoGL = false;
	std::vector< std::string > settings;
	std::vector< std::string > checks;

	const std::set< std::string > rendered = { "--overload", "--granular", "--adapt", "--tradeoff", "--leak", "--reference", "--vertical", "--errors", "--negative" };
	const std::set< std::string > offline  = { "--laws", "--names" };

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		if( argument == "--help" || argument == "-h" )
		{
			usage();
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--dump-shaders" && hasNext )
			dumpDir = argv[ ++i ];
		else if( argument == "--size" && hasNext )
		{
			const std::string size = argv[ ++i ];
			const size_t x         = size.find( 'x' );
			if( x == std::string::npos )
			{
				std::fprintf( stderr, "--size wants WxH\n" );
				return 2;
			}
			width  = std::atoi( size.substr( 0, x ).c_str() );
			height = std::atoi( size.substr( x + 1 ).c_str() );
		}
		else if( argument == "--width" && hasNext )
			width = std::atoi( argv[ ++i ] );
		else if( argument == "--height" && hasNext )
			height = std::atoi( argv[ ++i ] );
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--perturb" && hasNext )
			perturb = std::atoi( argv[ ++i ] );
		else if( argument == "--fail-render-at" && hasNext )
			failRender = std::atoi( argv[ ++i ] );//test hook: verify.sh proves --pipe exits 1 on a failed render
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--bench" )
			wantBench = true;
		else if( argument == "--pipe" )
			wantPipe = true;
		else if( argument == "--allow-no-gl" )
			allowNoGL = true;
		else if( argument == "--offline" )
			for( const char* m : { "--laws", "--names" } )
				checks.push_back( m );
		else if( rendered.count( argument ) || offline.count( argument ) )
			checks.push_back( argument );
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", argument.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 || fps <= 0.0 )
	{
		std::fprintf( stderr, "width, height, frames and fps must all be positive\n" );
		return 2;
	}

	if( !dumpDir.empty() )
		return dumpShaders( dumpDir );

	if( wantList )
	{
		//No GL needed: answered before a context is made, so it works in CI.
		Slope plugin;
		std::printf( "%3s  %-16s  %-9s  %-8s  %s\n", "id", "name", "kind", "default", "range" );
		for( const NamedParameter& p : listParameters( plugin ) )
			std::printf( "%3u  %-16s  %-9s  %.4f    [%g..%g]\n", p.index, p.name.c_str(), kindName( p ), p.value, p.low, p.high );
		return 0;
	}

	if( !checks.empty() )
	{
		bool needGL     = false;
		bool offlineRan = false;
		for( const std::string& check : checks )
		{
			if( check == "--laws" )
				runLaws();
			else if( check == "--names" )
				runNames();
			else
			{
				needGL = true;
				continue;
			}
			offlineRan = true;
			std::printf( "\n" );
		}
		if( offlineRan && !needGL )
			std::printf( "   OFFLINE: --overload, --granular, --adapt, --tradeoff, --leak, --reference,\n"
			             "   --vertical, --errors and their negative controls were NOT run. Nothing here\n"
			             "   drew a pixel through a GL driver; the shaders were not exercised, only (in CI)\n"
			             "   compiled by glslc. The laws have no negative control of their own.\n\n" );

		if( needGL )
		{
			CGLContextObj context = createContext();
			if( context == nullptr && allowNoGL )
				std::printf( "   SKIP  could not create an OpenGL 4.1 core context, accelerated or software.\n"
				             "         The rendering checks and their negative controls were NOT run.\n" );
			else if( context == nullptr )
			{
				std::printf( "   FAIL  could not create an OpenGL 4.1 core context\n" );
				++g_failures;
			}
			else
			{
				for( const std::string& check : checks )
				{
					if( check == "--overload" )
						runOverload( width, height, perturb );
					else if( check == "--granular" )
						runGranular( width, height, perturb );
					else if( check == "--adapt" )
						runAdapt( width, height, perturb );
					else if( check == "--tradeoff" )
						runTradeoff( width, height, perturb );
					else if( check == "--leak" )
						runLeak( width, height, perturb );
					else if( check == "--reference" )
						runReference( width, height, perturb );
					else if( check == "--vertical" )
						runVertical( width, height, perturb );
					else if( check == "--errors" )
						runErrors( width, height, perturb );
					else if( check == "--negative" )
						runNegative( width, height );
					else
						continue;
					std::printf( "\n" );
				}
				CGLSetCurrentContext( nullptr );
				CGLDestroyContext( context );
			}
		}
		std::printf( "%d checks, %d failed\n", g_checks, g_failures );
		return g_failures == 0 ? 0 : 1;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL context\n" );
		return 1;
	}
	auto finish = [ & ]( int result ) {
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return result;
	};

	if( wantBench )
		return finish( runBench( settings, frames < 40 ? 60 : frames ) );

	Session session;
	session.floatOutput = false;
	for( const std::string& setting : settings )
	{
		std::string error;
		if( applySetting( session.plugin, setting, error ) )
			continue;
		std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
		return finish( 2 );
	}

	if( wantPipe )
	{
		//Everything but the video goes to stderr: one stray byte in stdout is
		//a torn frame for the rest of the reel.
		std::map< unsigned int, Track > automation;
		if( !scriptPath.empty() )
		{
			std::string error;
			const std::map< std::string, Track > tracks = loadScript( scriptPath, error );
			if( !error.empty() )
			{
				std::fprintf( stderr, "%s\n", error.c_str() );
				return finish( 2 );
			}
			for( const auto& entry : tracks )
			{
				const int index = indexOfParameter( session.plugin, entry.first );
				if( index < 0 )
				{
					std::fprintf( stderr, "script names '%s', which is not a parameter (try --list)\n", entry.first.c_str() );
					return finish( 2 );
				}
				automation[ static_cast< unsigned int >( index ) ] = entry.second;
			}
		}

		//A closed stdout must be a failed write we can see, not a SIGPIPE
		//that kills the process with 141 before it can say so.
		std::signal( SIGPIPE, SIG_IGN );

		if( !session.begin( width, height ) )
			return finish( 1 );

		std::vector< unsigned char > frame( static_cast< size_t >( width ) * height * 4 );
		int status = 0;
		for( int index = 0;; ++index )
		{
			size_t got = 0;
			while( got < frame.size() )
			{
				const ssize_t n = read( STDIN_FILENO, frame.data() + got, frame.size() - got );
				if( n <= 0 )
					break;
				got += static_cast< size_t >( n );
			}
			//A partial frame is the end of the stream, never a frame.
			if( got < frame.size() )
			{
				if( got > 0 )
					std::fprintf( stderr, "partial frame at the end (%zu of %zu bytes, %dx%d): dropped\n", got, frame.size(), width, height );
				break;
			}

			//Through the plugin's own setter, so a cue moves what a slider would.
			for( const auto& track : automation )
				session.plugin.SetFloatParameter( track.first, valueAt( track.second, index ) );

			const bool rendered = index != failRender && session.render( frame );
			if( !rendered )
			{
				std::fprintf( stderr, "render failed at frame %d\n", index );
				status = 1;
				break;
			}

			const std::vector< unsigned char > out = session.readBack();
			size_t written                         = 0;
			while( written < out.size() )
			{
				const ssize_t put = write( STDOUT_FILENO, out.data() + written, out.size() - written );
				if( put <= 0 )
					break;
				written += static_cast< size_t >( put );
			}
			//The reader has gone: rendering on into a closed pipe is work
			//nobody will see, and a short frame is worse than none.
			if( written < out.size() )
			{
				std::fprintf( stderr, "stdout closed at frame %d\n", index );
				status = 1;
				break;
			}
		}
		session.end();
		return finish( status );
	}

	if( !session.begin( width, height ) )
		return finish( 1 );
	for( int frame = 0; frame < frames; ++frame )
		if( !session.render( buildCard( width, height, frame ) ) )
			return finish( 1 );

	const std::vector< unsigned char > image = session.readBack();
	session.end();
	if( !writePng( outPath, width, height, image ) )
	{
		std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
		return finish( 1 );
	}
	std::printf( "wrote %s (%dx%d, %d frames)\n", outPath.c_str(), width, height, frames );
	return finish( 0 );
}
