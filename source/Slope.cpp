#include "Slope.h"

#include "Controls.h"
#include "Diag.h"
#include "Model.h"
#include "Shaders.h"

//FFGLSDK.h includes every other scoped binding and omits this one (SDK
//b1afaf9). The symptom without it is an unknown-type error on
//ScopedFBOBinding and nothing else.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <cmath>
#include <string>

using namespace ffglex;
using namespace slopefx;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Slope >,// Create method
	"SL01",                // Plugin unique ID of maximum length 4.
	"SW Slope",            // Plugin name
	2,                     // API major version number
	1,                     // API minor version number
	0,                     // Plugin major version number
	1,                     // Plugin minor version number
	FF_EFFECT,             // Plugin type
	"A 1-bit delta modulator run along the scan line.\n\nEvery sample sends one bit: is the picture above or below the decoder's guess? The guess can only climb one step a sample, so hard edges arrive as ramps smeared along the scan; flat areas carry a fine two-sample dither; an adaptive step (CVSD) trades one for the other and no setting removes both; a bit error throws the rest of the line and fades on the leak.\n\nStart with Pixels/Sample and Step Size; push Bit Errors for streaks.",// Plugin description
	"Slope FFGL effect"    // About
);

namespace
{
/// glGetString returns nullptr with no current context; a log line must never
/// be the thing that brings the host down.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

void setUint( GLuint program, const char* name, GLuint value )
{
	//FFGLShader::Set has no unsigned overload. The program is bound by the
	//caller's ScopedShaderBinding.
	glUniform1ui( glGetUniformLocation( program, name ), value );
}
} // namespace

//---------------------------------------------------------------------------
Slope::Slope()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//No clock: nothing here moves with time. The bit errors are seeded from
	//a frame counter, which is what a channel does -- every transmitted
	//frame meets fresh noise -- and is the same in the harness as in a host.
	SetTimeSupported( false );

	//---------------------------------------------------------------------
	// Defaults: a 1/64 step adapting up to 1/8, a syllabic filter that lets
	// it fall back within a line, two pixels a sample, luma only, a light
	// reconstruction filter, no errors.
	//
	// Filled BEFORE any declaration: SetParamInfof reads its default out of
	// GetFloatParameter (compander's trap).
	//---------------------------------------------------------------------
	params[ PT_STEP_MIN ]   = 0.5f;//1/64
	params[ PT_STEP_MAX ]   = 0.5f;//1/8
	params[ PT_ADAPT ]      = 0.5f;//Max Step / 16 per run
	params[ PT_RUN_LENGTH ] = 0.0f;//3 bits
	params[ PT_LEAK ]       = 0.4f;//256 samples
	params[ PT_PITCH ]      = 2.0f;
	params[ PT_SCAN ]       = static_cast< float >( model::kHorizontal );
	params[ PT_CHANNELS ]   = static_cast< float >( model::kLuma );
	params[ PT_ERRORS ]     = 0.0f;
	params[ PT_RECON ]      = 0.75f;//1.83 samples
	params[ PT_SHOW_BITS ]  = 0.0f;
	params[ PT_MIX ]        = 1.0f;

	SetParamInfof( PT_STEP_MIN, "Step Size", FF_TYPE_STANDARD );
	SetParamInfof( PT_STEP_MAX, "Max Step", FF_TYPE_STANDARD );
	SetParamInfof( PT_ADAPT, "Adaptation Rate", FF_TYPE_STANDARD );
	SetOptionParamInfo( PT_RUN_LENGTH, "Run Length", model::kRunLengthCount, params[ PT_RUN_LENGTH ] );
	for( int i = 0; i < model::kRunLengthCount; ++i )
		SetParamElementInfo( PT_RUN_LENGTH, static_cast< unsigned int >( i ), model::kRunLengthNames[ i ], static_cast< float >( i ) );
	SetParamInfof( PT_LEAK, "Leak", FF_TYPE_STANDARD );

	//A real integer with a real range: FF_TYPE_INTEGER is exempt from the
	//0..1 clamp of a STANDARD default. "Pixels per Sample" is 17 characters,
	//one over what a host shows.
	SetParamInfo( PT_PITCH, "Pixels/Sample", FF_TYPE_INTEGER, params[ PT_PITCH ] );
	SetParamRange( PT_PITCH, static_cast< float >( model::kPitchMin ), static_cast< float >( model::kPitchMax ) );
	SetOptionParamInfo( PT_SCAN, "Scan", model::kScanCount, params[ PT_SCAN ] );
	for( int i = 0; i < model::kScanCount; ++i )
		SetParamElementInfo( PT_SCAN, static_cast< unsigned int >( i ), model::kScanNames[ i ], static_cast< float >( i ) );
	SetOptionParamInfo( PT_CHANNELS, "Channels", model::kChannelsCount, params[ PT_CHANNELS ] );
	for( int i = 0; i < model::kChannelsCount; ++i )
		SetParamElementInfo( PT_CHANNELS, static_cast< unsigned int >( i ), model::kChannelNames[ i ], static_cast< float >( i ) );

	SetParamInfof( PT_ERRORS, "Bit Errors", FF_TYPE_STANDARD );

	SetParamInfof( PT_RECON, "Reconstruction", FF_TYPE_STANDARD );
	SetParamInfo( PT_SHOW_BITS, "Show Bits", FF_TYPE_BOOLEAN, false );

	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	//SetParamGroup collapses consecutive ids under one header, so each group
	//is a contiguous run of the enum.
	for( FFUInt32 i = PT_STEP_MIN; i <= PT_LEAK; ++i )
		SetParamGroup( i, "Coder" );
	for( FFUInt32 i = PT_PITCH; i <= PT_CHANNELS; ++i )
		SetParamGroup( i, "Sampling" );
	SetParamGroup( PT_ERRORS, "Channel" );
	for( FFUInt32 i = PT_RECON; i <= PT_SHOW_BITS; ++i )
		SetParamGroup( i, "Decoder" );
	SetParamGroup( PT_MIX, "Output" );

	// The About block. Declared inline: SetParamInfo is protected on
	// CFFGLPlugin and nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	FFGLLog::LogToHost( "Created Slope effect" );
	diag::init();
}

//---------------------------------------------------------------------------
FFResult Slope::InitGL( const FFGLViewportStruct* vp )
{
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR ) + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	struct
	{
		FFGLShader* shader;
		const char* fragment;
		const char* name;
	} const stages[] = {
		{ &sampleShader, shaders::kSample, "sample" },
		{ &coderShader, shaders::kCoder, "coder" },
		{ &displayShader, shaders::kDisplay, "display" },
	};

	for( const auto& stage : stages )
	{
		if( stage.shader->Compile( shaders::kVertex, stage.fragment ) )
			continue;
		//FF_FAIL is invisible to an operator: the effect simply does nothing.
		//This line is the only record of which pass it was.
		diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Slope: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	frameIndex = 0;
	diag::info( "initialised" );
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
FFResult Slope::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& picture = *pGL->inputTextures[ 0 ];
	if( picture.Width == 0 || picture.Height == 0 )
		return FF_FAIL;

	//The host's viewport, before anything of ours changes it:
	//ScopedFBOBinding restores the framebuffer binding and only that.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	const int width  = static_cast< int >( picture.Width );
	const int height = static_cast< int >( picture.Height );
	const uint32_t seed = frameIndex++;

	//---------------------------------------------------------------------
	// The settings, in physical units.
	//---------------------------------------------------------------------
	const double stepMin  = controls::StepMin( params[ PT_STEP_MIN ] );
	const double stepMax  = controls::StepMax( params[ PT_STEP_MAX ], stepMin );
	const double stepAdd  = controls::StepAdd( params[ PT_ADAPT ], stepMax );
	const int runLength   = controls::RunLength( params[ PT_RUN_LENGTH ] );
	const double leakI    = controls::IntegratorLeak( params[ PT_LEAK ] );
	const double leakS    = controls::SyllabicLeak( params[ PT_LEAK ] );
	const int pitch       = controls::Pitch( params[ PT_PITCH ] );
	const int scan        = controls::OptionIndex( params[ PT_SCAN ], model::kScanCount );
	const int channels    = controls::OptionIndex( params[ PT_CHANNELS ], model::kChannelsCount );
	const double errors   = controls::ErrorRate( params[ PT_ERRORS ] );
	const double alpha    = controls::ReconstructionAlpha( params[ PT_RECON ] );
	const bool showBits   = params[ PT_SHOW_BITS ] >= 0.5f;
	const bool vertical   = scan == model::kVertical;
	const int coders      = model::CoderCount( channels );

	//---------------------------------------------------------------------
	// Geometry: lines run along the scan; a sample is `pitch` pixels of it.
	//---------------------------------------------------------------------
	const int scanLength = vertical ? height : width;
	const int lines      = vertical ? width : height;
	const int samples    = ( scanLength + pitch - 1 ) / pitch;
	const int rows       = lines * coders;
	const int chunks     = ( samples + model::kChunk - 1 ) / model::kChunk;

	//---------------------------------------------------------------------
	// Buffers. Every allocation happens here, before anything binds a
	// texture: FFGLFBO::Initialise sizes its colour texture under a scoped
	// binding, and every ffglex Scoped* binding CLEARS to 0 on exit.
	//---------------------------------------------------------------------
	if( !this->samples.Ensure( samples, lines, GL_RGBA32F, PassBuffer::Sampling::Nearest ) || !states[ 0 ].Ensure( samples, rows )
	    || !states[ 1 ].Ensure( samples, rows ) )
	{
		diag::error( "could not allocate the buffers: " + std::to_string( samples ) + " samples x " + std::to_string( rows ) + " rows" );
		return FF_FAIL;
	}

	//---------------------------------------------------------------------
	// 1. The samples.
	//---------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( this->samples.GetGLID(), ScopedFBOBinding::RB_REVERT );
		this->samples.ResizeViewPort();
		ScopedShaderBinding shader( sampleShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( picture.Handle );
		sampleShader.Set( "InputTexture", 0 );
		sampleShader.Set( "InW", width );
		sampleShader.Set( "InH", height );
		sampleShader.Set( "Vertical", vertical ? 1 : 0 );
		sampleShader.Set( "Pitch", pitch );
		sampleShader.Set( "Channels", channels );
		sampleShader.Set( "Perturb", perturb );
		quad.Draw();
	}

	//---------------------------------------------------------------------
	// 2. The coder, one draw per chunk. Chunk p writes pair p & 1 and reads
	//    the state at its first sample - 1 from the other pair, which the
	//    previous chunk wrote. Raw binds: nothing scoped survives a loop of
	//    draws, and the display rebinds everything it needs.
	//---------------------------------------------------------------------
	{
		ScopedShaderBinding shader( coderShader.GetGLID() );
		const GLuint program = coderShader.GetGLID();
		coderShader.Set( "Samples", 0 );
		coderShader.Set( "PrevE", 1 );
		coderShader.Set( "PrevD", 2 );
		coderShader.Set( "Coders", coders );
		coderShader.Set( "Channels", channels );
		coderShader.Set( "StepMin", static_cast< float >( stepMin ) );
		coderShader.Set( "StepMax", static_cast< float >( stepMax ) );
		coderShader.Set( "StepAdd", static_cast< float >( stepAdd ) );
		coderShader.Set( "LeakI", static_cast< float >( leakI ) );
		coderShader.Set( "LeakS", static_cast< float >( leakS ) );
		coderShader.Set( "Alpha", static_cast< float >( alpha ) );
		coderShader.Set( "RunMask", ( 1 << runLength ) - 1 );
		setUint( program, "ErrThreshold", controls::ErrorThreshold( errors ) );
		setUint( program, "FrameSeed", seed );
		coderShader.Set( "ForcedLine", forcedLine );
		coderShader.Set( "ForcedSample", forcedSample );
		coderShader.Set( "ForcedCoder", forcedCoder );
		coderShader.Set( "Perturb", perturb );
		const GLint chunkStartLocation = glGetUniformLocation( program, "ChunkStart" );

		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, this->samples.TextureID() );
		for( int p = 0; p < chunks; ++p )
		{
			const StateBuffer& write = states[ p & 1 ];
			const StateBuffer& read  = states[ ( p + 1 ) & 1 ];
			const int start          = p * model::kChunk;
			glBindFramebuffer( GL_FRAMEBUFFER, write.FBO() );
			glViewport( start, 0, std::min( model::kChunk, samples - start ), rows );
			glActiveTexture( GL_TEXTURE1 );
			glBindTexture( GL_TEXTURE_2D, read.E() );
			glActiveTexture( GL_TEXTURE2 );
			glBindTexture( GL_TEXTURE_2D, read.D() );
			glUniform1i( chunkStartLocation, start );
			quad.Draw();
		}
		glActiveTexture( GL_TEXTURE2 );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	//---------------------------------------------------------------------
	// 3. Display.
	//---------------------------------------------------------------------
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( displayShader.GetGLID() );
		ScopedSamplerActivation s0( 0 );
		Scoped2DTextureBinding input( picture.Handle );
		ScopedSamplerActivation s1( 1 );
		Scoped2DTextureBinding e0( states[ 0 ].E() );
		ScopedSamplerActivation s2( 2 );
		Scoped2DTextureBinding d0( states[ 0 ].D() );
		ScopedSamplerActivation s3( 3 );
		Scoped2DTextureBinding e1( states[ 1 ].E() );
		ScopedSamplerActivation s4( 4 );
		Scoped2DTextureBinding d1( states[ 1 ].D() );

		displayShader.Set( "InputTexture", 0 );
		displayShader.Set( "StateE0", 1 );
		displayShader.Set( "StateD0", 2 );
		displayShader.Set( "StateE1", 3 );
		displayShader.Set( "StateD1", 4 );
		displayShader.Set( "InW", width );
		displayShader.Set( "InH", height );
		displayShader.Set( "Vertical", vertical ? 1 : 0 );
		displayShader.Set( "Pitch", pitch );
		displayShader.Set( "Channels", channels );
		displayShader.Set( "Coders", coders );
		displayShader.Set( "Chunk", model::kChunk );
		displayShader.Set( "VpX", hostViewport[ 0 ] );
		displayShader.Set( "VpY", hostViewport[ 1 ] );
		displayShader.Set( "VpW", hostViewport[ 2 ] );
		displayShader.Set( "VpH", hostViewport[ 3 ] );
		displayShader.Set( "ShowBits", showBits ? 1 : 0 );
		displayShader.Set( "MixAmount", params[ PT_MIX ] );
		quad.Draw();
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Slope::DeInitGL()
{
	sampleShader.FreeGLResources();
	coderShader.FreeGLResources();
	displayShader.FreeGLResources();
	quad.Release();

	samples.Destroy();
	states[ 0 ].Destroy();
	states[ 1 ].Destroy();
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Slope::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// The About buttons open a browser and store nothing.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	params[ index ] = value;
	return FF_SUCCESS;
}

float Slope::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;
	return params[ index ];
}

char* Slope::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}
	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Slope::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only; it has to say so
	// successfully.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;
	return CFFGLPlugin::SetTextParameter( index, value );
}
