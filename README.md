# Stem Separation for Move Everything

Separate audio files into individual stems (drums, vocals, accompaniment) directly on your Ableton Move using SpleeterRT.

## Features

- 3-stem separation: drums, vocals, accompaniment
- ~0.85x realtime processing speed
- Any track length — long files are chunked so memory stays bounded
- Stem review with selective saving — keep all or pick individual stems
- Output saved as WAV files to UserLibrary/Stems/

## Prerequisites

- [Move Everything](https://github.com/charlesvestal/move-everything) installed on your Ableton Move
- SSH access enabled: http://move.local/development/ssh

## Install

### Via Module Store (Recommended)

1. Launch Move Everything on your Move
2. Select **Module Store** from the main menu
3. Navigate to **Tools** > **Stem Separation**
4. Select **Install**

### Build from Source

```bash
git clone https://github.com/charlesvestal/move-everything-stems
cd move-anything-stems
./scripts/build.sh
./scripts/install.sh
```

## Usage

1. Open the Tools menu (Shift+Vol+Step13)
2. Select **Stem Separation**
3. Browse and select a WAV file
4. Confirm to start processing
5. Review the produced stems — all are selected by default
6. Push **Save All** to keep everything, or deselect stems you don't want
7. Stems are saved to `UserLibrary/Stems/<filename>/`

## Output

Each separation produces three WAV files:
- `drums.wav` — percussion and drum hits
- `vocals.wav` — vocal content
- `accompaniment.wav` — everything else (bass, synths, guitars, etc.)

44.1 kHz stereo, 32-bit float.

## Long files

The SpleeterRT engine holds a full-length spectrogram, so its peak RSS is
linear in input length. Measured on device:

| input | peak RSS |
|-------|----------|
| 30 s  | 447 MB   |
| 60 s  | 592 MB   |

That is ~302 MB + 4.83 MB per second of audio. The Move has ~1.25 GB free, so
anything past roughly 145 s of audio used to be killed by the OOM killer — the
driver reported it as `Exit code: 137`.

Inputs longer than one chunk (60 s by default) are now split into overlapping
chunks, separated one at a time, and rejoined by `wavchunk` with a linear
crossfade across each 2 s overlap. Peak RSS then follows the chunk length
instead of the track length, so a 4-minute track peaks at the same ~590 MB a
1-minute one does.

Chunk geometry can be overridden for testing with the `STEMS_CHUNK_SEC` and
`STEMS_XFADE_SEC` environment variables.

Measured against a single pass over the same 100 s of music, the chunked result
is bit-identical for the first chunk, correlates 0.993–0.997 across the seam,
and 0.95–0.98 through the second chunk. The second-chunk difference is not a
seam artifact — it is the model seeing a different spectrogram context — and it
sits 10–13 dB below the stem's own level.

## Credits

- **SpleeterRT engine**: Real-time stem separation
- **Move Everything framework**: [Charles Vestal](https://github.com/charlesvestal/move-everything)

## License

MIT License — See [LICENSE](LICENSE)

## AI Assistance Disclaimer

This module is part of Move Everything and was developed with AI assistance, including Claude, Codex, and other AI assistants.

All architecture, implementation, and release decisions are reviewed by human maintainers.
AI-assisted content may still contain errors, so please validate functionality, security, and license compatibility before production use.
