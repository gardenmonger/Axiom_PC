# Axiom

**Teach it a sound once. Play it forever.**

Axiom is an AI-powered instrument-reconstruction synthesizer (AU / VST3 / Standalone,
JUCE 8, C++20). Import a single sample or an isolated stem and Axiom infers the
*synthesis recipe* behind it — oscillator stack, filter, envelopes, LFO, effects — then
builds a fully playable 16-voice synthesizer from that recipe. The result is a living,
editable patch, not only stretched audio. Everything runs locally and offline.

## What works today

- Import WAV / AIFF / FLAC / MP3 (drag-and-drop or Import button)
- **Stem separation** (Separate button): neural ONNX `stem_separator` model when one is
  installed in the model folder (Demucs-class, chunked with crossfade reassembly),
  automatic HPSS harmonic/percussive DSP fallback otherwise — pick a stem chip and it
  becomes the instrument source
- **Zone editing**: drag the start/end markers on the waveform to isolate and shorten
  the slice the AI learns from; drag the fade handles for raised-cosine fades (no pops
  or clicks); double-click resets; the instrument re-learns on release. **Zoom** with
  the scroll wheel (anchored at the cursor, down to a 20 ms window), pan with
  horizontal scroll, or use the + / − / Fit buttons for fine marker placement
- Analysis pipeline: YIN pitch detection, harmonic-ladder spectral analysis, ADSR
  extraction, vibrato/filter-LFO detection, unison-detune and stereo-width estimation
- **Patch Discovery**: analytical reconstruction of oscillator types (sine / triangle /
  saw / square / pulse / supersaw / noise), filter cutoff + resonance, envelopes,
  modulation and FX sends — shown as a readable recipe with a confidence score
- **Spectral resynthesis (source-faithful timbre)**: when the harmonic ladder is
  trustworthy, Axiom plays the *measured* spectrum itself — up to 64 harmonic
  amplitudes baked into mip-mapped additive wavetables (alias-free on every key) —
  instead of approximating with an idealized waveform. Detected unison becomes a
  detuned stereo pair of the same spectrum; percussive sources get a brightness-decay
  filter sweep matched to the measured envelope
- **Neural Embedding Sphere**: a rotating galaxy constellation where every star is one
  learned feature (pitch sun, harmonic cluster, timbre, envelope, modulation, noise
  dust) with synapse links — the instrument's brain, rendered at 60 FPS
- **DDSP resynthesis engine (pitch-stretch to every key)**: a second synthesis path
  built on Differentiable DSP — per-frame harmonic + filtered-noise control frames are
  extracted from the source (measured STFT tier always available; drop a
  `ddsp_decoder.onnx` into the model folder for the neural tier, running through the
  same ONNX Runtime as the other models) and re-rendered additively in real time at
  whatever keys are played, polyphonically and alias-free. The **RECIPE / DDSP / BOTH**
  chips (host-automatable `engineMode` parameter) toggle the classic recipe synth, the
  DDSP layer, or both layered per voice; the DDSP layer follows glide, pitch bend,
  vibrato and the filter section, loops its sustain frames on held notes, and persists
  through sessions and presets
- Real-time engine: PolyBLEP oscillators, TPT state-variable filter, dual ADSR, LFO,
  glide, pitch bend, mod wheel, sustain pedal; saturation → chorus → delay → reverb →
  width FX rack; 30 host-automatable parameters
- **Preset system**: header bar with prev/next cycling, a dropdown browser (Favorites
  section + all presets + save/delete/reveal actions), one-click favoriting (heart) and
  save-as. Saving always captures the sound **as heard** — every knob edit is folded
  into the preset. Editing a loaded preset marks it with `*` and the menu gains
  *Update "name" with edits* / *Revert to saved*. Presets are `.axiom` JSON files in
  `~/Library/Application Support/Axiom/Presets` (factory presets seeded on first run);
  exported `.axiom` instruments drop straight in as presets
- Export: `.axiom` patch (JSON), SFZ + 24-bit multisamples, DecentSampler preset
- ONNX Runtime is compiled in on macOS (CoreML/CPU execution providers) and Windows
  (CPU; `onnxruntime.dll` ships next to the binaries and is delay-loaded, so the plugin
  still scans fine without it); models are runtime-replaceable in
  `~/Library/Application Support/Axiom/Models/` (macOS) or `%APPDATA%\Axiom\Models`
  (Windows) — drop in `stem_separator.onnx` (+ optional `stem_separator.json` manifest)
  for neural-quality 4-stem separation, or `ddsp_decoder.onnx` for neural DDSP timbre
  frames, no recompile needed

A quick test file lives at `TestAudio/pad_plus_perc.wav` (detuned saw pad + percussion —
import it, hit Separate, pick the Harmonic stem, play).

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the full design, threading model,
AI pipeline and roadmap.

## Build (macOS)

```sh
/path/to/JUCE/Projucer.app/Contents/MacOS/Projucer --resave Axiom.jucer
xcodebuild -project Builds/MacOSX/Axiom.xcodeproj \
           -target "Axiom - All" -configuration Release build
auval -v aumu Axm1 Bzrk   # AU validation
```

## Build (Windows)

The VS2022 exporter expects an extracted [ONNX Runtime win-x64 release](https://github.com/microsoft/onnxruntime/releases)
at `%USERPROFILE%\Documents\onnxruntime` (a junction to the versioned folder works well).

```powershell
& C:\path\to\JUCE\Projucer.exe --resave Axiom.jucer
msbuild Builds\VisualStudio2022\Axiom.sln /p:Configuration=Release /p:Platform=x64 /m
```

The post-build step copies `onnxruntime.dll` next to the standalone exe and into the
VST3 bundle; the DLL is delay-loaded from there at first use.

## Build (CMake, all platforms)

```sh
cmake -B build -DJUCE_SOURCE_DIR=/path/to/JUCE   # add -DAXIOM_ENABLE_ONNX=ON for neural tier
cmake --build build --config Release
```
