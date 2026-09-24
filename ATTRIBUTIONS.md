# Attributions

Slope is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### Harness shape, --pipe contract and verify — Stoatworks clamp

<https://github.com/stoatworks-labs/clamp>  
Licence: MIT  
Copyright: Stoatworks Labs

The harness shape, the --pipe contract (SIGPIPE ignored, a closed stdout exits 1), the negative-control pattern, --offline, check-shaders.sh, the verify script, the sweep and the CI workflows are clamp's, by way of standards.

### A serial recurrence on the GPU — Stoatworks compander

<https://github.com/stoatworks-labs/compander>  
Licence: MIT  
Copyright: Stoatworks Labs

The idea of computing a serial recurrence on the GPU and holding it to a serial double-precision one is compander's and clamp's; the chunked exact form in scan order is slope's own.

### PassBuffer — Stoatworks tinsel

<https://github.com/stoatworks-labs/tinsel>  
Licence: MIT  
Copyright: Stoatworks Labs

The off-screen buffer wrapper is tinsel's, by way of standards and clamp.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### Continuously variable slope delta modulation

The one-bit coder with a syllabic step adapter and leaky integrators, as described in the open literature and in the data sheets of the classic CVSD codec parts (a 3- or 4-bit coincidence detector driving a syllabic filter, with integrator and syllabic time constants stated in samples). Built from the description; no manufacturer's design, coefficients or name is used, and the time constants here are chosen for a picture, not for speech.

## Standards and published specifications

What the implementation is measured against.

- **ITU-R BT.601** — The luma weights (0.299, 0.587, 0.114) and the colour-difference scaling (1.772, 1.402) the Luma and Y+C modes use.
- **Nicholas J. Higham, Accuracy and Stability of Numerical Algorithms** — The rounding-error bounds the harness's running float tolerance is derived from.
- **Melissa E. O'Neill, "PCG: A Family of Simple Fast Space-Efficient Statistically Good Algorithms for Random Number Generation" (Harvey Mudd College, 2014)** — The pcg_hash output mix the channel's bit errors and the harness's noise frames use, written out rather than copied from anyone's source.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
