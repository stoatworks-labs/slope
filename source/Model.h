#pragma once

#include <cstdint>

/**
	The coder as arithmetic: what the shaders compute, written down once, with
	no GL in it. The plugin's C++ uses only the constants and the option
	tables; the recurrence itself runs in GLSL (`Shaders.cpp`) and the harness
	restates it in double from this description.

	**One rule, per sample, per channel.** A delta modulator sends one bit a
	sample: is the input above or below the decoder's current guess? Encoder
	and decoder are the same state machine (y, step, the last J bits):

	    b      = ( x >= y ) ? +1 : -1                the bit (a tie is +)
	    hist   = ( ( hist << 1 ) | ( b > 0 ) ) & mask   the last J bits
	    run    = hist == 0 || hist == mask           J identical bits in a row
	    step   = clamp( beta_s step + ( run ? step_add : 0 ), step_min, step_max )
	    y      = rest + beta_i ( y - rest ) + b step

	with rest = kRestLevel (mid-grey: the integrator is AC-coupled and leaks
	toward its bias point), beta_i = exp( -1 / tau_i ) the integrator's leak
	and beta_s = exp( -1 / tau_s ) the syllabic filter's, both in samples;
	no leak is beta = 1 exactly. The step is updated by THIS sample's bit
	before it is integrated, as the hardware's syllabic filter sees the
	coincidence of the current bits. The channel flips the bit the decoder
	receives with probability p (an integer hash of frame, line, sample and
	channel). The decoder's output passes through a one-pole reconstruction
	filter, r = alpha r + ( 1 - alpha ) y, alpha = exp( -1 / tau_r ); alpha = 0
	is the bare staircase.

	**Every scan line is its own coder**, started from the blanking level
	(kStartLevel) with the minimum step and an alternating history: a real
	system would carry its state through a blanking interval of black and
	arrive at much the same place, and independent lines are what lets the
	GPU run them side by side. A sample is the box average of `pitch`
	consecutive pixels along the scan (line by line, or column by column).
	In Y+C mode the two chroma channels are coded at half the sample rate,
	one sample per two luma samples, as a real system would.

	**How the GPU runs a serial recurrence.** GLSL 4.10 has no image stores,
	so one invocation cannot write a whole line. The line is cut into chunks
	of kChunk samples and one draw per chunk runs every line's chunk in
	parallel: each fragment re-runs the recurrence from the chunk's first
	sample -- whose start state the previous draw wrote -- up to its own
	sample. That is exact, not a windowed approximation: the state at every
	chunk boundary is the serial one. The spec's windowed restart from a
	fixed state was tried on paper and rejected, because the idle pattern's
	PHASE (which of the two hunting values a flat field is on at a given
	sample) is a memory that no leak erases; see AGENTS.md.
*/
namespace slopefx::model
{

/// Negative-control hooks: a bitmask the shipped plugin always carries at 0.
/// Each one perturbs the PLUGIN's shaders, never the harness's expectation.
enum Perturb : int
{
	kPerturbNone           = 0,
	kPerturbRestart        = 1 << 0,///< every chunk restarts from the fixed initial state (the spec's windowed restart with K = 0)
	kPerturbFixedStep      = 1 << 1,///< the run detector removed: the step never grows (the spec's --adapt negative)
	kPerturbNoDecoderLeak  = 1 << 2,///< the decoder's integrator does not leak
	kPerturbLeakToBlack    = 1 << 3,///< the integrators leak toward black instead of mid-grey
	kPerturbStepBias       = 1 << 4,///< the integrators add 1.25 steps per bit
	kPerturbVerticalFlip   = 1 << 5,///< a vertical scan runs up the columns instead of down
	kPerturbHalfErrors     = 1 << 6,///< the bit error rate halved
	kPerturbChromaFullRate = 1 << 7,///< Y+C chroma coded at the full sample rate
};

/// Samples per chunk: one draw call per chunk, and each fragment re-runs at
/// most this many steps. Smaller means more draws; larger means more
/// re-running. 32 measured best of 16, 32 and 64 (see AGENTS.md).
constexpr int kChunk = 32;

/// Where every line's coder starts: the blanking level.
constexpr double kStartLevel = 0.0;

/// Where the leaky integrator rests: mid-grey, the bias point of an
/// AC-coupled stage.
constexpr double kRestLevel = 0.5;

/// The history register at the start of a line: alternating bits, so no
/// run is read from the empty register whatever the run length.
constexpr int kStartHistory = 0x5;

/// Rec. 601 luma and the analogue colour-difference scaling.
constexpr double kLumaR = 0.299;
constexpr double kLumaG = 0.587;
constexpr double kLumaB = 0.114;
constexpr double kCbScale = 1.772;///< B - Y spans this; Cb = 0.5 + ( B - Y ) / kCbScale
constexpr double kCrScale = 1.402;///< R - Y spans this; Cr = 0.5 + ( R - Y ) / kCrScale

enum Channels
{
	kLuma = 0,///< luma coded, the colour difference of the input carried through
	kRGB  = 1,///< three coders
	kYC   = 2,///< luma at the sample rate, Cb and Cr at half of it
	kChannelsCount
};
inline const char* const kChannelNames[ kChannelsCount ] = { "Luma", "RGB", "Y+C" };

enum Scan
{
	kHorizontal = 0,
	kVertical   = 1,
	kScanCount
};
inline const char* const kScanNames[ kScanCount ] = { "Horizontal", "Vertical" };

/// The run detector's window: J identical bits in a row.
constexpr int kRunLengthCount = 2;
inline const int kRunLengths[ kRunLengthCount ]               = { 3, 4 };
inline const char* const kRunLengthNames[ kRunLengthCount ] = { "3 bits", "4 bits" };

/// Pixels per sample, as a real integer parameter.
constexpr int kPitchMin = 1;
constexpr int kPitchMax = 16;

/// Coders per line for a Channels option.
inline int CoderCount( int channels )
{
	return channels == kLuma ? 1 : 3;
}

} // namespace slopefx::model
