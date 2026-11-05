# DVB-T2 broadcast component

Separate CPU receiver for 8 MHz DVB-T2 broadcasts. It does not change the
narrowband DVB-T or DVB-S/S2 receivers. Narrowband DVB-T2 is not supported by
the host adapter.

The initial source is the DVB-T2 core from
[OnAir](https://github.com/adswill/OnAir), revision
`25034214fa91adcdfd7ec843c51f9d79285472d8`. `UPSTREAM.json` records the original
file hashes and local changes. The source is maintained here from this point;
updating it requires review and regression testing. Its namespace remains
`dect2`. The GPL-3.0 license is preserved in `LICENSE`; the root MIT notice does
not relicense this component or the resulting combined executable.

## Boundaries

* Component: resampling, P1/P2 acquisition, L1 signalling, OFDM/equalization,
  cell/time deinterleaving, LDPC/BCH and BBFRAME-to-MPEG-TS conversion.
* `src/dvbt2_backend.cc`: stdin worker, bounded live input, TS stdout and
  decoder telemetry. It runs as `linrad_dvb --decoder-worker=dvbt2`.
* `src/linrad_dvb.c`: mode/PLP selection, process supervision and hardware input.
  Existing TS service selection and VLC playback handle the decoded stream.
* `t2gen.cpp`: file-only synthetic fixture generator, linked only into tests.
  No transmit functionality is included.

The adapter accepts 8..20 Msps u8, native little-endian s16 or cf32 IQ; the GUI
uses s16/u8 from hardware. Start with **10 Msps** for an 8 MHz channel. The core
resamples to 64/7 Msps. ADC rate, occupied bandwidth and symbol rate are distinct.
The optional IQ bandpass must span at least 8 MHz. Enabling it in DVB-T2 mode
raises a saved narrowband setting to 8 MHz; live narrowing below that is blocked.
The filter also requires room within 95% of the sample rate, so use 10 Msps
or leave it off. Filtering is optional and does not reduce the sample rate.
The 13.038-second Eindhoven recording was also replayed through the production
IQ file helper with an 8 MHz filter at 10 Msps, with the live receiver paused
to avoid competing decoder load: 238,496 TS packets, BCH 9,284/232, no input
or FEC-frame drops, 13.038 seconds source elapsed. Unfiltered replay produced
239,006 packets, BCH 9,304/212. The extra failures occurred during acquisition;
the filtered error count then stayed constant. A separately prefiltered replay
gave the same 238,496 packets. Filtering is therefore usable on this setup,
but it is not a reception improvement for this recording; leave it off unless
needed. Initial tests alongside the running live decoder overloaded the replay
and are not used to assess filter quality.
Actual hardware/filter bandwidth still needs to cover the channel at lower
sample rates. There is no global AVX2 build requirement: the imported runtime
dispatch selects the available CPU implementation. GPU decoding is omitted.

FEC, QPSK/16/64/256-QAM, FFT and guard interval are read from L1. PLP -1 selects
the first type-1 data PLP; 0..255 requests that ID explicitly. A missing explicit
ID produces no TS instead of silently falling back. Available PLPs and
unsupported parameters are logged. Inter-frame time interleaving, sub-sliced
type-2 PLPs, MISO and generic streams are not supported; FEF frames are skipped.
Decodability of a particular broadcaster must therefore be measured.

## Live input and losses

The stdin reader continues consuming samples independently of the decoder.
Its queue holds at most 64 blocks of 65,536 samples (about 419 ms at 10 Msps),
allowing for the burst of work when a long broadcast frame completes.
Overflow drops incoming whole samples; absolute input positions let the decoder
call `markGap()` at the correct boundary. FEC uses its own bounded queue. The
GUI/logs report both lost input samples and lost FEC frames. These counters
exclude losses upstream in USB or the SDR helper, which reports its own losses.
The decoder does not slow an SDR's sample clock. File tests must be clocked;
feeding a whole recording at filesystem speed deliberately exercises overload.

Partial reads are retained across sample boundaries; truncated samples and
nonfinite float samples fail the worker. EOF drains submitted FEC work, with a
10-second timeout, and flushes TS without fabricating additional IQ. Incomplete
final OFDM frames cannot be decoded. BB fragments are discarded across missing
frames/FEC blocks, preserving TS packet boundaries.

## Build and validation

The normal root build includes this component. It additionally needs a C++20
compiler and Rust/Cargo 1.70 or newer. All Rust dependencies are bundled locally;
`cargo build --offline --locked` does not fetch crates. `CARGO` can select a
different Cargo executable, with its matching `rustc` on PATH. Objects and Rust
artifacts are under `build/components/dvbt2`, not inside the source component.

```sh
make all
make test-dvbt2
make test-dvbt
```

`test-dvbt2` checks BCH/LDPC and L1 against known encoded bits, then feeds
synthetic 10 Msps recordings through the actual worker at a fixed live clock:
8K/16-QAM 2/3 s16 and 32K extended/256-QAM 3/5 u8, with time interleaving,
PLP 7 and a 125 Hz frequency offset. Every output TS packet is checked against
its known payload, including order/continuity after acquisition. It also tests
automatic/explicit/absent PLPs, invalid settings, truncated samples, EOF and
burst overload. The fixtures and receiver share an origin; these tests do not
replace independent off-air validation or measure weak-signal thresholds.

Initial Ryzen 7 5800H results: 38,192 exact packets (8K), 83,475 (32K), no input
or FEC-frame drops at live speed. Burst input correctly reported overflow.
Physical ODROID-N2/NEON testing is deferred.

Off-air trial in Eindhoven (2026-11-05), B210 Kintex-7, USB 3, channel A TX/RX,
small indoor antenna: 570 MHz/10 Msps locked to PLP 0, 32K, 256-QAM 3/5,
GI 1/8. At 50 dB gain the reported SNR was about 15.5..16.6 dB and RMS about
-22 dBFS; an 18-second measurement yielded only two correct BCH blocks and
50 TS packets, so that position did not provide usable continuous television.
After repositioning the antenna and selecting 52 dB gain, the user obtained
video; a 24-second observation at 26.4..26.9 dB reported SNR added 18,300 good
BCH blocks and 470,119 TS packets with no new BCH failures, input losses or
FEC-frame losses. This is not a measured sensitivity threshold or a long-run
test. The first live adapter lost short IQ bursts during long-frame processing.
Its bounded queue was increased from 210 to 419 ms and per-block display copies
removed before this observation and the benchmarks below.

## LDPC optimization measurements

The AVX2 decoder also processes layers with repeated information groups using
register-held min-sum state. All variable-to-check values are gathered first;
posterior updates keep the original connection order, including additions to
shared variables. This avoids repeated state-array traffic while preserving
decisions and rounding. Runtime dispatch requires AVX2 and FMA; the existing
generic/NEON paths, iteration limits and retry policy are unchanged.

On the Ryzen 7 5800H, a 13.038-second off-air recording from the good antenna
position was replayed at a fixed 10 Msps clock. Alternating baseline/candidate
runs measured the standalone production worker with profiling disabled:

| Recording | Runs per version | Median CPU seconds, before → after | Reduction |
| --- | ---: | ---: | ---: |
| Original broadcast | 3 | 48.51 → 30.18 | 37.8% |
| Added noise, 24 dB source/added-noise ratio | 2 | 73.73 → 36.64 | 50.3% |
| Deep fade and 0.5 Hz/s frequency drift | 2 | 78.25 → 45.42 | 41.9% |

The original and added-noise cases produced byte-identical TS in every run,
respectively 239,006 and 238,193 packets, with no source, input or FEC queue
losses. CPU seconds sum work across threads and exclude the IQ source, GUI and
video: the original recording uses about 3.72 → 2.31 CPU cores on average.
Noise was added digitally; these ratios are not RF sensitivity measurements.

After restarting the live B210 receiver with the optimized executable, a
32-second observation at 25.0..26.9 dB SNR added 24,156 good BCH blocks and
620,557 TS packets. BCH errors, continuity errors, lost IQ samples and lost FEC
frames all remained zero during that observation (570 MHz, gain 52 dB).

The deep fade attenuates the signal to 8% for two seconds, with added noise.
Both versions recover, but both show input and/or FEC queue losses during this
stress case. Baseline produced 191,354 packets; candidate produced 191,533 and
196,340. Output differs because live deadlines, drops and reacquisition depend
on timing. This test does **not** establish loss-free deep-fade operation; it
remains an area for further live scheduling investigation.

`test_dvbt2_ldpc` compares all hard bits, convergence and iteration counts with
the independent full-layer decoder: 504 cases across all 14 normal/short code
rates, noise, erasures, the actual retry alphas 0.66/0.78/0.9 and iteration limits
1/2/50/150. Failures are compared too. The same cases also exercise a build
with the explicit SIMD dispatch disabled. Full measurements and recording hashes
are in `benchmarks/dvbt2-ldpc-2026-11-05.json` at the repository root.

To repeat with a local headerless recording:

```sh
make build/dvbt2_worker
python3 tools/bench_dvbt2.py --decoder build/dvbt2_worker --iq FILE --output build/t2-bench/run1
```

Use a fresh output prefix for each run. The helper
uses a bounded, fixed-clock source and records TS hashes and all drop counters;
`--profile` additionally reports component CPU stages. The optional NumPy tool
`tools/dvbt2_impair_fixture.py` makes reproducible noise/fade recordings offline.
Physical ARM/NEON and long-duration broadcast testing remain separate work.

## Eindhoven reception

Use the root `linrad-dvbt2-eindhoven.example.ini`: 570 MHz (UHF channel 33),
8 MHz bandwidth, 10 Msps, B210 by default, or `--source hackrf`.
The preset has low gain as a starting point; choose the antenna connector and
gain for the connected receiver. For a Kintex-7 B210 clone, keep using the
correct board-specific firmware/image directory in Input settings.

KPN's [frequency table](https://community.kpn.com/kennisbank-digitenne-138/digitenne-frequenties-transportstreams-en-regionale-omroepen-470554)
lists Eindhoven RTS on channel 33, with Omroep Brabant and L1.
[NPO](https://npo.nl/service/npo-en-zenders) confirms NPO 1/2/3 and regional
public channels are free to air. Commercial Digitenne bouquets need a
subscription and are outside this component's scope. HEVC-capable VLC codecs
are needed to display the broadcast video.
