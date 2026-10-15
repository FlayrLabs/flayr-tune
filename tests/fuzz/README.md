# Fuzzing

Two fuzz targets cover the input Flayr Tune takes from outside:

- **`fuzz_state`**: `setStateInformation`, the whole path a project file takes: binary
  header, the XML pre-scan, JUCE's XML parser, the 1.0-1.2 throat migration, non-finite
  filtering and the PitchTrack base64 + gzip + decode. The first input byte picks the layer
  the rest goes to (raw blob, XML text, raw PitchTrack payload, gzip stream), so mutations
  get past the header, the base64 and the gzip checksum. After loading it checks every
  parameter is finite and in range, the track is sane, save then load gives back exactly
  the same state, and Graph-mode audio over the decoded track is finite and limited. A
  differential oracle (`tests/state_differential.h`) also parses every accepted blob with
  JUCE and fails if what JUCE built is beyond the pre-scan's limits.
- **`fuzz_vst3`**: the VST3 wrapper's state path. JUCE's wrapper splits the component
  state into the plug-in's part and its own `JUCEPrivateData` trailer, and reads the Bypass
  flag from it, before Tune sees anything; this runs that exact code
  (`juce::detail::VST3PrivateState`, from `patches/juce-vst3-private-state.patch`) and then
  `setStateInformation` on what it hands over. `tests/vst3_host_test.cpp` also loads the real
  VST3 bundle as a host would, in ctest.
- **`fuzz_host`**: what a host does while the plug-in runs, as a small opcode program:
  sample rates (0, negative, NaN, Inf, absurd), block sizes, mono and stereo, audio with
  NaN/Inf/denormal/huge samples, raw MIDI bytes, playhead positions (samples, seconds,
  non-finite), parameter changes including out-of-range and NaN, program changes, Graph
  capture and apply, and save/load mid-stream. Every block's output must be finite and
  within the hard limit.

Both build with AddressSanitizer and UndefinedBehaviorSanitizer (every UB report aborts).
They are off by default and never part of a release build.

## Build

```sh
cmake -S . -B build-fuzz -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFLAYR_TUNE_BUILD_FUZZERS=ON \
      -DFLAYR_TUNE_BUILD_TESTS=OFF -DFLAYR_TUNE_COPY_AFTER_BUILD=OFF \
      -DCMAKE_OSX_ARCHITECTURES=arm64          # macOS: one architecture is enough
cmake --build build-fuzz --target fuzz_state fuzz_host fuzz_make_corpus --parallel
```

With a clang that has libFuzzer (Linux: `-DCMAKE_CXX_COMPILER=clang++`) the targets link
libFuzzer. Apple clang has none, so there they link [`fuzz_driver.cpp`](fuzz_driver.cpp): a
small coverage-guided mutation fuzzer on `-fsanitize-coverage=inline-8bit-counters` that takes
the same options (`-runs`, `-max_total_time`, `-seed`, `-max_len`, `-timeout`, `-dict`,
`-artifact_prefix`) and is deterministic for a given seed.

## Run

```sh
cd build-fuzz
A=RelWithDebInfo
./fuzz_make_corpus_artefacts/$A/fuzz_make_corpus seeds/state seeds/host seeds/vst3
mkdir -p corpus/state corpus/host corpus/vst3 artifacts
./fuzz_state_artefacts/$A/fuzz_state -max_total_time=900 -dict=../tests/fuzz/fuzz.dict \
    -artifact_prefix=artifacts/ corpus/state seeds/state
./fuzz_host_artefacts/$A/fuzz_host -max_total_time=900 -artifact_prefix=artifacts/ corpus/host seeds/host
./fuzz_vst3_artefacts/$A/fuzz_vst3 -max_total_time=900 -dict=../tests/fuzz/fuzz.dict \
    -artifact_prefix=artifacts/ corpus/vst3 seeds/vst3
```

Some bugs only show on big inputs (quadratic parsing, huge allocations): also give one
`fuzz_state` copy `-max_len=1048576` and its own corpus directory for a slower run.

The seeds are real states from `getStateInformation` (default, every preset, a 400-point
and a full 30-minute PitchTrack, a 1.2 "throat" project) and the hostile blobs from earlier
security fixes. Run several copies with different `-seed=` values to use more cores.

A crash, sanitizer report, failed check or timeout saves the input as
`artifacts/crash-<hash>` (or `timeout-<hash>`). Re-run it with
`./fuzz_state_artefacts/$A/fuzz_state artifacts/crash-<hash>`, fix the bug, and add the
input to [`tests/plugin_test.cpp`](../plugin_test.cpp) as a regression test.

CI runs each target for 60 seconds with libFuzzer on Linux.
