# Contributing to Flayr Tune

Thanks for helping. A few notes so changes land smoothly.

## Reporting bugs

For security problems, please report privately instead: see [SECURITY.md](SECURITY.md).


Please include:
- Your OS and DAW, with versions.
- The plug-in format (AU, VST3 or LV2).
- Whether you were in Studio, Live or Graph mode.
- A short audio clip, if the bug is about how it sounds.

## Making changes

- **DSP lives in `Source/TuneEngine.h`** and has no JUCE dependency. Add or update a check in
  `tests/engine_test.cpp` for any behaviour change, and run `ctest --test-dir build -C Release`.
- **The audio thread must never allocate, lock or look parameters up by name.** Follow the
  existing pattern: cached parameter pointers, preallocated buffers, atomics for GUI data.
- **Parameter IDs are permanent.** Saved projects depend on them. Rename the display name
  instead (for example, the Expression knob still has the ID `flex`).
- **UI changes:** build with `-DFLAYR_TUNE_BUILD_TOOLS=ON` and attach a `FlayrTuneSnapshot`
  render to the pull request.
- **Validate the plug-in** with [pluginval](https://github.com/Tracktion/pluginval) at strictness 10 if you can.

## Releases (maintainers)

1. Bump `project(FlayrTune VERSION x.y.z)` in `CMakeLists.txt`, commit, then tag and push `vx.y.z`.
   GitHub Actions builds the Windows installer and the Linux package and puts them on a draft release.
2. On a Mac with the Flayr Labs Developer ID, run `scripts/release-macos.sh --upload` to add the
   signed and notarized DMG.
3. Publish the draft.

By contributing, you agree that your contributions are licensed under the AGPLv3.
