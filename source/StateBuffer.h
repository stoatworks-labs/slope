#pragma once

#include <FFGLSDK.h>

namespace slopefx
{
/**
	A framebuffer with TWO colour attachments, for the coder pass.

	The coder's state at a sample is seven numbers -- the encoder's and the
	decoder's ( y, step, history ) and the reconstruction filter's output --
	and one RGBA texel holds four. The SDK's FFGLFBO has one attachment, so
	this is its own small class: two RGBA32F textures on one framebuffer
	with glDrawBuffers set for both, Nearest filtering and clamp-to-edge,
	reallocated only when the size changes and cleared when it is. The
	coder pass ping-pongs between two of these, chunk by chunk, because a
	draw may not read the texture it writes.

	Same rules as PassBuffer: Ensure() before anything binds a texture
	(it binds and unbinds textures itself), and Destroy() in DeInitGL.
*/
class StateBuffer
{
public:
	bool Ensure( GLsizei width, GLsizei height );
	void Destroy();

	GLuint FBO() const
	{
		return fbo;
	}
	/// The encoder's state, ( y, step, history, received bit ).
	GLuint E() const
	{
		return textures[ 0 ];
	}
	/// The decoder's state, ( y, step, history, reconstruction ).
	GLuint D() const
	{
		return textures[ 1 ];
	}
	GLsizei Width() const
	{
		return width;
	}
	GLsizei Height() const
	{
		return height;
	}

private:
	GLuint fbo           = 0;
	GLuint textures[ 2 ] = { 0, 0 };
	GLsizei width        = 0;
	GLsizei height       = 0;
};

} // namespace slopefx
