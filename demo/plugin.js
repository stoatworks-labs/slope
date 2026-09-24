/**
 * Slope — browser demo.
 *
 * A 1-bit delta modulator (CVSD) run along the scan line. The one idea, from
 * `source/Slope.h`: every sample sends one bit — is the picture above or below
 * the decoder's guess? — and the guess can only climb one step a sample, so a
 * hard edge arrives as a ramp smeared along the scan, a flat area carries a
 * two-sample dither, an adaptive step trades one for the other, and a wrong
 * bit throws the rest of the line and fades on the leak.
 *
 * Unlike galvo and clamp, this plugin has **no CPU half worth the name**. The
 * coder IS the shader: the recurrence runs in GLSL, one draw per chunk of 32
 * samples, and the C++ only converts sliders to uniforms (`Controls.cpp`) and
 * schedules the draws (`Slope::ProcessOpenGL`). So this page is:
 *
 *   The plugin's shaders, unedited. `VERTEX`, `SAMPLE`, `CODER` and `DISPLAY`
 *   below are `kVertex`, `kSample`, `kCoder` and `kDisplay` from
 *   `source/Shaders.cpp`, copied across character for character.
 *   `demo/tools/check_shaders.py` compares them and `tools/verify.sh` runs it.
 *
 *   A port of the draw schedule and the control laws. `Controls.cpp` is ported
 *   function for function below; the sample / coder / display sequence, the
 *   chunked viewport and the ping-pong between two state buffers follow
 *   `Slope::ProcessOpenGL` in its order; `StateBuffer` mirrors
 *   `StateBuffer.cpp`. That much is a port, and nothing checks a port but a
 *   reader. `sltest` checks the C++ and the GLSL and has no idea this page
 *   exists.
 *
 * ------------------------------------------------------- the two attachments
 *
 * The coder's state at a sample is seven numbers and one texel holds four, so
 * the plugin writes two RGBA32F colour attachments from one draw
 * (`layout( location = 0 ) out outE; layout( location = 1 ) out outD`). That is
 * core WebGL2 — `gl.drawBuffers` — and RGBA32F render targets need
 * EXT_color_buffer_float, which `needFloat` asks the kit for. The kit's
 * PassBuffer has one attachment, so the state buffer is written out here.
 *
 * ------------------------------------------------------- the frame counter
 *
 * The plugin keeps no clock (`SetTimeSupported( false )`); the only thing that
 * carries across frames is a counter that seeds the bit errors, incremented on
 * every render. The page does the same, with its own counter incremented on
 * every render call, so — as in a host — a re-render of the same picture meets
 * fresh noise. Pause holds the last frame; Step renders one more.
 *
 * ------------------------------------------------------- what is missing
 *
 * **Pixels/Sample is a dropdown.** It is FF_TYPE_INTEGER in the plugin, 1 to 16,
 * and the kit has no integer control, so it is a dropdown of those sixteen
 * values and the element index converts (`pitchValue`). **Nothing audio.**
 * Slope has no audio path. **The About block is absent**, as on every page in
 * this suite. The harness-only uniforms — Perturb, ForcedLine, ForcedSample,
 * ForcedCoder — are set to what the shipped plugin sets them to: 0 and -1.
 *
 * And what every page in this suite is not: this is the plugin's shaders and a
 * port of its scheduling, not the plugin. No Resolume, no composition, no
 * FFGL, and GLSL ES 3.00 in a browser rather than desktop GL 4.1 core.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, GLError, bindTexture } from './vendor/gl.js';

//---------------------------------------------------------------------------
// Shaders — verbatim from source/Shaders.cpp. Do not edit here.
//---------------------------------------------------------------------------

const VERTEX = `#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
`;

const SAMPLE = `#version 410 core

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
`;

const CODER = `#version 410 core

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

	float ye = kStartLevel, se = StepMin, yd = kStartLevel, sd = StepMin, r = kStartLevel;
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
`;

const DISPLAY = `#version 410 core

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
`;

//===========================================================================
// Model.h, the constants the C++ uses.
//===========================================================================

/// Samples per chunk: one draw call per chunk, and each fragment re-runs at
/// most this many steps. 32 measured best of 16, 32 and 64 (AGENTS.md).
const K_CHUNK = 32;

const K_CHANNEL_NAMES = ['Luma', 'RGB', 'Y+C'];
const K_SCAN_NAMES = ['Horizontal', 'Vertical'];
const K_RUN_LENGTHS = [3, 4];
const K_RUN_LENGTH_NAMES = ['3 bits', '4 bits'];
const K_PITCH_MIN = 1;
const K_PITCH_MAX = 16;

/// Coders per line for a Channels option.
const coderCount = (channels) => (channels === 0 ? 1 : 3);

//===========================================================================
// Controls.cpp, ported. Every conversion from the host's 0..1 to a physical
// unit lives there and nowhere else, so it lives here and nowhere else too.
// The powers of two are deliberate: a slider at 0, 0.5 or 1 gives a step
// that is exact in float.
//===========================================================================

const unit = (v) => Math.min(1, Math.max(0, Number(v)));

/// 2^e, exact for an integral e (ldexp in the C++; Math.pow( 2, integer ) is
/// exact too), so the dyadic settings are dyadic in fact.
function pow2(e) {
  const whole = Math.floor(e);
  return Math.pow(2, e - whole) * Math.pow(2, whole);
}

/// An option's stored value, as an index into its `count` elements.
const optionIndex = (value, count) => Math.min(count - 1, Math.max(0, Math.round(value)));

/// Step Size: the minimum step, 2^( -8 + 4 v ): one 8-bit code at 0, 1/64 at
/// the default 0.5, sixteen codes at 1.
const stepMin = (v) => pow2(-8.0 + 4.0 * unit(v));

/// Max Step: 2^( -6 + 6 v ), 1/64 to the whole range, 1/8 at the default 0.5
/// -- and never below the minimum step.
const stepMax = (v, minimum) => Math.max(minimum, pow2(-6.0 + 6.0 * unit(v)));

/// Adaptation Rate: what one run adds to the step, v x Max Step / 8.
const stepAdd = (v, maximum) => (unit(v) * maximum) / 8.0;

/// Run Length: the option's element as J, 3 or 4 bits.
const runLength = (v) => K_RUN_LENGTHS[optionIndex(v, K_RUN_LENGTHS.length)];

/// Leak: the integrator's time constant in samples. 0 is no leak; otherwise
/// 4 x 2^( 10 ( 1 - v ) ), 4096 just above 0 down to 4 at 1; 256 at 0.4.
function integratorTau(value) {
  const v = unit(value);
  return v <= 0.0 ? 0.0 : 4.0 * pow2(10.0 * (1.0 - v));
}

/// The syllabic filter's time constant is a quarter of the integrator's.
const syllabicTau = (v) => integratorTau(v) / 4.0;

/// The leak coefficients, exp( -1 / tau ), exactly 1 for no leak.
function integratorLeak(v) {
  const tau = integratorTau(v);
  return tau <= 0.0 ? 1.0 : Math.exp(-1.0 / tau);
}
function syllabicLeak(v) {
  const tau = syllabicTau(v);
  return tau <= 0.0 ? 1.0 : Math.exp(-1.0 / tau);
}

/// Pixels/Sample: the integer, clamped to its range.
const pitch = (value) => Math.min(K_PITCH_MAX, Math.max(K_PITCH_MIN, Math.round(value)));

/// Bit Errors: the probability a received bit is flipped. 0 at 0, else
/// 10^( -5 + 4 v ): one in a hundred thousand just above 0, one in ten at 1.
function errorRate(value) {
  const v = unit(value);
  return v <= 0.0 ? 0.0 : Math.pow(10.0, -5.0 + 4.0 * v);
}

/// The error rate as the 32-bit hash threshold the shader compares against.
/// Rounded, in double, as llround does.
const errorThreshold = (rate) => Math.round(Math.min(0.5, Math.max(0.0, rate)) * 4294967296.0) >>> 0;

/// Reconstruction: the decoder's low-pass, as a time constant in samples,
/// 2^( 6 ( 1 - v ) ) - 1: 63 samples at 0, none at 1, 1.83 at the default.
const reconstructionTau = (v) => pow2(6.0 * (1.0 - unit(v))) - 1.0;

/// Its coefficient exp( -1 / tau_r ), exactly 0 for no filter.
function reconstructionAlpha(v) {
  const tau = reconstructionTau(v);
  return tau <= 0.0 ? 0.0 : Math.exp(-1.0 / tau);
}

//===========================================================================
// StateBuffer.cpp, ported: a framebuffer with TWO RGBA32F colour attachments,
// Nearest and clamp-to-edge, reallocated only when the size changes and
// cleared when it is -- a state buffer whose contents are undefined is
// whatever the driver handed back, and the first chunk of the first frame
// would start from it.
//===========================================================================

class StateBuffer {
  constructor(gl) {
    this.gl = gl;
    this.fbo = null;
    this.textures = [null, null];
    this.width = 0;
    this.height = 0;
  }

  ensure(width, height) {
    const gl = this.gl;
    if (width <= 0 || height <= 0) throw new GLError(`state buffer asked for ${width}x${height}`);
    if (this.fbo && this.width === width && this.height === height) return this;

    this.dispose();

    for (let i = 0; i < 2; i += 1) {
      const texture = gl.createTexture();
      gl.bindTexture(gl.TEXTURE_2D, texture);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA32F, width, height, 0, gl.RGBA, gl.FLOAT, null);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
      this.textures[i] = texture;
    }
    gl.bindTexture(gl.TEXTURE_2D, null);

    this.fbo = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.fbo);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, this.textures[0], 0);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT1, gl.TEXTURE_2D, this.textures[1], 0);
    gl.drawBuffers([gl.COLOR_ATTACHMENT0, gl.COLOR_ATTACHMENT1]);
    const status = gl.checkFramebufferStatus(gl.FRAMEBUFFER);
    if (status === gl.FRAMEBUFFER_COMPLETE) {
      gl.viewport(0, 0, width, height);
      gl.clearColor(0, 0, 0, 0);
      gl.clear(gl.COLOR_BUFFER_BIT);
    }
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);

    if (status !== gl.FRAMEBUFFER_COMPLETE) {
      this.dispose();
      throw new GLError(`state buffer incomplete (0x${status.toString(16)}) at ${width}x${height}: two RGBA32F attachments on one framebuffer`);
    }
    this.width = width;
    this.height = height;
    return this;
  }

  /// The encoder's state, ( y, step, history, received bit ).
  get E() { return this.textures[0]; }
  /// The decoder's state, ( y, step, history, reconstruction ).
  get D() { return this.textures[1]; }

  dispose() {
    const gl = this.gl;
    if (this.fbo) gl.deleteFramebuffer(this.fbo);
    for (const t of this.textures) if (t) gl.deleteTexture(t);
    this.fbo = null;
    this.textures = [null, null];
    this.width = 0;
    this.height = 0;
  }
}

//===========================================================================
// The renderer: Slope::ProcessOpenGL, in its order.
//
//   1. sample    samples x lines, RGBA32F: each coder's signal
//   2. coder     one draw per chunk of K_CHUNK samples, viewport offset so
//                gl_FragCoord.x IS the sample index, ping-ponging between two
//                state buffers
//   3. display   onto the canvas
//===========================================================================

/// What the line under the canvas reports. Filled by the renderer.
const telemetry = { samples: 0, lines: 0, coders: 0, chunks: 0, seed: 0 };

function createRenderer(gl, quad) {
  const sampleShader = new Program(gl, VERTEX, SAMPLE, 'sample');
  const coderShader = new Program(gl, VERTEX, CODER, 'coder');
  const displayShader = new Program(gl, VERTEX, DISPLAY, 'display');

  const samples = new PassBuffer(gl, { filter: 'nearest' });
  const states = [new StateBuffer(gl), new StateBuffer(gl)];

  // The frame counter: seeds the bit errors, the only state across frames.
  let frameIndex = 0;

  // Harness-only hooks, at what the shipped plugin carries.
  const PERTURB = 0;
  const FORCED_LINE = -1;
  const FORCED_SAMPLE = -1;
  const FORCED_CODER = -1;

  return {
    render({ input, params, width: vpW, height: vpH }) {
      const p = (id) => params.get(id);
      const picture = input;
      const width = picture.width;
      const height = picture.height;
      const seed = frameIndex;
      frameIndex = (frameIndex + 1) >>> 0;

      //------------------------------------------------------------------
      // The settings, in physical units.
      //------------------------------------------------------------------
      const minimum = stepMin(p('stepMin'));
      const maximum = stepMax(p('stepMax'), minimum);
      const add = stepAdd(p('adapt'), maximum);
      const J = runLength(p('runLength'));
      const leakI = integratorLeak(p('leak'));
      const leakS = syllabicLeak(p('leak'));
      const pitchPx = pitch(pitchValue(p('pitch')));
      const scan = optionIndex(p('scan'), K_SCAN_NAMES.length);
      const channels = optionIndex(p('channels'), K_CHANNEL_NAMES.length);
      const errors = errorRate(p('errors'));
      const alpha = reconstructionAlpha(p('recon'));
      const showBits = p('showBits') >= 0.5;
      const vertical = scan === 1;
      const coders = coderCount(channels);

      //------------------------------------------------------------------
      // Geometry: lines run along the scan; a sample is `pitch` pixels of it.
      //------------------------------------------------------------------
      const scanLength = vertical ? height : width;
      const lines = vertical ? width : height;
      const sampleCount = Math.floor((scanLength + pitchPx - 1) / pitchPx);
      const rows = lines * coders;
      const chunks = Math.floor((sampleCount + K_CHUNK - 1) / K_CHUNK);

      //------------------------------------------------------------------
      // Buffers. Every allocation happens here, before anything binds.
      //------------------------------------------------------------------
      samples.ensure(sampleCount, lines, gl.RGBA32F);
      states[0].ensure(sampleCount, rows);
      states[1].ensure(sampleCount, rows);

      gl.disable(gl.BLEND);

      //------------------------------------------------------------------
      // 1. The samples.
      //------------------------------------------------------------------
      samples.bind();
      sampleShader.use();
      bindTexture(gl, 0, picture.texture);
      sampleShader.setSampler('InputTexture', 0);
      sampleShader.setInt('InW', width);
      sampleShader.setInt('InH', height);
      sampleShader.setInt('Vertical', vertical ? 1 : 0);
      sampleShader.setInt('Pitch', pitchPx);
      sampleShader.setInt('Channels', channels);
      sampleShader.setInt('Perturb', PERTURB);
      quad.draw();

      //------------------------------------------------------------------
      // 2. The coder, one draw per chunk. Chunk p writes pair p & 1 and reads
      //    the state at its first sample - 1 from the other pair, which the
      //    previous chunk wrote. The viewport is offset to the chunk, so
      //    gl_FragCoord.x is the sample index and only the chunk's columns
      //    are rasterised.
      //------------------------------------------------------------------
      coderShader.use();
      coderShader.setSampler('Samples', 0);
      coderShader.setSampler('PrevE', 1);
      coderShader.setSampler('PrevD', 2);
      coderShader.setInt('Coders', coders);
      coderShader.setInt('Channels', channels);
      coderShader.set('StepMin', minimum);
      coderShader.set('StepMax', maximum);
      coderShader.set('StepAdd', add);
      coderShader.set('LeakI', leakI);
      coderShader.set('LeakS', leakS);
      coderShader.set('Alpha', alpha);
      coderShader.setInt('RunMask', (1 << J) - 1);
      coderShader.setUint('ErrThreshold', errorThreshold(errors));
      coderShader.setUint('FrameSeed', seed);
      coderShader.setInt('ForcedLine', FORCED_LINE);
      coderShader.setInt('ForcedSample', FORCED_SAMPLE);
      coderShader.setInt('ForcedCoder', FORCED_CODER);
      coderShader.setInt('Perturb', PERTURB);

      bindTexture(gl, 0, samples.texture);
      for (let c = 0; c < chunks; c += 1) {
        const write = states[c & 1];
        const read = states[(c + 1) & 1];
        const start = c * K_CHUNK;
        gl.bindFramebuffer(gl.FRAMEBUFFER, write.fbo);
        gl.viewport(start, 0, Math.min(K_CHUNK, sampleCount - start), rows);
        bindTexture(gl, 1, read.E);
        bindTexture(gl, 2, read.D);
        coderShader.setInt('ChunkStart', start);
        quad.draw();
      }
      bindTexture(gl, 2, null);
      bindTexture(gl, 1, null);
      bindTexture(gl, 0, null);

      //------------------------------------------------------------------
      // 3. Display, onto the canvas. The host's viewport is the whole canvas.
      //------------------------------------------------------------------
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, vpW, vpH);

      displayShader.use();
      bindTexture(gl, 0, picture.texture);
      bindTexture(gl, 1, states[0].E);
      bindTexture(gl, 2, states[0].D);
      bindTexture(gl, 3, states[1].E);
      bindTexture(gl, 4, states[1].D);
      displayShader.setSampler('InputTexture', 0);
      displayShader.setSampler('StateE0', 1);
      displayShader.setSampler('StateD0', 2);
      displayShader.setSampler('StateE1', 3);
      displayShader.setSampler('StateD1', 4);
      displayShader.setInt('InW', width);
      displayShader.setInt('InH', height);
      displayShader.setInt('Vertical', vertical ? 1 : 0);
      displayShader.setInt('Pitch', pitchPx);
      displayShader.setInt('Channels', channels);
      displayShader.setInt('Coders', coders);
      displayShader.setInt('Chunk', K_CHUNK);
      displayShader.setInt('VpX', 0);
      displayShader.setInt('VpY', 0);
      displayShader.setInt('VpW', vpW);
      displayShader.setInt('VpH', vpH);
      displayShader.setInt('ShowBits', showBits ? 1 : 0);
      displayShader.set('MixAmount', p('mix'));
      quad.draw();

      // Leave nothing bound that a framebuffer will be written through next
      // frame; the kit's source pass only uses unit 0.
      for (let u = 4; u >= 1; u -= 1) bindTexture(gl, u, null);
      gl.activeTexture(gl.TEXTURE0);

      telemetry.samples = sampleCount;
      telemetry.lines = lines;
      telemetry.coders = coders;
      telemetry.chunks = chunks;
      telemetry.seed = seed;
    },
  };
}

//===========================================================================
// The controls, read out of Slope::Slope(). Same names, same groups, same
// order, same defaults, same dropdown elements. Absent: the About block.
//===========================================================================

/// Pixels/Sample is FF_TYPE_INTEGER, 1..16, default 2: exempt from the 0..1
/// clamp, so the plugin stores the integer itself. The kit has no integer
/// control, so it is a dropdown of its own sixteen values and the index
/// converts.
const PITCH_ELEMENTS = [];
for (let v = K_PITCH_MIN; v <= K_PITCH_MAX; v += 1) PITCH_ELEMENTS.push(String(v));
function pitchValue(index) {
  return K_PITCH_MIN + optionIndex(index, PITCH_ELEMENTS.length);
}
const pitchIndex = (value) => value - K_PITCH_MIN;

const std = (id, name, def, group, extra = {}) => ({ id, name, type: 'standard', default: def, group, ...extra });
const opt = (id, name, elements, def, group, hint) => ({ id, name, type: 'option', elements, default: def, group, hint });
const bool = (id, name, def, group, hint) => ({ id, name, type: 'boolean', default: def, group, hint });

/// A step as the plugin's own dyadic fractions read: 1/64, or 3 significant
/// figures when it is not one over an integer.
function stepText(step) {
  const inverse = 1 / step;
  if (Math.abs(inverse - Math.round(inverse)) < 1e-9 && inverse >= 1) return `1/${Math.round(inverse)}`;
  return step.toPrecision(3);
}
const samplesText = (tau) => `${tau < 10 ? tau.toPrecision(3) : Math.round(tau)} samples`;

const demo = mountDemo({
  name: 'Slope',
  pluginId: 'SL01',
  tagline:
    'A one-bit delta modulator run along the scan line. Every sample sends one bit — is the picture above or below the decoder’s guess? — and the guess can only climb one step a sample, so hard edges arrive as ramps smeared along the scan, flat areas carry a fine two-sample dither, an adaptive step (CVSD) trades one for the other and no setting removes both, and a wrong bit throws the rest of the line and fades on the leak. The coder here is the plugin’s own shader, run in the plugin’s own chunks.',
  repo: 'https://github.com/stoatworks-labs/slope',

  // The stock sentence is right for this plugin: the coder is the shader, and
  // the shader is the plugin's. What is a port is said in the disclosure.
  kind: 'effect',

  // The samples and both state buffers are RGBA32F, as in the plugin: the
  // history register and the received bit ride in float channels, and a step
  // of 1/256 quantised to 8 bits would be one code wide.
  needFloat: true,

  params: [
    std('stepMin', 'Step Size', 0.5, 'Coder', {
      display: (v) => `Δmin ${stepText(stepMin(v))}`,
      hint: 'The smallest step, 2^(-8 + 4v): one 8-bit code at 0, 1/64 at the default, sixteen codes at 1. Every line starts at it.',
    }),
    std('stepMax', 'Max Step', 0.5, 'Coder', {
      display: (v) => `Δmax ${stepText(pow2(-6.0 + 6.0 * unit(v)))}`,
      hint: 'How far adaptation may grow the step, 2^(-6 + 6v): 1/64 at 0, 1/8 at the default, the whole range at 1. Never below Step Size: the readout here is the law alone, the coder takes the larger of the two.',
    }),
    std('adapt', 'Adaptation Rate', 0.5, 'Coder', {
      display: (v) => (unit(v) <= 0 ? 'none (fixed step)' : `Δmax / ${Number((8 / unit(v)).toPrecision(3))} per run`),
      hint: 'What a run of identical bits adds to the step: v × Max Step / 8. Zero is no adaptation at all, a plain delta modulator with a fixed step.',
    }),
    opt('runLength', 'Run Length', K_RUN_LENGTH_NAMES, 0, 'Coder',
      'The run detector’s window: this many identical bits in a row means the guess is falling behind and the step grows.'),
    std('leak', 'Leak', 0.4, 'Coder', {
      display: (v) => (integratorTau(v) <= 0 ? 'none' : `τ ${samplesText(integratorTau(v))}`),
      hint: 'The integrator’s time constant in samples, 4 × 2^(10(1 − v)): 4096 just above 0 down to 4 at 1, 256 at the default; exactly no leak at 0. The syllabic filter’s is a quarter of it. Both leak toward mid-grey.',
    }),

    opt('pitch', 'Pixels/Sample', PITCH_ELEMENTS, pitchIndex(2), 'Sampling',
      'Pixels box-averaged into one sample along the scan. FF_TYPE_INTEGER 1–16 in the plugin; a dropdown of the same sixteen values here.'),
    opt('scan', 'Scan', K_SCAN_NAMES, 0, 'Sampling',
      'Horizontal runs a coder along every row, left to right; Vertical runs one down every column.'),
    opt('channels', 'Channels', K_CHANNEL_NAMES, 0, 'Sampling',
      'Luma codes one signal and carries the input’s colour difference through uncoded; RGB codes three; Y+C codes luma at the sample rate and Cb, Cr at half of it.'),

    std('errors', 'Bit Errors', 0, 'Channel', {
      display: (v) => (errorRate(v) <= 0 ? 'none' : `1 in ${Math.round(1 / errorRate(v)).toLocaleString('en-GB')}`),
      hint: 'The probability a received bit is flipped: 0 at 0, else 10^(-5 + 4v), one in a hundred thousand to one in ten. Each wrong bit throws the decoder’s guess for the rest of the line, fading on the leak.',
    }),

    std('recon', 'Reconstruction', 0.75, 'Decoder', {
      display: (v) => (reconstructionTau(v) <= 0 ? 'none (bare staircase)' : `τ ${samplesText(reconstructionTau(v))}`),
      hint: 'The decoder’s one-pole low-pass, as a time constant in samples: 2^(6(1 − v)) − 1, 63 samples at 0, 1.83 at the default, none at 1.',
    }),
    bool('showBits', 'Show Bits', 0, 'Decoder',
      'Draw the received bitstream instead of the decoded picture, one primary per coder.'),

    std('mix', 'Mix', 1.0, 'Output'),
  ],

  // Hard edges are the effect, so the clips with edges lead. The scene moves.
  sources: ['bars', 'scene', 'ramp', 'grid', 'detail', 'spot'],

  // The plugin ships no factory presets. These are the page's own, expressed
  // entirely in the plugin's parameters and reachable with the controls.
  presets: {
    'Streaks (bit errors)': { errors: 0.5 },
    'Heavy overload': { pitch: pitchIndex(6), stepMin: 0.25, adapt: 0 },
    'Fixed step, no leak': { adapt: 0, leak: 0 },
    'Bits (RGB)': { showBits: 1, channels: 1 },
    'Y+C, vertical scan': { channels: 2, scan: 1 },
    'Bare staircase': { recon: 1, pitch: pitchIndex(4) },
  },

  differences: [
    'The shaders are the plugin’s. Sample, coder and display are the plugin’s own GLSL, and demo/tools/check_shaders.py fails the repository’s verify script if a character of any of the four shaders drifts. The recurrence you are watching is the one the plugin runs.',
    'The scheduling is a PORT. Slope::ProcessOpenGL’s draw order — the sample pass, one coder draw per chunk of 32 samples with the viewport offset so gl_FragCoord.x is the sample index, ping-ponging between two state buffers, then the display — is followed here in JavaScript, and StateBuffer.cpp (one framebuffer, two RGBA32F attachments, cleared on allocation) is written out again in WebGL2. Controls.cpp’s laws are ported function for function. Nothing checks a port but a reader; sltest checks the C++ and has never heard of this page.',
    'Pixels/Sample is FF_TYPE_INTEGER in the plugin, 1 to 16, with a real range. The demo kit has no integer control, so it is a dropdown of the same sixteen values.',
    'The Max Step readout shows its law, 2^(-6 + 6v), alone; the coder takes the larger of that and Step Size, as the plugin does. Where Step Size is above it the number beside the slider is not the step in use.',
    'The bit errors are seeded from a frame counter, as the plugin seeds them: incremented on every render, so a re-render of a paused frame — a parameter moved while paused — meets fresh noise, as it would in a host. The plugin has no clock and neither does this page’s coder.',
    'The harness-only uniforms — Perturb, ForcedLine, ForcedSample, ForcedCoder — are set to what the shipped plugin sets them to, 0 and -1. The negative controls and the forced error that sltest drives through them are not on this page.',
    'There is no audio caveat on this page: Slope has no audio path. The About block is absent, as on every page in this suite.',
    'The plugin’s numerical proof — a step of height h climbing in exactly ⌈h/Δ⌉ samples, the idle pattern’s period and amplitude, the syllabic law, the leak fitted to 64 and 256 samples, every pixel against a serial double run with a tolerance of zero — is an offline harness in the repository. Nothing on this page measures anything.',
  ],

  createRenderer,
});

//---------------------------------------------------------------------------
// The line under the canvas: the geometry the port chose from the raster and
// the settings, and how many coder draws that is. Skipped in embed mode.
//---------------------------------------------------------------------------
if (demo && !new URLSearchParams(window.location.search).has('embed')) {
  const stage = document.querySelector('.stage');
  if (stage) {
    const line = document.createElement('p');
    line.className = 'stage__status';
    stage.append(line);
    setInterval(() => {
      const { samples, lines, coders, chunks, seed } = telemetry;
      if (!samples) return;
      line.textContent =
        `${samples.toLocaleString('en-GB')} samples per line, ${lines.toLocaleString('en-GB')} lines × ${coders} coder${coders === 1 ? '' : 's'}: `
        + `${chunks} coder draws of ${K_CHUNK} samples this frame, bit errors seeded from frame ${seed.toLocaleString('en-GB')}.`;
    }, 250);
  }
}
