# Axiom — Architecture

**AI instrument reconstruction synthesizer.** Import one sample or stem; Axiom infers the
*synthesis recipe* behind it — oscillators, filter, envelopes, modulation, effects — and
builds a fully playable, fully editable polyphonic synthesizer from that recipe.
Everything runs locally: no cloud, no Python, no web tech.

> Working title in the original brief: *SynthGenesis*. Shipping codename: **Axiom**.

---

## 1. Core philosophy

Axiom is **not** a sample player, pitch shifter, or wavetable extractor. The product of an
import is an `InstrumentPatch` — a compact, human-readable set of synthesis parameters.
The synth engine regenerates audio mathematically for any MIDI note; the patch stays
editable forever ("Patch Discovery"). Three consequences drive the whole design:

1. **The patch is the contract.** Analysis produces features; reconstruction produces a
   patch; the engine consumes a patch; export serializes a patch. Any tier of the AI stack
   can be upgraded without touching the others.
2. **Deterministic first, neural as refinement.** A DSP-analytical estimator ships working
   on day one, is explainable, and doubles as the *prior* that neural models correct. The
   product never depends on a model download to function.
3. **The engine is the dataset.** Because the engine renders any patch, it can generate
   unlimited (patch → audio) training pairs. The neural patch-refinement model is trained
   by *inverse synthesis* on Axiom's own oscillators — so what the model predicts is
   exactly what the engine plays.

## 2. High-level signal flow

```
Import (WAV/AIFF/FLAC/MP3)
  └─ decode (JUCE AudioFormatManager, ≤30 s window)
Stem separation (optional, Model 1 — ONNX slot "stem_separator")
Feature extraction              [Analysis/, analysis thread]
  ├─ YIN pitch track  → root note, stability, confidence, vibrato
  ├─ STFT 4096/Hann   → harmonic ladder (24), centroid, flatness, rolloff,
  │                     noisiness, odd/even ratio, slope, inharmonicity,
  │                     unison-detune estimate, filter-LFO estimate
  ├─ RMS contour      → ADSR estimate, transient sharpness, dynamic range
  └─ L/R correlation  → stereo width
Reconstruction                  [AI/, analysis thread]
  ├─ AnalyticalReconstructor (tier 1, always available)
  └─ ONNX "patch_refiner" (tier 2, optional, refines tier 1 prior)
DDSP encoding                   [AI/DdspEncoder, analysis thread]
  ├─ per-frame harmonic tracking (measured STFT tier, always available)
  └─ ONNX "ddsp_decoder" (optional: (f0, loudness) frames → harmonics+noise)
InstrumentPatch + DdspTimbre ──(atomic double-buffer)──► SynthEngine [audio thread]
  └─ 16 voices, two switchable layers per voice (engineMode: Recipe/DDSP/Both):
     ├─ recipe: PolyBLEP oscs → drive → TPT SVF → amp env
     ├─ DDSP:   64-partial additive resynth @ played f0 + filtered noise
     │          → drive → SVF → transparent gate env
     └─ FX rack: saturation → chorus → delay → reverb → width → gain
Export                          [Export/, export thread]
  └─ .axiom (JSON) · SFZ + 24-bit WAV multisamples · DecentSampler
```

## 3. Module map (folder structure)

| Folder | Contents | Depends on |
|---|---|---|
| `Source/Core/` | `InstrumentPatch` (data model + JSON), `DdspTimbre` (DDSP control frames + EngineMode), `AnalysisFeatures`, `ParamIDs` | JUCE only |
| `Source/Analysis/` | `AudioAnalyzer` (STFT/envelope/stereo), `PitchDetector` (YIN) | Core |
| `Source/AI/` | `IReconstructor`, `AnalyticalReconstructor`, `HybridReconstructor`, `DdspEncoder`, `IInferenceEngine` + ONNX seam, `ModelRegistry`, `IStemSeparator` | Core |
| `Source/Synth/` | `SynthModules` (PolyBLEP osc, TPT SVF), `DdspResynth` (additive+noise DDSP playback), `Voice`, `SynthEngine`, `EffectsChain` | Core |
| `Source/Export/` | `InstrumentExporter` (WAV/SFZ/DecentSampler/.axiom) | Core, Synth |
| `Source/Preset/` | `PresetManager` (file-based presets, favorites, factory seeds) | Core |
| `Source/GUI/` | `AxiomLookAndFeel`, `WaveformView`, `PatchView` | Core |
| `Source/` | `PluginProcessor` (composition root, DI), `PluginEditor` (passive view) | all |

Dependency rules: `Core` depends on nothing app-side; `Analysis`/`AI`/`Synth`/`Export`
depend only on `Core` (plus `Export → Synth` for offline rendering); GUI never touches
DSP or AI directly — it observes the processor. No globals, no singletons; the processor
constructor is the single composition root and injects `IInferenceEngine` into the
reconstruction stack.

## 4. Class hierarchy (key types)

```
juce::AudioProcessor
└─ AxiomAudioProcessor ────owns──► SynthEngine ──► Voice[16] ──► Oscillator, TptSvf, juce::ADSR
      │                                 └────────► EffectsChain (sat, chorus, delay, reverb, width)
      ├─ owns ► APVTS (29 automatable params) ──raw atomics──► RuntimeOverrides
      ├─ owns ► IInferenceEngine  (NullInferenceEngine | OnnxInferenceEngine)
      ├─ owns ► HybridReconstructor ──► AnalyticalReconstructor (fallback/prior)
      └─ owns ► AnalysisJob (juce::Thread) ──► AudioAnalyzer ──► PitchDetector

juce::AudioProcessorEditor
└─ AxiomAudioProcessorEditor ──► WaveformView | PatchView | MidiKeyboardComponent
                                 (all styled by AxiomLookAndFeel)
```

## 5. AI pipeline

**Model slots** (ONNX files in `~/Library/Application Support/Axiom/Models/`, discovered
at runtime by `ModelRegistry` — models are replaceable without recompiling):

| Slot | Purpose | Status |
|---|---|---|
| `stem_separator` | full mix → stems, tensor contract `[1,2,T] -> [1,S,2,T]` (Demucs / BS-RoFormer class); optional `stem_separator.json` manifest sets stem names / model rate / chunking | **harness shipped** (chunked inference, triangular crossfade reassembly, resampling); drop the .onnx in the model folder to activate. Until then `StemSeparationEngine` falls back to HPSS median-filter harmonic/percussive separation (Fitzgerald 2010) — separation always works |
| `patch_refiner` | packed `AnalysisFeatures` (48 floats) → refined synth parameters | I/O spec v1 implemented in `HybridReconstructor`; falls back to analytical tier when absent |
| `ddsp_decoder` | DDSP control frames, tensor contract `[1,T,2]` (per frame: midiPitch/127, loudness 0..1) → `[1,T,66]` (64 linear harmonic amps, noise gain, noise cutoff 0..1 log-mapped 20 Hz·2^(10x)) | **harness shipped** (`DdspEncoder`); without the model the same frames are measured directly from the source STFT (per-frame f0 refinement → harmonic peak ladder → residual noise gain/rolloff), so DDSP mode always works. Sessions live in the same `OnnxInferenceEngine` as the other slots |

`OnnxInferenceEngine` is live on macOS builds (`AXIOM_ENABLE_ONNX=1`, ONNX Runtime via
Homebrew, CoreML EP with CPU fallback, sessions cached per model). The Xcode build is
pinned to the native architecture because the Homebrew dylib is single-arch; shipping
builds will bundle a universal ONNX Runtime inside the app bundle.

**Spectral resynthesis (fidelity tier).** When pitch confidence and harmonic capture
are good, reconstruction bypasses waveform classification for playback: the measured
64-harmonic ladder becomes `patch.harmonicGains`, `OscType::Harmonic` renders it via
`Synth/Wavetable.h` (7 mip levels, 64→1 partials, golden-ratio partial phases, built on
the message thread into the same double-buffer slot as the patch). Sustained sounds
play through an open filter (the table already embeds the source filtering); percussive
sounds get a measured brightness-decay sweep. Classification still runs for the
description and as the fallback when the ladder is unreliable.

**DDSP resynthesis (pitch-stretch tier).** Alongside Patch Discovery, `DdspEncoder`
extracts a `DdspTimbre` block — up to 2048 harmonic+noise control frames (~93 fps) —
which `Synth/DdspResynth` re-renders per voice in real time: one shared sine LUT, one
phase accumulator per partial, per-tick amplitude ramps interpolated from the frame
pair under a playhead that ping-pongs across the measured sustain region (one-shots
play straight through). The partial count is capped below Nyquist per control tick, so
the source timbre stretches to *every key played* — polyphonic, alias-free, no
inference and no allocation on the audio thread. The `engineMode` parameter (Recipe /
DDSP / Both, chips in the PatchView) gates the recipe and DDSP layers per voice; each
layer has its own drive+filter+envelope so Recipe mode stays byte-identical to the
pre-DDSP engine, and with no timbre loaded the engine falls back to the recipe so keys
never go silent. The timbre block persists in session state (binary blob) and presets
(base64 in the `.axiom` JSON — old builds ignore the key).

**Patch Discovery (the differentiator).** Reconstruction output is not audio — it is an
editable synthesis recipe with a natural-language description
(`patch_io::describe`): *"Saw + Saw (−7/+7 ct) · Low-pass 3.4 kHz, res 0.31 · ADSR
4ms/210ms/0.74/300ms · vibrato 5.1 Hz · chorus"*, plus per-patch confidence and a
character profile (brightness/warmth/movement/complexity). Tier 1 does this analytically
today:

- **Oscillator classification** from the harmonic ladder: odd-only + slope ≈ −6 dB/oct →
  square; odd-only + ≈ −12 → triangle; full series → saw; strong evens → pulse (duty from
  first spectral null); fundamental-peak widening beyond the Hann mainlobe → unison
  detune → supersaw; buried upper partials → sine; high flatness → noise.
- **Filter exposure**: measured spectrum minus the classified oscillator's ideal spectrum
  *is* the filter curve. Persistent −6 dB residual knee → cutoff; positive residual bump
  at the knee → resonance.
- **Envelopes** from the RMS contour (10→90 % attack, plateau sustain, tail release,
  with long tails on sustained sources re-attributed to reverb).
- **Modulation** from autocorrelation of the pitch track (vibrato) and centroid track
  (filter LFO).

**Tier 2 training plan (M5)**: sample random `InstrumentPatch`es → render with
`SynthEngine::renderNoteOffline` → extract `AnalysisFeatures` → train an MLP/transformer
to invert (features → patch), export to ONNX, drop into the model folder. Loss combines
parameter regression with a multi-resolution spectral loss on re-rendered audio.

**Execution providers** when built with `AXIOM_ENABLE_ONNX=ON`: CoreML (macOS),
DirectML (Windows), CUDA, CPU fallback — selected at session creation in
`OnnxInferenceEngine`.

## 6. Threading & memory model

| Thread | Work | Rules |
|---|---|---|
| Audio | `SynthEngine::processBlock` | no locks, no allocation, no system calls; all buffers preallocated in `prepare()` |
| Message | GUI, host automation, patch application | only thread that calls `engine.setPatch` |
| Analysis (`AnalysisJob`) | decode → zone crop/fade → analyze → reconstruct | cancellable (`threadShouldExit` polled between stages); results posted via `MessageManager::callAsync` + `WeakReference` |
| Separation (`SeparationJob`) | full mix → stems (ONNX chunked inference or HPSS) | same cancellation/posting pattern; stems land in the dataLock-guarded model |
| Export (`juce::Thread::launch`) | offline multisample render + file IO | owns a private `SynthEngine`; progress via `shared_ptr<atomic<float>>` so processor teardown is safe |

**Patch handoff**: `InstrumentPatch` is trivially copyable (~600 B). Two-slot buffer +
`std::atomic<int>` index; message thread writes the inactive slot then flips
(release-store), audio thread reads the active slot once per block (acquire-load).
The heap-backed `DdspTimbre` block (up to ~0.5 MB of control frames) rides in a
parallel slot pair guarded by the same index, so patch, spectral wavetables and DDSP
frames always swap together; plain `setPatch(patch)` copies the current timbre across
(knob edits and preset saves never drop the resynthesis layer), while
`setPatch(patch, timbre)` replaces it.
Updates are rare (reconstruction/state-restore); continuous edits flow through APVTS raw
atomic floats (`RuntimeOverrides`) merged over the patch at block start — voices hear
knob changes on held notes, offline renders hear the pure patch.

**Control-rate optimization**: voices update filter coefficients, LFO and glide every 16
samples; per-sample work is oscillator ticks, SVF, and amp envelope only.

## 7. Rendering / GUI architecture

- `AxiomLookAndFeel` centralizes the design system (palette + neomorphic dual-shadow
  panels, rotary arcs, rounded controls). Dark neutral surfaces, one mint accent, wide
  spacing — Teenage Engineering / FabFilter direction.
- Editor is a passive view: `ChangeListener` for pipeline transitions + 10 Hz timer for
  meters (CPU %, voices, buffer/latency readout). Heavy state lives in the processor, so
  the editor can be destroyed/reopened freely (validated by auval's open/close cycles).
- Layout: header (brand, Import/Separate/Export) · left column `WaveformView` (drop
  zone, thumbnail, zone markers + fade handles, pipeline status overlay), stem chips
  (Full Mix / separated stems), `EmbeddingSphereView` · right `PatchView` (discovery
  readout, confidence, character bars, 16 macro knobs) · bottom 88-key keyboard +
  status strip.
- **Zone editing** (`Core/AudioZone`): start/end markers isolate and shorten the learned
  slice; raised-cosine fade-in/out (draggable handles) kill edge pops/clicks. The zone
  is applied on the analysis thread before feature extraction and persists in session
  state.
- **Neural Embedding Sphere** (`GUI/EmbeddingSphereView`): 60 FPS rotating constellation
  — every star is one extracted feature (gold pitch sun, cyan harmonic cluster, violet
  timbre, magenta envelope, teal modulation, noise dust density) with synapse links to
  nearest same-group features and to the pitch hub. Deterministic placement: the same
  sound always grows the same brain.
- Future (M6): move waveform + sphere to a `juce::OpenGLContext`-attached component for
  120 FPS at higher particle counts; latent-space projection once the neural tier ships.

## 8. Parameters & state

30 host-automatable parameters (stable string IDs in `Core/ParamIDs.h`; never rename —
`engineMode` is the Recipe/DDSP/Both engine selector).
Presets: `PresetManager` stores each preset as an `.axiom` JSON file (patch + presetName /
favorite / createdAt metadata) under the per-user data root (`Core/AppPaths.h` — note
JUCE's `userApplicationDataDirectory` is `~/Library` on macOS, so AppPaths adds
"Application Support" explicitly). The list is favorites-first alphabetical; the header
`PresetBar` offers prev/next cycling, a dropdown browser and heart/save actions. Loading
a preset routes through the same `engine.setPatch` + parameter-sync path as
reconstruction. The current preset name persists in session state.
Session state = `AxiomSession` ValueTree: APVTS state + patch JSON + source-sample path +
tier tag, serialized in `get/setStateInformation`. The `.axiom` file is the same patch
JSON with a `version` field — old sessions must always load (`patch_io` defaults every
missing field).

## 9. Export pipeline

`InstrumentExporter` renders C1–C7 every 3 semitones (25 zones, 24-bit/48 kHz WAV,
natural release tail captured, bounded 0.25–6 s) through a private offline engine and
writes: `.axiom` (lossless recipe), `.sfz` (ampeg from the patch; loads in Sforzando,
TX16Wx, and Kontakt via SFZ import — native `.nki` is a closed format), and `.dspreset`
(DecentSampler, reverb effect mapped). Progress is polled by the UI; the Export button is
disabled during runs.

## 10. Performance strategy

- Voice cost: 1–7 PolyBLEP partials + 2 SVF ticks + ADSR per sample; 16 voices ≈ well
  under one core at 48 kHz/64 samples (Release, LTO on — measured via the built-in CPU
  meter, ~budget fraction shown in the status bar).
- No allocations or locks on the audio thread (verified by design review; TSan/ASan pass
  planned in CI, see §12).
- SIMD headroom: oscillator partial loops and the SVF are structured for future
  `juce::dsp::SIMDRegister` batching (M6); analysis already amortizes via STFT hops.
- AI inference never touches the audio thread; ONNX sessions are created lazily and
  cached per model.

## 11. Milestones

| M | Scope | Status |
|---|---|---|
| M1 | Core model, analysis pipeline, analytical reconstruction, 16-voice engine, FX, GUI, SFZ/DS/.axiom export, AU/VST3/Standalone, auval green | **done (this codebase)** |
| M2 | Reconstruction quality pass: golden-sample corpus + regression tests; loop/sustain detection; delay/echo detection (envelope autocorr); FM/inharmonic handling | next |
| M3 | Preset browser, A/B compare against source sample, undo/redo (ValueTree-based), MPE + aftertouch routing, mod matrix UI | |
| M4 | Stem separation: chunk/crossfade ONNX harness ✔, HPSS DSP fallback ✔, stem picker UI ✔, zone/fade editing ✔, ONNX Runtime integration ✔ — remaining: in-app model download manager (checksummed, resumable) | **mostly done** |
| M5 | Neural `patch_refiner`: synthetic-dataset generator CLI (engine-rendered), training recipe, ONNX export + CoreML/DirectML providers | |
| M6 | GPU waveform/spectrum rendering, SIMD voice batching, wavetable oscillator import, Kontakt-oriented export polish | |

## 12. Testing & validation strategy

- **Unit (M2)**: synthesize known signals → assert analyzer output (440 Hz saw → root A4,
  saw classification, open filter; filtered square → cutoff within ±1 harmonic;
  ADSR-shaped noise → envelope within tolerance). Reconstruction round-trip: patch →
  render → analyze → reconstruct → compare (this is also the neural training loss).
- **Real-time safety**: TSan build of processBlock under randomized MIDI fuzz;
  allocation-detector (override global new in debug audio callback).
- **Plugin validation**: `auval -v aumu Axm1 Bzrk` (passing today), Steinberg
  `pluginval` at strictness 10 for VST3, host smoke matrix (Live/Logic/Reaper/FL).
- **Export conformance**: load generated SFZ in sforzando/TX16Wx; dspreset in
  DecentSampler; schema-check `.axiom` JSON.

## 13. Build

- **macOS day-to-day**: `Projucer --resave Axiom.jucer` →
  `xcodebuild -project Builds/MacOSX/Axiom.xcodeproj -target "Axiom - All" -configuration Release build`
  (AU/VST3 auto-install to `~/Library/Audio/Plug-Ins/`).
- **Cross-platform / CI**: `cmake -B build -DJUCE_SOURCE_DIR=<path> [-DAXIOM_ENABLE_ONNX=ON]`
  → `cmake --build build`. C++20, JUCE 8.
- ONNX Runtime is an optional dependency by design; without it the product is fully
  functional on the analytical tier (`NullInferenceEngine` reports the backend in the UI).
