# slope

A 1-bit delta modulator (CVSD) run along the scan line, as an FFGL **effect**
for Resolume Arena/Avenue. C++/GLSL, CMake MODULE → universal `.bundle` (macOS) +
Windows `.dll`. MIT.

Read `AGENTS.md` before changing the recurrence (`Shaders.cpp`, the coder pass),
the chunking in `Slope.cpp`, the control laws or the harness's bounds.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Universal (what ships, and what `verify.sh` builds): `cmake -B build-universal -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build --parallel 4`
- Install into Arena: `cmake --install build` — **not run from a session**, it writes
  into `~/Documents/Resolume Arena/Extra Effects`
- Render a frame offline: `./build/sltest --out /tmp/f.png --size 1920x1080`
- Set anything by name: `--set "Step Size=0.25" --set "Channels=1" --set "Pixels/Sample=4"`
  (0..1 for sliders, the element index for options, the integer for Pixels/Sample)
- List parameters, kinds, defaults and ranges: `./build/sltest --list`
- The exact GLSL the plugin compiles: `./build/sltest --dump-shaders DIR`
- Footage through the real shaders — **`--pipe`**, raw RGBA frames in, raw RGBA frames
  out, with `--size WxH`, `--fps N` (accepted for the fleet's contract; this plugin has
  no clock, so it changes nothing) and an optional `--script` of
  `frame Parameter Name value` cues, linearly interpolated between a name's cues and
  held before the first and after the last — an option index interpolated passes
  through the options between, so key a cut two cues a frame apart. A cue naming no
  parameter exits 2 before any frame; a partial frame at the end of stdin ends the
  stream with exit 0; a failed render or a closed stdout exits 1 (SIGPIPE is
  ignored so a closed stdout is a failed write, not a 141):
  `ffmpeg … -f rawvideo -pix_fmt rgba - | ./build/sltest --pipe --size 1920x1080 [--script cues.txt] | ffmpeg …`

## Verify
- Everything: `tools/verify.sh` (fresh universal build + glslc + the offline checks +
  every rendered check at 320x180 AND 1280x720 + the --pipe contract + the sweep + the
  bundle, ~4 min)
- A step edge climbs in ⌈h/Δ⌉ samples, whole- and part-sample edges: `./build/sltest --overload`
- A flat field's idle pattern: period 2, one step peak-to-peak: `./build/sltest --granular`
- The step grows by the stated law per run; the climb shortens as predicted: `./build/sltest --adapt`
- Over Adaptation Rate the climb shortens and the post-edge energy rises: `./build/sltest --tradeoff`
- A forced bit error fades on the integrator's time constant: `./build/sltest --leak`
- Every pixel against a serial double run: `./build/sltest --reference`
- A vertical scan is the transposed horizontal one, exactly: `./build/sltest --vertical`
- Bits flipped at the stated rate, freshly each frame: `./build/sltest --errors`
- The checks can fail: `./build/sltest --negative`; one perturbation verbosely:
  `./build/sltest --adapt --perturb 2` (bits in `Model.h`)
- No GL (what CI runs first): `./build/sltest --offline` = `--laws --names`
- Every rendered check takes `--size WxH`; CI runs them at 320x180 with `--allow-no-gl`
- Shaders through glslc: `tools/check-shaders.sh build/sltest`
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`)
- Render cost: `./build/sltest --bench` (best of three; the GPU is shared, so run it twice)
- What a host sees: `~/Projects/resolume/oxbow/build/oxbow probe build-universal/Slope.bundle`
- The demo's shaders are still the plugin's: `python3 demo/tools/check_shaders.py`

## Notes
- **The shader IS the coder.** The recurrence lives once, in GLSL (`Shaders.cpp`,
  the coder pass); the C++ converts sliders to its uniforms (`Controls.cpp`) and
  schedules the draws (`Slope.cpp`). The harness restates the recurrence in double
  from `Model.h`'s description and holds the shader to it.
- **One draw per chunk of 32 samples**, in scan order. Each fragment re-runs the
  recurrence from its chunk's first sample, whose state the previous draw wrote to
  the other buffer pair. Serial in fact, parallel in execution: the state at every
  chunk boundary is the serial one. Not a windowed approximation.
- **Every scan line is its own coder**, from the blanking level (0) with the minimum
  step. Nothing carries across frames except a frame counter for the bit errors'
  seed, so there is no resize check to run and no clock (`SetTimeSupported(false)`).
- **The integrators leak toward mid-grey**, the bias point of an AC-coupled stage;
  Leak at 0 is exactly no leak. The syllabic time constant is a quarter of the
  integrator's (AGENTS.md says why).
- **The comparator's tie is +** (`x >= y`). It matters: levels on the step grid tie
  every other sample, and the harness's exact checks depend on the rule.
- **A pixel shows the decoder's guess AFTER its own bit** (y[n+1]); the harness's
  first version of `--granular` compared against y[n] and was one step out everywhere.
- **The display clamps to 0..1**, so a check that lets the guess overshoot past white
  reads a truncated step. `--adapt` and `--tradeoff` refuse clipped readings.
- **Parameter names must be unique and 16 characters or under** — `Pixels per Sample`
  is 17, hence `Pixels/Sample`.
- `SetParamInfo` clamps a STANDARD default into 0..1; `SetParamInfof` reads its default
  out of `params[]`, so fill `params[]` first. Options are mapped by index in
  `Controls.cpp` (an option's range reads back 0..1); `Pixels/Sample` is a real
  `FF_TYPE_INTEGER` with `SetParamRange(1, 16)`.
- Override `SetTextParameter` to return FF_SUCCESS for the About block, or no host can
  instantiate the plugin at all.
- `slope_core` is an OBJECT library, not STATIC — the plugin registers itself from a
  file-scope constructor nothing references by name.
- `FFGLScopedFBOBinding.h` is not in the umbrella header; include `<ffglex/FFGLScopedFBOBinding.h>`.
- `FFGLShader::Set` has no unsigned overload: the hash seed and the error threshold go
  in with `glUniform1ui` under the pass's own shader binding.
- The coder pass needs two colour attachments (seven state values a sample); the
  SDK's FFGLFBO has one, so `StateBuffer` is its own small MRT class.
- macOS build must be universal. Verify with `lipo`, never the build log.
- FFGL id is `SL01`, display name `SW Slope`.

## Not done yet
- **Never loaded into Resolume on macOS.** Everything numeric is measured offline on
  macOS, plus an `oxbow` load. Footage only through `--pipe` (the release video).
- No OpenFX port, no factory presets, no audio input.
- **The browser demo's scheduling is a port**: `Controls.cpp`, `StateBuffer.cpp` and the draw order in `ProcessOpenGL` are a hand port in `demo/plugin.js`, and nothing checks it. Change any of those and change `demo/plugin.js` by hand. The shaders themselves are checked (`demo/tools/check_shaders.py`).
- `StoatworksAbout.h` and `ATTRIBUTIONS.md` are GENERATED by the backend's syncs
  (sync-about.py, sync-attributions.py); never edit them by hand.

## Browser demo

`demo/` is the page at **slope-demo.stoatworks-labs.com**, deployed from
`wrangler.toml` with `cf-run npx wrangler deploy` and by `deploy.yml` on a push to
main — no build step; what is committed is what is served. The host is a Worker
route plus a proxied `AAAA 100::` record (the zone is at its custom-domain limit).
`demo/vendor/` is copied in by
`~/Projects/infrastructure/stoatworks-backend/resolume-demo/sync.sh slope` and is not
a place to edit. The shaders in `demo/plugin.js` must stay the plugin's:
`python3 demo/tools/check_shaders.py` (run by `tools/verify.sh`). Serve it locally
with `python3 -m http.server` in `demo/`. See AGENTS.md, *The browser demo*.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside Resolume).

    ~/Library/Logs/slope/slope.YYYY-MM-DD.log        (macOS)
    %LOCALAPPDATA%\slope\logs\slope.YYYY-MM-DD.log   (Windows)
