#include "Controls.h"

#include "Model.h"

#include <algorithm>
#include <cmath>

namespace slopefx::controls
{
namespace
{
double unit( float value )
{
	return std::clamp( static_cast< double >( value ), 0.0, 1.0 );
}
} // namespace

double Pow2( double e )
{
	const double whole = std::floor( e );
	return std::ldexp( std::exp2( e - whole ), static_cast< int >( whole ) );
}

int OptionIndex( float value, int count )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

double StepMin( float value )
{
	return Pow2( -8.0 + 4.0 * unit( value ) );
}

double StepMax( float value, double stepMin )
{
	return std::max( stepMin, Pow2( -6.0 + 6.0 * unit( value ) ) );
}

double StepAdd( float value, double stepMax )
{
	return unit( value ) * stepMax / 8.0;
}

int RunLength( float value )
{
	return model::kRunLengths[ OptionIndex( value, model::kRunLengthCount ) ];
}

double IntegratorTau( float value )
{
	const double v = unit( value );
	return v <= 0.0 ? 0.0 : 4.0 * Pow2( 10.0 * ( 1.0 - v ) );
}

double SyllabicTau( float value )
{
	return IntegratorTau( value ) / 4.0;
}

double IntegratorLeak( float value )
{
	const double tau = IntegratorTau( value );
	return tau <= 0.0 ? 1.0 : std::exp( -1.0 / tau );
}

double SyllabicLeak( float value )
{
	const double tau = SyllabicTau( value );
	return tau <= 0.0 ? 1.0 : std::exp( -1.0 / tau );
}

int Pitch( float value )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), model::kPitchMin, model::kPitchMax );
}

double ErrorRate( float value )
{
	const double v = unit( value );
	return v <= 0.0 ? 0.0 : std::pow( 10.0, -5.0 + 4.0 * v );
}

unsigned int ErrorThreshold( double rate )
{
	//Rounded, in double: 4294967296 x 0.1 is far inside the range.
	return static_cast< unsigned int >( std::llround( std::clamp( rate, 0.0, 0.5 ) * 4294967296.0 ) );
}

double ReconstructionTau( float value )
{
	return Pow2( 6.0 * ( 1.0 - unit( value ) ) ) - 1.0;
}

double ReconstructionAlpha( float value )
{
	const double tau = ReconstructionTau( value );
	return tau <= 0.0 ? 0.0 : std::exp( -1.0 / tau );
}

} // namespace slopefx::controls
