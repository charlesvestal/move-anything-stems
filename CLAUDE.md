# CLAUDE.md — Stem Separation Module

External tool module for Move Everything. Separates audio into stems using SpleeterRT.

## Module Structure

```
src/
  module.json       # Module metadata (component_type: "tool")
  separate          # Shell driver: chunks long inputs, invokes the engine
  help.json         # On-device help content
  tools/
    wavchunk.c      # WAV splitter/joiner, cross-compiled to ARM64 by build.sh
  engine/
    spleeter        # SpleeterRT binary (pre-built ARM64)
    libgfortran.so.5
    libopenblas.so.0
tests/
  test_wavchunk.py  # Round-trip + mutation tests, run by CI
```

## Build & Deploy

```bash
./scripts/build.sh        # Cross-compile wavchunk, package to dist/stems/
./scripts/install.sh      # Deploy to Move device
python3 tests/test_wavchunk.py   # Host tests, no device needed
```

The engine ships pre-built. `wavchunk` is cross-compiled for ARM64 using
`${CROSS_PREFIX}gcc` if one is on PATH, else Docker. build.sh **hard-fails** if
the binary is missing, is not ARM64, or is absent from the tarball — a helper
that silently fails to build leaves the device running whatever stale copy it
already had, which defeats every bisect that follows.

## Release

1. Update version in `src/module.json`
2. `git commit -am "bump to vX.Y.Z"`
3. `git tag vX.Y.Z && git push --tags`
4. GitHub Actions builds and creates release

## How It Works

The `separate` script:
1. Receives input WAV path and output directory from shadow_ui.js
2. Runs SpleeterRT in 3-stem mode — over chunks, if the input is long
3. Renames outputs to `drums.wav`, `vocals.wav`, `accompaniment.wav`
4. Creates `.done` marker on success or `.error` on failure

The shadow UI's tool framework handles file browsing, progress display, and stem review.

## The engine's memory is linear in input length

SpleeterRT holds a full-length spectrogram, so peak RSS is ~302 MB + 4.83 MB
per second of audio (measured on device 2026-09-01: 30 s -> 447 MB,
60 s -> 592 MB). Past ~145 s of audio that exceeds the Move's ~1.25 GB free and
the OOM killer takes the engine; the driver surfaces it as `Exit code: 137`.

**v0.3.2's swapfile never worked.** It created `/data/UserData/.stems-swap` and
called `swapon`, but `swapon(2)` needs `CAP_SYS_ADMIN` and this module runs as
uid 1000 — every device in the field got `EPERM`, silenced by
`if swapon ... 2>/dev/null`. Three consequences: the OOM protection was inert,
the 1 GB file leaked (cleanup was gated on a flag that was never set), and the
first run on any device paid ~100 s of `dd` before touching audio. v0.4.0 drops
it and `rm -f`s the stray file.

**Intermediates must not be `*.wav` in the output directory.** The shadow UI
counts every `.wav` there as a finished stem and offers them all for review, so
chunk files live in `$OUTPUT_DIR/.work` and are removed before `.done`.

**The UI cancels by SIGTERMing the driver.** Now that the driver outlives a
single engine invocation, it traps TERM/INT and kills the engine child —
otherwise a cancel orphans a process burning three cores.
