# AGENTS.md — Slope

Onboarding for whoever (or whatever) picks this up next. `CLAUDE.md` is the short
command reference; this is the *why*. Read "What is actually verified" before you
tell anybody this works.

---

## What the plugin is

A 1-bit delta modulator — CVSD, continuously variable slope delta — run along the
scan line of the picture, as an FFGL 2.1 effect (`SL01`, shown as `SW Slope`) for
Resolume Arena and Avenue. C++17 + GLSL 4.10, CMake, universal macOS `.bundle` and
a Windows `.dll`. MIT, intended home `github.com/stoatworks-labs/slope`.

Built 2026-09-24 in one session from the fleet's templates and `specs/SPEC-slope.md`
(with `BRIEF.md` and `BRIEF-ADDENDUM.md`): clamp for the harness, the verify script,
the `--pipe` contract, the negative-control pattern, `--offline` and the GL-less CI;
compander and clamp for the idea of a serial recurrence the GPU computes and is held
to; tinsel for `PassBuffer` and the trap list; graticule for the notes.

---

## The one idea

**One bit per sample: is the picture above or below the decoder's guess?**

Encoder and decoder are the same state machine — a guess y, a step Δ and the last J
bits — and the decoder never sees the picture, only the bits:

    b     = ( x >= y ) ? +1 : -1                          the bit; a tie is +
    hist  = ( hist << 1 | b > 0 ) & mask                  the last J bits
    run   = hist == 0 || hist == mask                     J alike in a row
    Δ     = clamp( β_s Δ + ( run ? Δ_add : 0 ), Δmin, Δmax )
    y     = rest + β_i ( y - rest ) + b Δ                 rest = mid-grey

The received bit is the sent one, flipped where an integer hash of (frame, line,
sample, coder) falls under the error rate; the decoder's y goes through a one-pole
reconstruction, r ← α r + (1 − α) y. Run it along every scan line, one sample per
`Pixels/Sample` pixels, and:

| what the rule does | what comes out |
| --- | --- |
| the guess climbs one step a sample | **slope overload**: an edge of height h is a ramp of ⌈h/Δ⌉ samples, smeared along the scan |
| on a flat input the bits alternate | **granular noise**: a period-2 hunt one step wide |
| a run of alike bits grows the step, the syllabic leak shrinks it back | **the trade-off**: adaptation shortens the ramp and lengthens the hunt after it |
| a flipped bit moves only the decoder's guess, by 2Δ, and the integrator leaks | **streaks** that fade on β_i^k |
| every line starts at the blanking level | **the left margin is itself an edge**, climbing |

### What does not fall out, and is the honest limit

- **Lines are independent.** A real serial system carries its state through the
  blanking interval; here every line restarts from black with the minimum step. A
  coder given 12 µs of blanking would settle near black anyway, and independent
  lines are what lets the GPU run them side by side.
- **The leak rests at mid-grey**, the bias point of an AC-coupled decoder. A codec
  built for speech has no DC to hold; a picture is mostly DC, so on a flat field away
  from mid-grey the coder spends bit density fighting the leak, and with a short
  Leak and a small step it cannot hold black or white at all — the step adapts and
  the flat area shimmers. That is the physics of putting DC through CVSD, and Leak
  is where the operator decides how much of it to have. The default (256 samples)
  holds a 1/64 step against full white with a bit to spare.
- **The syllabic time constant is a quarter of the integrator's**, the wrong way
  round for a speech codec, where the syllabic filter is the slow one. It is that
  way here because the picture wants two things a speech codec does not: flat
  areas that hold (a long integrator leak) and a step that falls back within the
  line after an edge (a short syllabic leak). One control, one ratio, stated.
- **Y+C chroma is coded at half the sample rate** and held between its samples;
  nothing interpolates it. Luma mode codes Y and carries the input's colour
  difference through untouched, which is the Y of a Y+C system with a clean C.

---

## The shape of the code

| File | What it is |
| --- | --- |
| `source/Model.h` | The recurrence, described; the `Perturb` bits; `kChunk`; the option tables; the colour constants. No code that runs the coder — that is GLSL. |
| `source/Controls.{h,cpp}` | Every slider to its physical unit, with powers of two where the harness needs exactness. |
| `source/Shaders.{h,cpp}` | Four shaders: vertex, sample, coder, display. **The coder pass is the plugin.** |
| `source/StateBuffer.{h,cpp}` | A two-attachment RGBA32F framebuffer: seven state values a sample do not fit one texel. |
| `source/Slope.{h,cpp}` | The plugin: parameters, buffers, the chunk schedule, the test hooks. |
| `source/PassBuffer.*` | tinsel's FFGLFBO with the leak fixed, for the sample buffer. |
| `source/Diag.{h,cpp}` | A log file, for the shader that will not compile. |
| `tools/sltest/` | The offline harness: renders, measures, benchmarks, pipes, dumps shaders. |
| `tools/check-shaders.sh` | glslc on the dumped shaders; verify.sh and CI both call it. |
| `tools/sweep.py` | No control is silently dead. |
| `tools/verify.sh` | All of it, at two rasters, plus the release-time checks done locally. |

Per frame:

1. **sample** — samples × lines, RGBA32F: each coder's signal box-averaged over
   `Pixels/Sample` pixels of the scan (luma; R, G, B; or Y with Cb and Cr averaged
   over the pair of samples they are coded on).
2. **coder** — one draw per chunk of 32 samples, in scan order, into a pair of
   buffers (E: encoder y, step, history, received bit; D: decoder y, step, history,
   reconstruction), ping-ponged between chunks. A fragment reads the state at its
   chunk's first sample − 1 from the pair the previous draw wrote and re-runs the
   recurrence up to its own sample.
3. **display** — the host's framebuffer: the decoded signal back to pixels, or the
   bitstream, and the mix.

### How a serial recurrence runs on a GPU with no image stores

The spec proposed a **windowed restart**: each output sample re-runs the coder from
K samples earlier, from a fixed state, K chosen so the leaks have forgotten the
start (β^K · range < 1/255). That was worked through on paper and **rejected**,
because the coder does not forget:

The idle pattern on a flat field is a 2-cycle. Two coders on the same flat input in
*opposite phases* — one at the upper value while the other is at the lower — stay in
opposite phases forever: with no leak the difference δ between them obeys
δ' = δ + Δ(b_A − b_B), which only ever moves δ by ±2Δ, so δ mod 2Δ is invariant; with
a leak both coders contract onto the *same* 2-cycle, one of them a sample behind the
other, and a sample's delay is not something a contraction toward the cycle removes.
So the phase at a given sample is a memory of the whole line — of the parity of
every edge's climb before it — and a window that starts K samples back from a fixed
state gets the phase from its own start, not the line's. On a flat field every
window is identical, so the granular noise vanishes entirely; near an edge it
comes back in a form the serial coder never produces. No K fixes this, and the
leak is not the mechanism that would.

What ships instead is **exact**: the line is cut into chunks of 32 samples and one
draw per chunk runs every line's chunk in parallel, each fragment re-running its
chunk from the true state at the chunk's start, which the previous draw wrote. The
state at every chunk boundary is the serial one; every fragment of a chunk does the
same arithmetic in the same order on the same numbers, so what fragment 31 writes is
what the next chunk's fragments start from. Cost: N/32 draws a frame and an average
of 16 re-run steps per sample. The chunk size was measured (see the bench) and 32
is the balance between draw calls and re-running. The spec's windowed restart with
K = 0 is kept as a negative control (`kPerturbRestart`) and `--reference` fails on
it in every setting.

---

## Traps

Roughly in the order they will bite.

### ☠️ The idle pattern's phase is a memory no leak erases

Above. It cost the spec's chosen GPU form, and it shapes the harness too: a check
that compares the GPU to a serial run cannot tolerate a single flipped comparator
decision, because one flip is a different orbit for the rest of the line — which is
why the exact settings and the tie exclusion below exist.

### ☠️ A pixel shows the guess AFTER its own bit

The decoder's output for sample n is y after integrating bit n, y[n+1] in the
recurrence's indexing. `--overload` was written knowing that (the staircase is
y_s + (i + 1)Δ); `--granular` was not, and read as exactly one step wrong at every
sample of every level — a mutation-sized error in the harness, caught only because
the exact checks have a tolerance of zero.

### ☠️ The display clamps to 0..1, and an adapted step overshoots

`--adapt` first ramped to 0.98; with the step grown to 0.07 the guess crossed 1.0,
the display's `clamp()` truncated it, and the last step of the run read as half its
law value — 99 tolerances out, on a correct plugin. The ramp now tops at 0.88, the
trade-off edge at 0.8, and `--adapt` and `--tradeoff` report a reading on the floor
or ceiling as a failure of its own (clamp's `clipped()` trap, met again).

### The run detector fires a sample before the counted run's J-th bit

The hunt's last bit before a ramp can itself be a +, so the first run fires after
two counted + bits, not three. Assuming otherwise put the first checked step one
sample early. `--adapt` now reads the detector's firing off the Show Bits picture.

### 0.6f is not 0.6

The harness asks for a 64-sample leak through `sliderForTauI(64)`; the float slider
that gives is 0.6000000238, and the law gives 63.999989 samples on both sides. The
first `--laws` asserted `IntegratorTau(0.6f) == 64.0`. The float slider is the
statement, not the decimal; the dyadic promises are made only where a slider is an
exact float (0, 0.5, 1).

### `Pixels per Sample` is seventeen characters

The FFGL name field a host shows is sixteen, and the SDK hides the cut. It is
`Pixels/Sample`. `--names` checks every name.

### zsh has no `PIPESTATUS`

`${PIPESTATUS[0]}` is bash; in zsh it is `$pipestatus` and reads empty. The
harness's closed-stdout exit was right all along (stderr said "stdout closed at
frame 1") and a hand check in zsh said nothing. `verify.sh` is bash.

### Inherited from the fleet, and all still true here

`ScopedFBOBinding` does not restore the viewport (the host's is captured first and
restored before the display, with the host's FBO bound explicitly); every
`ffglex::Scoped*` clears to 0 on exit, so every `Ensure()` happens before anything
binds a texture, and the coder loop uses raw binds it clears itself;
`FFGLFBO::Release()` leaks the colour texture (`PassBuffer::Destroy()` deletes it
first); `SetParamInfo` clamps a STANDARD default into 0..1 and `SetParamInfof` reads
its default out of `params[]`; an option's range reads back 0..1 whatever its element
count; the core is an **OBJECT** library; `SetTextParameter` must return `FF_SUCCESS`
for the About block; `FFGLShader::Set` has no unsigned overload (`glUniform1ui` for
the hash seed and threshold); `nm | grep -q` fails under pipefail when grep
succeeds; a closed stdout must be a failed write, so `--pipe` ignores SIGPIPE.
Resolume's clock overflowing a float does not arise: the plugin has no clock.

---

## Would this hold on another rasteriser, at another raster?

One line per check. Every tolerance is derived, not fitted; every check ran at
320×180 and 1280×720 in `verify.sh`.

What makes them rasteriser-proof by construction: **every coordinate is an integer
computed in integers** (`gl_FragCoord`, never an interpolated uv); **every read is
`texelFetch`**; **every coefficient is computed on the CPU in double** and handed
over as a float uniform, and the harness's statement of each law is rounded through
the same float; **no transcendental runs on the GPU** in any checked path; and the
recurrence is written as `rest + leak * ( y - rest ) + d` and
`clamp( LeakS * step + add, .. )` so that in an **exact setting** — no leak (β = 1),
no filter (α = 0), RGB (no luma dot or colour transform), a power-of-two pitch and
steps on a 2⁻¹² grid, on input from a 1/1024 grid — every value the coder forms has at
most 14 significant bits and **no order of summation, reassociation or fma
contraction can round it**. In those settings the tolerance is zero and the check
is independent of the driver's arithmetic. The comparator's tie (x == y) is then an
exact tie, decided by `>=` on both sides.

In the other settings a **running bound** is carried beside the double reference
(u = 2⁻²⁴, M = 2.5 the largest |y − rest|): per sample, e_s ← β_s e_s + 2.2u Δmax
(0 when the clamp clearly holds), e_y ← β e_y + e_s + 4uM, e_r ← α e_r + (1 − α) e_y
+ 4uM, e_x = (pitch + 1)u (+ 8u for a luma dot or colour transform), plus the
display's own roundings per channel. A comparator decision with |x − y| ≤ e_x + e_y
is undetermined, so from that sample on that line's coder is **excluded** from the
comparison and the exclusion is counted and printed. The bound is loose (a γ_n bound
against errors that mostly cancel), which shows as the measured worst sitting at
0.01–0.06 of it, and as the exclusion growing with the leak and the line length
(35% at 720p with a 256-sample leak in Y+C, which is why that setting now uses 64).

| check | what it measures | tolerance and where it comes from | raster dependence |
| --- | --- | --- | --- |
| `--overload` | the run of + bits from the edge (Show Bits) and the staircase values | **0**: exact setting; the prediction is the closed form ⌈(b − y_s)/Δ⌉ with y_s from the idle pattern's parity; the part-sample cases assert the box average's margin against the guess ≥ Δ/8 | the edge sits at 3/5 of the scan; 15 cases need ≥ 31 samples after it |
| `--granular` (a) | every sample from the lock sample on against the idle pattern | **0**: exact setting, closed form | none |
| `--granular` (b) | period 2 and peak-to-peak on the last half of the line, 64-sample leak at the rest level | Δ β^(n − lock) (the orbit's contraction onto the cycle from within a step) + 4 e_y | a shorter line is less converged: 0.008 of tolerance at 320 wide, 0.000 at 1280 |
| `--adapt` law | each step of the run, recovered from y as (y_n − rest) − β_i(y_{n−1} − rest), against β_s × the previous measured step + Δ_add | 2(e_y[n] + e_y[n−1] + e_y[n−2]) + 2β_s(e_y[n−1] + e_y[n−2]); refuses clipped readings | none: the ramp is 11 samples wherever it starts |
| `--adapt` climb | the two runs against the stated recurrence from the stated run-up | **0**: exact setting | none |
| `--tradeoff` | five climbs and five post-edge energies | none: monotonicity, measured; refuses clipped readings | none |
| `--leak` | d_k between two renders against −2 b_s Δ β^k; τ fitted from the first and last point > 20 tolerances | 2 e_yd[n] + 8uM (two renders) | more usable points on a longer line (160 / 421 at 256 samples) |
| `--reference` | every pixel of three noise frames, five settings | **0** in the two exact settings; the running bound with tie exclusion in the three others | the exclusion grows with line length (1.4–12.6% at 720p) |
| `--vertical` | every value of the transposed render against the transposed horizontal one | **0**: the same instructions on the same numbers, only addressing differs | none |
| `--errors` | flipped bits against the reference's transmitted ones; the two frames' flip masks | 4σ of Binomial(n, p) and of 2np(1 − p) | σ shrinks with n: ±165 at 320×180, ±662 at 720p |
| `--laws` | every control law at 21 points; the dyadic promises | 1e-12 relative; exact | none (no GL) |

Deliberately NOT relied on: round-to-nearest anywhere in a bounded setting;
`mix(a, b, 1) == b` (the display returns early at Mix 1); any GPU transcendental;
interpolated varyings; a texture unit's filtering; GLSL integer division of a
negative operand (none occurs); the 8-bit readback (the harness reads floats).

What might still differ on another rasteriser: a driver whose float `+`, `−`, `×`
are not correctly rounded would break the bounded settings (GLSL 4.10 requires
them); the exact settings survive even that, short of a driver that flushes
denormals — none arise — or computes in less than 24 bits. `check-shaders.sh`
covers the syntax on a second compiler and the rendered checks run with
`--allow-no-gl` so a runner without a context skips loudly.

### The negative controls

`sltest --negative` runs nine against the rendered checks; `--perturb BITS` runs any
check verbosely against one. Each perturbs the *plugin's* shaders — a `Perturb`
uniform the shipped plugin carries at zero — never the harness's expectation. Every
one below was read for what it failed on; none fails on a clipped reading.

| perturbation | what fails, measured at 320×180 |
| --- | --- |
| the integrators add 1.25 steps a bit | `--overload`: climbs of 7, 11, 22 against 9, 14, 28; staircase 0.035–0.094 off |
| the integrators leak toward black | `--granular` (b): peak-to-peak 0.01204 against 0.01575, 24 tolerances; period 2 broken by 50 |
| the run detector removed (the spec's) | `--adapt`: 30 steps, none grown, 21 tolerances at the first run sample; the adaptive climb 20 against a predicted 4. `--tradeoff`: five climbs of 23 and five energies of 0.0047 |
| the decoder's integrator does not leak | `--leak`: 369 and 66 tolerances, fitted τ = ∞ |
| every chunk restarts from the fixed state (the spec's K = 0) | `--reference`: worst 0.625 and 1.0 against 0 in the exact settings, 33,000 bounds in the Luma one |
| Y+C chroma at the full rate | `--reference` Y+C: worst 0.552, 20,000 bounds |
| the vertical scan runs up the columns | `--vertical`: 155,499 and 160,230 of 230,400 values differ |
| the error rate halved | `--errors`: 890 and 851 flips against 1,728 ± 165 |

### The mutation

One character of the shipped GLSL, on a clean committed tree: in the coder, the
comparator `int bit = x >= ye ? 1 : 0;` → `x > ye` (the tie decided the other
way). Caught at 320×180 and 1280×720 by `--granular` (all three no-leak levels,
worst exactly 2Δ = 0.03125: the lock sample's tie flips the phase), by
`--reference` (both exact settings: worst 0.03125 and 0.648 against a required 0)
and by `--errors` (2,017 and 2,058 "flips" against 1,728 ± 165: the received bits no
longer match the transmitted ones the reference states). `--overload` passed it at
both rasters — the mutated run-up hunts at {a − Δ, a} instead of {a, a + Δ}, and at
the edge sample the parity happened to put the guess at a in both, so the count
came out the same; a different edge column would have caught it. `--adapt`,
`--tradeoff`, `--leak` and `--vertical` passed, correctly: none depends on the tie.
Reverted with `git checkout source/Shaders.cpp`; the tree was clean before and
after.

---

## Decisions taken without asking

- **The GPU form is chunked-exact, not the spec's windowed restart** (above). The
  spec's fallback ("a per-line chunked pass") is what this is. K and its bound do not
  exist; the windowed restart survives as a negative control.
- **Leak may be none.** With no windowed restart there is nothing to floor it for,
  so Leak at 0 is β = 1 exactly; that is also what makes the exact checks possible.
- **Every line starts at the blanking level** (0) with the minimum step and an
  alternating history register (0x5) so the empty register cannot read as a run.
- **The step is updated by the current sample's bit before that bit is integrated**,
  as the hardware's syllabic filter sees the coincidence of the bits it has.
- **The comparator's tie is +.**
- **The integrators leak toward mid-grey; the syllabic time constant is a quarter of
  the integrator's** (both above). Leak: 0 = none, else 4 × 2^(10(1 − v)) samples.
- **Adaptation Rate is the increment per run**, v × Max Step / 8; with no syllabic
  leak the step grows without bound to Max Step, with one it settles at
  Δ_add / (1 − β_s) or Max Step, whichever is lower.
- **Reconstruction is a one-pole**, time constant 2^(6(1 − v)) − 1 samples, exactly
  none at 1; the default 0.75 is 1.83 samples, enough to take the period-2 hunt down
  without smearing an edge further than the coder already has.
- **Bit Errors**: 0 at 0, else 10^(−5 + 4v); seeded from a frame counter, not the
  host clock, because a channel meets fresh noise every frame and a counter is the
  same in the harness as in a host. `SetTimeSupported(false)`.
- **Show Bits shows the RECEIVED bitstream**, so errors are visible in it.
- **Luma mode carries the input's colour difference through** (out = in + (y − luma)),
  rather than going monochrome.
- **Pixels/Sample is a real integer**, 1..16, default 2.
- **`Pixels per Sample` → `Pixels/Sample`** (sixteen characters).
- **Steps are powers of two at the sliders' 0, 0.5 and 1** so exact checks exist.
- **kChunk = 32**, measured: at the defaults 0.60 / 1.21 / 2.89 ms (720p / 1080p /
  4K) against 0.81 / 1.22 / 3.06 at 16 and 0.75 / 1.47 / 3.35 at 64; on RGB at one
  pixel a sample 1.41 / 2.73 / 7.84 against 1.66 / 3.22 / 10.5 and 1.68 / 3.25 / 9.67.
- **No resize check.** Nothing carries across frames but the frame counter; a resize
  reallocates buffers that are rewritten from the start every frame.
- **No offline negative control for `--laws`**; it says so.
- **`--fps` is accepted and does nothing**, so the fleet's video renderer can pass it.
- **`--fail-render-at N`** is a harness-only hook so `verify.sh` can prove `--pipe`
  exits 1 on a failed render.
- **Provisional About and attributions** (`StoatworksAbout.h`, `ATTRIBUTIONS.md`) are
  hand copies adapted from clamp's with `guide=""`, so three About buttons; the
  release step registers the project and re-runs the syncs.
- **The FFGL submodule was dissociated from the reference clone** (`repack -a -d`,
  the alternates file removed) so this repo does not depend on a path in `~/Projects`.

---

## What is actually verified, and what is assumed

### Verified by measurement, on an M4 Max running macOS 26.4.1 (2026-09-24)

Every number is `tools/verify.sh` on this machine against a fresh universal Release
build, at 320×180 and 1280×720.

- **Overload.** 15 cases (three heights, two pitches, every fractional edge
  position): the climb starts at the predicted sample and runs the predicted count,
  9 / 14 / 28 for 8.5 / 13.5 / 27.5 steps; the staircase is exact.
- **Granular.** No leak: 2,448–2,736 samples at 320 wide and 9,856–10,112 at 1280
  exactly on the idle pattern, three levels. 64-sample leak at 0.5: peak-to-peak
  0.015747 against 2Δ/(1+β) = 0.015747, period 2, worst 0.008 of tolerance.
- **Adaptation.** Ten steps of an 11-bit run recovered from the picture, each on the
  law to 0.000 of tolerance (0.0156 → 0.0644); a 0.3 edge climbs in 20 samples fixed
  and 4 adapting, both predicted exactly.
- **Trade-off.** Climb 23 → 16 → 12 → 10 → 7 samples, energy 0.0047 → 0.0204, over
  five rates; same at both rasters.
- **Leak.** Fitted 64.02 and 256.0 samples; worst 0.03 of tolerance; the received
  bitstreams differ in exactly one sample.
- **Reference.** Five settings, ~0.5 M values each at 320×180 and 7.3–8.3 M at 720p:
  worst **0** in both exact settings; 0.010–0.060 of the running bound in the other
  three, with 0.6–12.6% of values excluded past comparator ties.
- **Vertical.** 0 of 230,400 (320×180) and 0 of 3,686,400 (720p) values differ, Luma
  and Y+C.
- **Errors.** 1,708 / 1,742 flips against 1,728 ± 165; 27,616 / 27,627 against
  27,648 ± 662; the frames' masks differ as 2np(1 − p) predicts.
- **Negative controls.** All nine fail their check, on the coder.
- **Mutation.** Caught (above), with the one check that missed it stated.
- **No dead controls**, all 12, with the four About buttons skipped.
- **Every shader compiles** through `glslc`, as the plugin hands it to the driver.
- **`--pipe`** returns exactly two frames for two and a half, refuses an unknown cue
  with 2, and exits 1 on a failed render and on a closed stdout (`| head -c 1`).
- **The bundle** is universal, exports `_plugMain`, carries
  `com.stoatworks.ffgl.slope`, ad-hoc signs, and `oxbow` reports `SW Slope` / `SL01`
  / `effect` and renders 120 frames through `plugMain`.
- **Render cost**, best of three runs of 60 frames after a warm-up, `glFinish` both
  sides, on a shared GPU:

  | | ms/frame | % of a 60fps frame | RGB, 1 pixel/sample |
  | --- | --- | --- | --- |
  | 1280×720 | 0.60 | 3.6% | 1.41 |
  | 1920×1080 | 1.21 | 7.2% | 2.73 |
  | 3840×2160 | 2.89 | 17.4% | 7.84 |

  The cost is the draw calls: N/32 of them a frame along the scan (120 at 4K
  horizontally), each cheap. A vertical scan of 4K is 68.

### Assumed, or not done

- ☠️ **Never loaded into Resolume**, on either platform. Everything was compiled,
  rendered and measured offline against the real plugin class in a headless CGL
  context, plus an `oxbow` load.
- **Never seen on footage.** Every picture so far is synthetic: cards, edges, noise.
- **The defaults are judged from three renders of the card**, not from a show. The
  left margin's climb from blanking is visible by design; whether an operator wants
  it is untested.
- **Not verified at 4K**, only benchmarked there.
- **Windows** has not been built; the CI workflow is adapted from clamp's and has not
  run.
- **No OpenFX port**, not required for 0.1.0. The browser demo's draw schedule
  and control laws are a hand port nothing checks; see *The browser demo*.
- **`StoatworksAbout.h` and `ATTRIBUTIONS.md` are provisional hand copies** with
  `guide=""`; register the project and re-run the syncs before the first release.
- **Nothing has been through a show.**

---

## Open questions

- **Should lines carry state through blanking?** Truthful to a serial system; it
  would make the whole frame one recurrence (N × L / 32 draws) or need a per-line
  start state estimated from the previous line's end, which is a second mechanism.
- **Should the leak rest at black rather than mid-grey?** Black is the blanking
  level a DC-coupled decoder would rest at; mid-grey halves the worst-case bit
  density the coder spends holding a level. Either is one constant (`kRestLevel`).
- **Should the syllabic ratio be a control?** One more slider against a ratio nobody
  will know how to set.
- **Should the draw count be cut** with a two-level chunking (a coarse pass to find
  chunk-start states over 8 chunks at once, then the fine one)? It would roughly
  halve the draws at the cost of a third pass and a longer re-run; 4K is at 17% of a
  frame now.
- **A tighter running bound** would need the errors of consecutive steps to be
  correlated, which a worst-case bound cannot use; the exact settings make up for it.

---

## The browser demo

`demo/` is the page at **slope-demo.stoatworks-labs.com**, a static-assets Worker
deployed from `wrangler.toml` with `cf-run npx wrangler deploy` and by
`.github/workflows/deploy.yml` on every push to main (no build step; what is
committed is what is served). `demo/vendor/` is the shared kit from
`stoatworks-backend/resolume-demo/` and is not edited here. The host is a Worker
**route** plus a proxied `AAAA 100::` DNS record, not a custom domain: the zone
hit Cloudflare's 100-custom-domain limit on 2026-09-24. Delete that record and
the page goes dark while deploys stay green.

The page runs the plugin's four shaders, copied across unedited:
`demo/tools/check_shaders.py` compares them with `source/Shaders.cpp` character
for character and `tools/verify.sh` fails if one drifts. This plugin has no CPU
half to speak of — the coder is the shader — so the demo is closer to the plugin
than galvo's or clamp's. **What is a port** is the scheduling: `Controls.cpp`
function for function, `Slope::ProcessOpenGL`'s order (the sample pass, one coder
draw per chunk of `kChunk` with the viewport offset so `gl_FragCoord.x` is the
sample index, the ping-pong between two state buffers, the display), and
`StateBuffer.cpp` as a WebGL2 framebuffer with two RGBA32F attachments
(`gl.drawBuffers`; `EXT_color_buffer_float` is required and the kit refuses to
start without it). **Nothing checks that port but a reader.** Change any of those
and change `demo/plugin.js` by hand to match.

What the page does differently, all of it said on the page:

- **Pixels/Sample is a dropdown** of 1..16. It is `FF_TYPE_INTEGER` in the plugin
  and the kit has no integer control.
- The Max Step readout shows its law alone; the coder takes `max( StepMin, … )`
  as the plugin does.
- The bit errors are seeded from the page's own per-render counter, as the
  plugin's `frameIndex++` is. The harness hooks (`Perturb`, `Forced*`) are held
  at 0 and -1.
- The About block is absent, as on every page in the suite. No audio caveat:
  Slope has no audio path.

Decided without asking, for the page: colour bars lead the clip list because hard
edges are the effect; the presets are the page's own (the plugin ships none),
expressed entirely in its parameters; and a line under the canvas reports the
geometry the port chose (samples per line, lines × coders, coder draws).

---

## Siblings

- **clamp** — the harness, verify, CI and `--pipe` shapes, the negative controls,
  `--offline`, the clipped-reading guard, the AGENTS.md shape.
- **compander** — a serial recurrence the GPU does in parallel, checked against the
  serial one.
- **tinsel** — `PassBuffer`, `sweep.py`, and the fleet's trap list.
- **graticule** — the notes, and the provisional About.
- **oxbow** — `oxbow probe` and `oxbow selftest` are what load this bundle as a host.
