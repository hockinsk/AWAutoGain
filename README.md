# Airwindows AutoGain shim

A drop-in loudness-matching wrapper for [Airwindows](https://www.airwindows.com/)
VST2 plugins. It proxies any Airwindows `.dll` (or `.so`/`.vst`) without modifying
it, forwards every parameter and the full plugin interface to the host, and inserts
an automatic gain-matching stage so the processed signal sits at the same level as
the dry signal — honest A/B, no "louder sounds better" bias.

It needs **no Steinberg VST2 SDK** to build (it uses a clean-room minimal ABI
header), so you can ship a prebuilt binary that users just run.

## How a user uses it (no building)

1. Download the release: `airwrap` (generator) + `awautogain` (shim template).
2. Run the generator against your Airwindows VST2 folder:

   ```
   airwrap --shim awautogain.dll --in "C:\VST\Airwindows" --out "C:\VST\Airwindows AG"
   ```

   This creates one wrapper per plugin: `ToTape6 AG.dll`, `Density AG.dll`, …
   each with a small `.cfg` beside it.
3. Add **only** the output folder to your host's VST2 scan path (keep the original
   Airwindows folder out of the scan path so you don't see duplicates), or use
   `--copy` to bundle the originals into `<out>/_airwin` for a self-contained folder.
4. In your host, each wrapper shows up as `<Name> AG` with the plugin's normal
   controls plus extra parameters:

   - **AG Hold** — the wrapper always auto-matches: it continuously derives a
     BS.1770 **gated, integrated** dry-vs-wet loudness offset and applies it.
     Leave **AG Hold** off (the default) and it keeps adapting; set it above 50%
     to **freeze** the offset at its current value. The display reads
     "Auto ±x.x dB" / "Hold ±x.x dB".
   - **AG Window** — how much recent audio the rolling gated measure integrates,
     ~2 s to ~30 s. Shorter reacts faster; longer is steadier (and better for
     dynamics, where you want a representative span).
   - **AG Trim** — manual offset, ±12 dB, applied on top of the learned offset.
   - **In LUFS** / **Out LUFS** — read-only K-weighted loudness readouts: the dry
     input and the as-heard output after compensation. They should read roughly
     equal once matched. These are meters, not controls — they ignore writes and
     report `canBeAutomated = 0`. How often they visibly refresh depends on the
     host: DAWs that poll parameter displays while the device panel is open show a
     live number; others update only when they next re-read the parameter.

**How it behaves.** Because the offset is gated and integrated over the window, it
handles both static-character processing (EQ, saturation, consoles) and dynamics
(compressors, limiters) without pumping — a continuous *instantaneous* match would
fight a compressor's gain changes, but an integrated one settles on the right
long-term level. Use **AG Hold** to lock a value once you're happy with it (e.g.
after playing a representative section), and the held offset is saved with the
project.

**Boost is limited.** The match attenuates freely but boosts at most +12 dB, and
it ducks faster than it lifts. This keeps it from "restoring" the loudness of
plugins that legitimately remove broadband energy — lowpass/highpass filters, band
cuts, mid/side and mono utilities, dither — where loudness matching isn't
meaningful. On those, Hold near 0 dB or just use Trim.

Set `AWDEBUG=1` in the environment before launching the host to get a
once-per-second readout in the log (see below) of dry/wet RMS, the computed
match, and the gain actually applied — handy for confirming matching is active.

## Building from source

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Produces `awautogain.{dll,so,vst}` (the shim template) and `airwrap` (the generator).
On Windows build 64-bit to match Airwindows' 64-bit VST2s.

## How it works

`airwrap` copies the single compiled shim once per plugin and writes a sidecar
`.cfg` (`target=`, `name=`, `id=`). At load the shim reads its own filename, finds
the matching `.cfg`, `LoadLibrary`s the real plugin, and returns an `AEffect` that:

- copies the inner plugin's I/O counts, flags and version verbatim;
- forwards `dispatcher`, `get/setParameter`, both `processReplacing` and
  `processDoubleReplacing` (Airwindows' double-precision path is preserved);
- translates the `audioMaster` callback pointer so host automation still resolves
  to the wrapper, not the hidden inner plugin;
- appends the three AG parameters and reports `numParams = inner + 3`;
- wraps the state chunk (`AGW1` header) so the AG settings survive save/reload —
  Airwindows set `programsAreChunks(true)`, so without this the extra params would
  be silently dropped;
- measures dry vs. wet mean-square per block, integrates with a one-pole filter,
  and applies a per-sample-smoothed gain to match levels.

Set `AWDEBUG=1` in the environment to get load diagnostics on stderr.

## Caveats worth knowing

- **Dynamics processors.** Because the offset is gated and integrated over the
  window rather than instantaneous, it settles on the long-term level without
  fighting a compressor's own gain changes. For these, a longer **AG Window** (and
  **Hold** once it's settled) gives the steadiest result.
- **Loudness, not flat RMS.** Matching uses ITU-R BS.1770 K-weighting (the same
  weighting LUFS meters use), so it tracks perceived loudness. This matters for
  saturators, tape, and consoles whose harmonics read louder than their flat RMS
  suggests; a plain-RMS match would leave those a couple of dB hot.
- **Latency alignment.** Dry is measured before the inner plugin, wet after. For the
  rare Airwindows plugin reporting latency this is unaligned within the integration
  window; with the default slow window it's negligible. (A dry delay line matching
  `initialDelay` would tighten it — easy to add if you hit a case that needs it.)
- **Legal.** VST2 is Steinberg's deprecated format; this is a personal-use utility
  built against a clean-room ABI header (no Steinberg SDK source). "VST" is a
  Steinberg trademark.

Airwindows is MIT-licensed work by Chris Johnson. This wrapper is independent and
does not include or modify Airwindows source.
