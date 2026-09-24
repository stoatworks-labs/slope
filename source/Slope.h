#pragma once

#include "PassBuffer.h"
#include "StateBuffer.h"

#include <FFGLSDK.h>

#include <cstdint>
#include <string>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

/**
	Slope -- a 1-bit delta modulator run along the scan line, as an FFGL
	effect.

	**The one idea.** A delta modulator sends one bit per sample: is the
	input above or below my current guess? The decoder has only that guess,
	stepped up or down by one step per bit. CVSD adapts the step: a run of
	identical bits means the guess is falling behind, so the step grows;
	alternating bits mean it is hunting round a flat input, so the step
	shrinks. Run it along each scan line of the picture and every artefact
	falls out of that one rule: a hard edge arrives as a ramp smeared to
	the right (slope overload), flat areas carry a fine two-sample dither
	(granular noise), adaptation trades one for the other, and a bit error
	throws the guess for the rest of the line, decaying on the leak.

	**Three passes.** The sample pass box-averages the picture into one
	texel per sample; the coder pass runs the recurrence in chunks of
	kChunk samples, one draw per chunk, each fragment re-running its chunk
	from the previous chunk's end state -- serial in fact, parallel in
	execution; the display puts the decoded signal back onto the picture.
	Nothing carries across frames but a frame counter for the bit errors'
	seed, so a resize cannot lose anything. See AGENTS.md.
*/
class Slope : public CFFGLPlugin
{
public:
	Slope();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters
	/// and deletes the whole instance if one fails, and CFFGLPlugin's
	/// SetTextParameter is a stub that returns exactly that failure.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	//--- test hooks. Read by sltest; the plugin's own operation never uses
	//--- them, and the perturbation is always 0 outside the harness.

	/// Negative-control hooks, a bitmask of `model::Perturb`.
	void SetPerturbForTest( int bits )
	{
		perturb = bits;
	}

	/// One forced bit error at ( line, sample, coder ), instead of the hash;
	/// line -1 for none.
	void SetForcedErrorForTest( int line, int sample, int coder )
	{
		forcedLine   = line;
		forcedSample = sample;
		forcedCoder  = coder;
	}

	/// The frame counter the next ProcessOpenGL will seed the bit errors with.
	uint32_t FrameIndexForTest() const
	{
		return frameIndex;
	}

	/// Everything the operator can reach, in the order Resolume shows them.
	enum ParamID : FFUInt32
	{
		//Coder
		PT_STEP_MIN,
		PT_STEP_MAX,
		PT_ADAPT,
		PT_RUN_LENGTH,
		PT_LEAK,

		//Sampling
		PT_PITCH,
		PT_SCAN,
		PT_CHANNELS,

		//Channel
		PT_ERRORS,

		//Decoder
		PT_RECON,
		PT_SHOW_BITS,

		//Output
		PT_MIX,

		//About. FFGL has no window, so the name, the version and the links are
		//parameters the host draws. Last, so no saved composition's ids shift.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

private:
	ffglex::FFGLShader sampleShader;
	ffglex::FFGLShader coderShader;
	ffglex::FFGLShader displayShader;
	ffglex::FFGLScreenQuad quad;

	slopefx::PassBuffer samples;    ///< samples x lines: each coder's signal
	slopefx::StateBuffer states[ 2 ];///< samples x ( lines x coders ), ping-ponged per chunk

	uint32_t frameIndex = 0;///< seeds the bit errors; the only state across frames

	int perturb      = 0;
	int forcedLine   = -1;
	int forcedSample = -1;
	int forcedCoder  = -1;

	/// Zero-initialised: the About block's ids are never stored to.
	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
