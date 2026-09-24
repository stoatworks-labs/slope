#pragma once

/**
	What a host parameter means.

	Every ranged host parameter is 0..1 (SetParamInfo clamps a STANDARD
	default into 0..1 before a range can be attached), and an option
	parameter's range reads back 0..1 whatever its element count -- so an
	option is its element INDEX, rounded and clamped here, never a fraction
	of a range. Pixels/Sample is a real FF_TYPE_INTEGER with a real range.
	Every conversion to a physical unit lives here and nowhere else; sltest
	states the same laws from their definitions and --laws holds the two
	together.

	The powers of two are deliberate: a slider at 0, 0.5 or 1 gives a step
	that is exact in float, so the harness can choose settings in which the
	whole coder is exact arithmetic and compare against its reference with a
	tolerance of zero (AGENTS.md, "Would this hold on another rasteriser").
*/
namespace slopefx::controls
{

/// An option's stored value, as an index into its `count` elements.
int OptionIndex( float value, int count );

/// Step Size: the minimum step, 2^( -8 + 4 v ): one 8-bit code at 0, 1/64
/// at the default 0.5, sixteen codes at 1.
double StepMin( float value );

/// Max Step: 2^( -6 + 6 v ), 1/64 to the whole range, 1/8 at the default
/// 0.5 -- and never below the minimum step.
double StepMax( float value, double stepMin );

/// Adaptation Rate: what one run adds to the step, v x Max Step / 8. Zero
/// is no adaptation at all: the step stays at Step Size.
double StepAdd( float value, double stepMax );

/// Run Length: the option's element as J, 3 or 4 bits.
int RunLength( float value );

/// Leak: the integrator's time constant in samples. 0 is no leak (beta =
/// 1); otherwise 4 x 2^( 10 ( 1 - v ) ), 4096 samples just above 0 down to
/// 4 at 1; 256 at the default 0.4. Returned as 0 for none.
double IntegratorTau( float value );

/// The syllabic filter's time constant is a quarter of the integrator's:
/// the step must fall back within a line where the level may hold across
/// many. (Speech codecs have it the other way round; a picture is not
/// speech. AGENTS.md.)
double SyllabicTau( float value );

/// The leak coefficients, exp( -1 / tau ), exactly 1 for no leak.
double IntegratorLeak( float value );
double SyllabicLeak( float value );

/// Pixels/Sample: the integer, clamped to its range.
int Pitch( float value );

/// Bit Errors: the probability a received bit is flipped. 0 at 0, else
/// 10^( -5 + 4 v ): one in a hundred thousand just above 0, one in ten at 1.
double ErrorRate( float value );

/// The error rate as the 32-bit hash threshold the shader compares against.
unsigned int ErrorThreshold( double rate );

/// Reconstruction: the decoder's low-pass, as a time constant in samples,
/// 2^( 6 ( 1 - v ) ) - 1: 63 samples at 0, none at 1 (the bare staircase),
/// 1.83 at the default 0.75.
double ReconstructionTau( float value );

/// Its coefficient exp( -1 / tau_r ), exactly 0 for no filter.
double ReconstructionAlpha( float value );

/// 2^e, exact for an integral e on every libm (ldexp), so the dyadic
/// settings above are dyadic in fact and not only in intent.
double Pow2( double e );

} // namespace slopefx::controls
