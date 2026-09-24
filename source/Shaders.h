#pragma once

/**
	The passes. Every read is `texelFetch` at integer coordinates computed in
	integers, so nothing here depends on a texture unit's filtering or on
	where a rasteriser's interpolated uv lands. Every coefficient is computed
	on the CPU in double (`Controls.cpp`) and handed over as a float uniform;
	the GPU compares, multiplies and adds.

	Rows in the buffers this plugin owns are SCAN LINES, first line first:
	texel row l is line l (the picture's top row, or its left column under a
	vertical scan). Only the sample pass, which reads the host's picture,
	and the display, which writes it, know that GL's row 0 is the bottom.

	  sample   host picture -> one texel per sample of each line: the box
	                           average of Pitch pixels of each coder's
	                           signal (samples x lines)
	  coder    samples      -> the encoder's and the decoder's state at
	           + the state     every sample, run in chunks of kChunk: one
	             at the       draw per chunk, each fragment re-running the
	             chunk start   recurrence from the chunk's start. Two
	                           attachments: E = ( y_e, step_e, hist_e, bit )
	                           and D = ( y_d, step_d, hist_d, r ), rows =
	                           lines x coders, ping-ponged between chunks
	  display  host picture -> the decoded signal back to pixels, or the
	           + D + E         bitstream, and the mix
*/
namespace slopefx::shaders
{

extern const char* const kVertex;
extern const char* const kSample;
extern const char* const kCoder;
extern const char* const kDisplay;

} // namespace slopefx::shaders
