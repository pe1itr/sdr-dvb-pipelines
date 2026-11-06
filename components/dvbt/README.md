# DVB-T receiver (rbdvbt)

This is the actively maintained DVB-T component of `linrad_dvb`. Develop the
receiver directly in this directory. The former `dvbt-rx` repository is no
longer the development source; no checkout, installed `rbdvbt_rx` executable or
download from that repository is needed.

## License

Copyright (c) 2026 Rob Hardenberg / PE1ITR.
This component is licensed under GNU GPL version 3 only (`GPL-3.0-only`),
as part of Linrad DVB. See [LICENSE](LICENSE) and the project [NOTICE](../../NOTICE).
This replaces the custom attribution license for the current version;
previously distributed versions retain their original licensing terms.

## Source layout and integration

- `include/`: receiver configuration and C interfaces (`rbdvbt_*`).
- `src/config.c`, `src/iq_input.c`, `src/receiver.c`, `src/pipeline.c`: input,
  configuration and receiver orchestration.
- `src/probe_constellation.c`: acquisition, OFDM, equalization, demapping and
  streaming Viterbi decoding.
- `src/dvbt_2k_model.c`: DVB-T 2K carriers, pilots and interleaving model.
- `src/dvbt_outer.c`: outer FEC, TS validation and output gating.
- `src/udp_ts_output.c`, `src/visualizer.c`: TS and diagnostic output.

The application adapter is [`../../src/dvbt_backend.c`](../../src/dvbt_backend.c).
It supplies the entry point used by `linrad_dvb --decoder-worker=dvbt` and
connects receiver telemetry to `src/decoder_monitor.h`. Keep GTK code and
application process management in the host. `LINRAD_EMBEDDED_DVBT` guards the
monitor hooks; no X11 decoder windows are compiled in. The original `src/main.c`
is retained but excluded from the integrated build.

From the repository root:

```sh
make all
make test-dvbt
./build/linrad_dvb --decoder-worker=dvbt --help
```

The component uses FFTW3 single precision, pthreads and libm. The full
application's dependencies are checked by `bash tools/build.sh --check`.
[`test_dvbt_stream.c`](../../tests/test_dvbt_stream.c) covers streaming Viterbi
puncturing, errors, chunk continuity and reset behavior;
[`test_dvbt_psi.c`](../../tests/test_dvbt_psi.c) covers PAT/PMT bootstrap and
continued delivery. Run `make test` for host integration and replay a known
signal for changes to acquisition, demodulation or FEC.

## Rotation/FFT reuse in the decoder

The decoder now computes each rotated FFT once per pilot-alignment scan and
reuses that spectrum across bin, conjugation and symbol-phase hypotheses. The
cache belongs to that one scan, is freed on both success and failure, and never
survives a change of input or CFO. If allocation fails, the scan uses the original
uncached calculation. The live four-symbol cache needs 32 KiB (384 KiB for the
maximum 24-symbol offline scan). The formulas, scoring and tie ordering remain
unchanged.

Payload demapping also reuses the exact rotation already computed for power
normalization, using the existing per-symbol workspace. Non-finite cells still
produce zero soft-symbol erasures. This change does not apply the block sin/cos
approximation or the buffer-reuse experiment discussed below.

[`test_dvbt_frontend.c`](../../tests/test_dvbt_frontend.c), included in
`make test-dvbt`, verifies exact alignment/score equality against the uncached
allocation-failure path, the reduction in FFT calls, guard intervals 1/32, 1/16
and 1/8, changing input/CFO, partial and empty scans, tie ordering, and cache
cleanup. The existing FEC and PSI tests remain part of the same target.

The full 35/40 kHz test files were replayed at their original 250,000 samples/s,
twice per decoder version in alternating order. These are existing simulated
propagation files, each 40.01 seconds long. Median decoder CPU times:

| Input | Baseline CPU s | Integrated CPU s | Reduction |
| --- | ---: | ---: | ---: |
| 35 kHz FEC 2/3 (`oliYYC`) | 3.208 | 2.751 | 14.2% |
| 40 kHz FEC 1/2 (`OAeYaF`) | 3.001 | 2.580 | 14.0% |
| 40 kHz FEC 1/2 (`jubzGI`) | 4.883 | 3.948 | 19.2% |

Every narrowband run consumed all 10,002,500 samples with zero IQ overruns and
zero FEC queue drops. Baseline and integrated decoder produced identical TS
hashes and error counts per file: 47,000 / 94,564 / 35,344 TS bytes and
564 / 277 / 508 uncorrectable RS blocks respectively. The nonzero error counts
belong to the impaired inputs and are unchanged by this optimization.

The final-code replays on the ten-second 250/333 kHz references saved 18.1% and
11.3% decoder CPU respectively; the controlled noise/fading/drift case saved
14.0%. All 24 final-code comparison runs had matching TS hashes/error counts
within each case, complete input consumption and no IQ/FEC queue drops.
[Final integration measurements](benchmarks/step1-2026-11-05.json) retain the
input hashes, source/build identity and individual runs. These results concern
decoder CPU on this x86_64 host, not total application CPU or an ARM guarantee.
`make all` and `make test-dvbt` pass. An optional sanitizer build could not link
because the system's ASan runtime library is missing; no sanitizer result is
claimed.

For a live-rate comparison of the narrow files with the pre-change revision
(adjust the local recording paths if necessary):

```sh
make all test-dvbt
python3 - <<'PY'
import json
from pathlib import Path
report = json.loads(Path('components/dvbt/benchmarks/step1-2026-11-05.json').read_text())
Path('/tmp/dvbt-inputs.json').write_text(json.dumps(report['fixtures'][:3]))
PY
python3 dev/dvbt_frontend_experiment.py build /tmp/dvbt-step1 --baseline-ref 1069f21 --variants baseline,current
python3 dev/dvbt_frontend_experiment.py run /tmp/dvbt-step1 /tmp/dvbt-inputs.json --repeat 2 --variants baseline,current
```

`current` reads the working-tree source; `baseline` reads the specified Git
revision. Both link the same current backend objects, which were unchanged by
this optimization. The manifest specifies each file's actual input sample rate;
in particular, the 35/40 kHz test files use **250,000 samples/s**, not their RF
bandwidth, for live pacing.

## Viterbi SIMD investigation (2026-11-05)

The following records the **research phase** of the now integrated optimization.
The [experiment tool](../../dev/dvbt_viterbi_experiment.py) builds
the baseline from `3b9077b`, an AVX2 candidate, and a per-step comparison checker.
Both variants include the already integrated frontend optimization above.
The [report](benchmarks/viterbi-2026-11-05.json) records all 24 live runs,
input/output hashes, build commands, object identities and validation results.

The candidate computes eight Viterbi states at a time with AVX2 floating-point
operations and packs eight survivor decisions into one byte. It preserves the
original costs, addition order, tie decisions, scalar best-state reduction,
normalization, puncturing, traceback, resets and chunk boundaries. It adds no
batching or waiting. The kernel is restricted to one function; the surrounding
decoder retains the normal compiler flags. No integer quantization is used.

Measured on Ryzen 7 5800H with IQ supplied at its original sample rate. Each
case ran baseline/candidate/candidate/baseline. Values are median decoder CPU
seconds across all threads, excluding the feeder, GUI and video renderer:

| Input | IQ duration | Baseline CPU s | Candidate CPU s | Reduction |
| --- | ---: | ---: | ---: | ---: |
| 35 kHz, FEC 2/3, oliYYC | 40.01 s | 2.759 | 2.643 | 4.2% |
| 40 kHz, FEC 1/2, OAeYaF | 40.01 s | 2.579 | 2.473 | 4.1% |
| 40 kHz, FEC 1/2, jubzGI | 40.01 s | 4.066 | 3.968 | 2.4% |
| 250 kHz, FEC 1/2 | 10 s | 2.758 | 2.570 | 6.8% |
| 333 kHz, FEC 2/3 | 10 s | 2.599 | 2.252 | 13.3% |
| 250 kHz, noise/fading/drift | 10 s | 2.376 | 2.192 | 7.7% |

All runs consumed every input sample with zero IQ overruns and zero FEC queue
drops. TS bytes, TS hashes, Viterbi step counts and uncorrectable RS counts were
identical per input. First TS timing showed no added delay. The narrow recordings
were replayed in full at **250,000 samples/s**, taking about 40.1 seconds each;
the 250/333 kHz references used the same ten-second inputs as the frontend study.

Thread CPU timing around the complete Viterbi function shows a **2.08–2.28×**
speedup, including depuncturing, normalization, traceback and packing. Viterbi
accounted for approximately 13% of baseline decoder CPU on the clean 250 kHz
input and 28% on the 333 kHz input, explaining the different end-to-end gains.
The separate unpaced FEC 2/3 benchmark took 0.1971 versus 0.0938 seconds for one
million trellis steps (three runs per variant). That is not a live receiver
speedup claim. The existing sampled ACS wall-time breakdown is not used for
the CPU figures above.

The checker compares every float metric byte, survivor decision and selected
best state against the original loop at every step. Tests cover all five FECs
(1/2, 2/3, 3/4, 5/6, 7/8), independently encoded data, sparse errors, chunk
continuity, tails, resets, random floating-point costs, ties and erased bursts.
Recorded-IQ tests cover FEC 1/2 and 2/3; the other rates have synthetic coverage.

Reproduce on an AVX2 machine with the Git history and local recordings present:

```sh
make all
python3 dev/dvbt_viterbi_experiment.py build /tmp/dvbt-viterbi
python3 dev/dvbt_viterbi_experiment.py test /tmp/dvbt-viterbi
python3 dev/dvbt_viterbi_experiment.py kernel /tmp/dvbt-viterbi --repeat 3
python3 - <<'PY'
import json
from pathlib import Path
r = json.loads(Path('components/dvbt/benchmarks/viterbi-2026-11-05.json').read_text())
Path('/tmp/viterbi-cases.json').write_text(json.dumps(r['fixtures']))
PY
python3 dev/dvbt_viterbi_experiment.py run /tmp/dvbt-viterbi /tmp/viterbi-cases.json --repeat 2
```

Adjust recording paths in the manifest as needed. The controlled fading input
can be recreated with `dvbt_frontend_experiment.py impair` and a manifest
containing just the clean 250 kHz case; update its output path in the run manifest.
Both variants link the same current backend objects from `build/`.

The production decoder now selects AVX2 at runtime on supporting x86 CPUs,
NEON in AArch64 builds, and scalar otherwise. `LINRAD_DVBT_VITERBI_SCALAR=1`
forces the scalar route for comparison; this changes no buffering or receive
parameters. The selected kernel is logged once during table initialization.
There is no sensitivity improvement claim and no DVB-S change.

`make test-dvbt` tests independent encoded payloads for every FEC, errors,
chunk/reset handling, random soft values and ties. It runs automatic selection,
forced scalar and a build excluding all explicit SIMD kernels. On AArch64 it
also runs `test_viterbi_neon`: 100,000 independently checked ACS cases, including
signed zero and subnormal floats. The NEON kernel has passed those 6.4 million
metric checks under `qemu-aarch64 -cpu cortex-a53`; this verifies computations,
not physical N2 performance or a full native receiver build. The native build
and SDR throughput test on the ODROID-N2 remain to be performed there.

Integration checks and their limits are recorded in the
[integration report](benchmarks/viterbi-integration-2026-11-05.json).
The full x86 receiver was replayed with the fixed-clock source, including high
FECs, 60 seconds at 500 kHz/7/8, fading, the 35/40 kHz recordings and forced
scalar; outputs matched the saved baseline. See the root README's ODROID-N2
section for native build/test and scalar/NEON comparison commands.

### Higher FEC with a fixed SDR-like input clock

The earlier live IQ comparison used the regular file player, which can slow
down under backpressure. Acceptance for a live SDR requires input that keeps
arriving when processing stalls. The additional
[high-FEC report](benchmarks/viterbi-high-fec-2026-11-05.json) records **24 runs**
with this stricter source model, including FEC **3/4, 5/6 and 7/8**.

`dev/fixed_iq_source.py` releases 10 ms chunks against one monotonic sample
clock. Its 100 ms FIFO is bounded: overflow drops incoming samples and records
the loss; it never shifts the clock to accommodate the receiver. The kernel
pipe and the receiver's existing buffers also have finite capacity. The source
reports its peak FIFO occupancy, sample losses and scheduling lateness.
`tests/test_fixed_iq_source.py` verifies byte-exact normal delivery and explicit
loss when the reader is deliberately stalled for 300 ms.

No matching RF recordings were available. The independent `datv-transmitter`
`datv-iq` test encoder generated valid QPSK 2K, GI 1/32 IQ, with known counter
TS payloads and PAT/PMT. The 250 kHz cases use 1.25 Msamples/s and the 500 kHz
cases 2.5 Msamples/s. Generation and testing open no radio hardware. Every
received payload packet is checked against the known pattern as well as the
baseline output. Generated files and metadata are under the ignored
`build/diagnostics/viterbi-high-fec/` directory.

Total decoder CPU reductions at the fixed input rate:

| FEC | 250 kHz, 20 s | 500 kHz, 20 s |
| --- | ---: | ---: |
| 3/4 | 13.5% | 16.7% |
| 5/6 | 15.3% | 17.7% |
| 7/8 | 15.6% | 15.8% |

The 500 kHz high-FEC cases were run twice per variant in alternating order;
the 250 kHz cases once per variant. A 500 kHz/FEC 1/2 control also passed.
The complete FEC thread used 36.6–45.7% less CPU in the high-FEC clean cases.

For **60 continuous seconds at 500 kHz/FEC 7/8**, decoder CPU decreased from
23.762 to 19.544 seconds (**17.8%**), while FEC-thread CPU decreased from
11.251 to 6.921 seconds. Both variants returned the same 4,945,716 TS bytes.
The 20-second 7/8 noise/fading/drift case also returned identical TS and the
same 318 uncorrectable RS blocks, with every emitted payload packet intact.

Across all 24 runs:

- No source FIFO loss, receiver IQ overrun or FEC queue drop; every input sample
  was consumed, and all TS bytes and RS outcomes matched per case.
- Peak FEC queue occupancy was **64/1024 symbols (6.25%)** in both variants.
  This counts waiting symbols, excluding the job currently being processed.
  First-third and last-third queue medians remained 64 symbols, with no buildup.
- The slowest successful FEC job used at most 66.8% of its represented signal
  duration in the baseline and 60.0% in the candidate.
- Source completion exceeded the nominal signal duration by less than **1 ms**.
  Maximum individual source scheduling lateness was 5.42 ms. This is measured
  OS scheduling, not a hardware real-time clock guarantee.

The historical high-FEC problem involved falling back to repeated block
decoding; the baseline already contains the fix that enables the continuous
Viterbi path for all five rates. Both current baseline and SIMD candidate keep
up here. These results establish additional CPU headroom on this Ryzen 7 5800H,
not that the historical failure is still present or that every slower machine
will keep up. The longest test is 60 seconds; RF and longer soak tests remain
separate from this generated-IQ result. These measurements precede production
integration and measure the AVX2 prototype, not NEON.

Reproduce the files and fixed-clock comparison (NumPy and the independently
built transmitter fixture are required):

```sh
python3 tests/test_fixed_iq_source.py
python3 dev/dvbt_high_fec_fixtures.py /path/to/datv-transmitter/build/datv-iq build/diagnostics/viterbi-high-fec
python3 dev/dvbt_viterbi_experiment.py build /tmp/dvbt-viterbi
python3 - <<'PY'
import json
from pathlib import Path
p = Path('build/diagnostics/viterbi-high-fec/cases.json')
cases = json.loads(p.read_text())
for c in cases:
    c['repeat'] = 2 if (c['bandwidth'] == 500000 and c['nominal_duration_s'] == 20
                       and c['fec'] != '1/2' and c['expected_clean']) else 1
Path('/tmp/high-fec-sdr.json').write_text(json.dumps(cases))
PY
python3 dev/dvbt_viterbi_experiment.py run /tmp/dvbt-viterbi /tmp/high-fec-sdr.json --sdr-live --repeat 1
```

## Live-rate frontend investigation (2026-11-05)

The following records the earlier research phase, before integration above. The
[experiment tool](../../dev/dvbt_frontend_experiment.py) builds isolated copies
of this component, linked to the normal embedded backend objects. The
[measurements](benchmarks/frontend-2026-11-05.json) retain the 46 individual runs,
input hashes, build identity, limitations and per-phase summaries.

All recordings were supplied through `build/iq_file_to_stdout` at their original
**1,010,526 complex samples/s**. Each ten-second recording took approximately ten
seconds to replay. CPU below means decoder user + system CPU time, including its
threads, excluding the feeder, GTK and video player. This measures processing
headroom at live speed, not faster-than-live throughput or whole-application CPU.
Host: Ryzen 7 5800H, Linux x86_64, normal `-O3` build, baseline `1069f21`.

### 1. Avoid repeated rotation calculations

Two alternatives were measured. `rotations` factors the sin/cos calculations into
blocks of 64 samples and reuses the first payload rotation during demapping.
`cached` keeps the original arithmetic and instead reuses identical rotated FFT
results across acquisition hypotheses, together with the same payload reuse.
Hypothesis ordering and scoring remain unchanged. The live four-symbol FFT cache
occupies 32 KiB.

The preferred candidate is **`cached`**: it preserves the arithmetic and gave a
clear CPU reduction in each tested signal. Medians from its own paired baseline
phase are shown below; the clean recordings have three repetitions per variant,
and the impaired recording has two.

| Input (QPSK 2K, GI 1/32) | Baseline CPU s / 10 s | Cached CPU s / 10 s | Reduction |
| --- | ---: | ---: | ---: |
| 250 kHz, FEC 1/2 | 3.357 | 2.733 | 18.6% |
| 333 kHz, FEC 2/3 | 2.905 | 2.574 | 11.4% |
| 250 kHz, added noise/fading/drift | 2.703 | 2.327 | 13.9% |

For example, the first row is about 33.6% versus 27.3% of one CPU core over the
replay interval, not an 18.6 percentage-point reduction in system CPU usage.
The block-rotation alternative saved 15.7%, 11.3% and 11.6% respectively in its
matched comparisons. It changes floating-point rounding and is less attractive
than eliminating identical work.

For each input, all tested variants produced identical TS lengths and SHA256
hashes. The clean inputs produced 1,140 and 2,206 packets with zero uncorrectable
RS blocks. The impaired input produced 928 packets and 259 uncorrectable RS
blocks for every variant: partial reception, with no measured quality change.
It adds noise at 14 dB relative to the original average signal power, two 350 ms
amplitude fades to 0.1 and 3 Hz/s frequency drift. The generator is deterministic
and reproduced the original test fixture byte for byte, with no clipping.

The 22 cached/clean and impaired runs consumed all 10,105,260 samples each,
with zero cumulative IQ overruns and zero FEC queue drops. The earlier 24-run
phase predates the cumulative counter; its 18 candidate logs confirm complete
input consumption, while its six original baseline logs were overwritten by the
later phase. Those six retain saved CPU/TS results, but input-count verification
is unavailable. Missing counters are explicitly `null` in the saved results.

### 2. Reuse buffers and avoid copies

The `buffers` prototype reuses the OFDM input and prefix-merge buffers and borrows
the existing resampler output instead of allocating and copying it. It preserves
the asynchronous FEC queue's ownership. Measured CPU reductions were only 0.32%
and 0.18% on the clean inputs, and -0.02% on the impaired input. These differences
do not establish a useful improvement; do not prioritize integration for CPU
savings on this evidence. Combining buffers with block rotations only gave a
small additional difference, without establishing a separate benefit.

A separate FEC-job allocation profile measured 29 allocation/copy operations
plus frees at **1.65 ms total per ten-second recording**, below 0.1% of decoder
CPU time. A FEC buffer pool is therefore not justified by this CPU profile.
Peak-RSS measurements are retained but can include inherited process high-water
marks; they do not substantiate a memory-saving claim.

### Reproduction and limits

Use existing local recordings, or update the paths in the saved `fixtures`
entries. Recordings are not distributed with this repository and the old
`dvbt-rx` checkout is not a build dependency. From the repository root:

```sh
make all
python3 - <<'PY'
import json
from pathlib import Path
report = json.loads(Path('components/dvbt/benchmarks/frontend-2026-11-05.json').read_text())
Path('/tmp/dvbt-inputs.json').write_text(json.dumps(report['fixtures'][:2]))
PY
python3 dev/dvbt_frontend_experiment.py build /tmp/dvbt-front --baseline-ref 1069f21 --variants baseline,rotations,buffers,combined,cached,alloc-profile
python3 dev/dvbt_frontend_experiment.py run /tmp/dvbt-front /tmp/dvbt-inputs.json --repeat 3 --variants baseline,rotations,buffers,combined,cached
python3 dev/dvbt_frontend_experiment.py impair /tmp/dvbt-front /tmp/dvbt-inputs.json
python3 dev/dvbt_frontend_experiment.py run /tmp/dvbt-front /tmp/dvbt-front/controlled.json --repeat 2 --variants baseline,rotations,buffers,combined,cached
```

`impair` requires NumPy. Run variants sequentially without competing benchmarks.
Results are saved as `MANIFEST-results.json`, with TS and logs under
`MANIFEST-logs/`. Rerunning the same manifest overwrites that manifest's outputs;
use a new manifest name to retain a separate phase. Optional `offset_s` and
`duration_s` select recording segments. `--variants alloc-profile` measures FEC
allocation overhead separately; diagnostic logging makes it unsuitable as a
normal timing candidate.

An attempted 18-second real-fade segment produced no TS and lost input samples
even in the baseline. It is excluded from performance/quality comparisons.
Investigation also found that the existing per-read overrun delta can miss
overruns while OFDM is busy between reads. The experiment therefore logs the
cumulative ring-buffer counter at shutdown; production logging is unchanged.

`make test-dvbt` passes. This research covers short recordings on one x86_64 host,
QPSK 2K, GI 1/32 and FEC 1/2 or 2/3; it does not prove unchanged sensitivity at all
signal thresholds or a sustained whole-application saving. The integration above
adds cache ownership/fallback tests and further live-rate comparisons. Longer
SDR reception on the intended hardware remains useful validation. The separate
scratch-buffer prototype is not production-ready code and has not been adopted.

## Fixed carrier-role lookup and DX validation (2026-11-05)

`dvbt_2k_model.c` now computes the four fixed carrier layouts once during model
initialization. A role lookup reads one byte from a 4 x 1705 table (6,820 bytes),
instead of repeatedly searching the 17 TPS and 45 continual-pilot positions.
The original classification priority is preserved: TPS, scattered pilot,
continual pilot, then data. Carrier 1704 and out-of-range queries retain their
original handling.

This caches only the modulation's fixed carrier layout. Pilot phase, slope,
coherence, CFO, acquisition hypotheses, tracking cadence, normalization and
soft decisions are still computed from each incoming symbol exactly as before.
There is no extra smoothing, waiting, frame batching or reduced search range.
This distinction matters for weak-signal DX, rapid QSB and aircraft scatter.
The change is ordinary C and does not require SIMD; physical ODROID-N2/NEON
performance testing remains a separate task.

On the Ryzen 7 5800H, uninstrumented paired replays against `d45f7c8` measured
the following **decoder-process** user+system CPU times. Video, GUI and SRT CPU
are not included. Each row is one full A/B replay, on a fixed input clock:

| Recording | Signal duration | Before CPU s | After CPU s | Reduction |
| --- | ---: | ---: | ---: | ---: |
| 35 kHz, FEC 2/3 | 40.01 s | 2.682 | 1.763 | 34.3% |
| 40 kHz OAeYaF, FEC 1/2 | 40.01 s | 2.493 | 1.533 | 38.5% |
| 250 kHz, FEC 1/2 | 10 s | 2.583 | 1.664 | 35.6% |
| 333 kHz, FEC 2/3 | 10 s | 2.235 | 1.648 | 26.3% |
| 500 kHz, FEC 3/4 | 20 s | 6.047 | 4.858 | 19.6% |
| 500 kHz, FEC 5/6 | 20 s | 6.410 | 5.204 | 18.8% |
| 500 kHz, FEC 7/8 | 20 s | 6.892 | 5.674 | 17.7% |
| 500 kHz, FEC 7/8, noise/fading | 20 s | 7.096 | 5.935 | 16.4% |

The other 40 kHz recording (jubzGI) was compared in the instrumented profiling
phase: identical 188 TS packets and 508 uncorrectable RS blocks; decoder CPU
3.967 to 2.366 seconds. Keep this profiling comparison separate from the table.

Additional 40 kHz DX replays each used two A/B repetitions in reversed order.
At a median reported SNR of 6.02 dB, both versions delivered the same 332 TS
packets and 421 uncorrectable RS blocks, with 34.7% less decoder CPU. At median
5.31 dB they delivered the same 253 packets; at 4.70 dB, the same 67 packets.
These are partial-reception recordings with varying SNR, not measurements of
a new decoding threshold. The light-Doppler case had median reported SNR
5.52 dB and identical 258 packets with 28.9% less CPU. It used reacquisition
throughout, rather than uninterrupted tracking, in both versions.

The severe continuous QSB/echo case produced **zero TS in both versions**.
Its identical decisions only validate failure behavior, not usable reception.
In the separate recovery fixture, QSB/echo ends at 24 seconds: both versions
produce their first TS in the same subsequent signal block (about 29.8 seconds
after playback starts), with identical 78 packets and 171 uncorrectable RS
blocks. The optimization preserves that recovery behavior; it does not change
the block buffering or acquisition algorithm.

The [measurement report](benchmarks/pilot-roles-2026-11-05.json) retains build
commands and hashes, fixtures, SNR sequences, CPU measurements, queue/loss
counters and decision hashes. Validation comprises 48 fixed-clock replays:
six profiling runs, 38 A/B runs without detailed profiling, and four runs through
the built `linrad_dvb --decoder-worker=dvbt` (weak 40 kHz, light Doppler, 500 kHz
FEC 7/8, and QSB recovery). All consumed the full input without source, IQ or
FEC loss. Each comparison retained exact TS bytes, RS counts and signal
decisions. `make all test-dvbt` passed.

The investigation is reproducible with:

```sh
make all test-dvbt
python3 dev/dvbt_pilot_experiment.py build /tmp/dvbt-pilots
python3 dev/dvbt_pilot_experiment.py check /tmp/dvbt-pilots
python3 dev/dvbt_pilot_experiment.py run /tmp/dvbt-pilots CASES.json --repeat 1
python3 dev/dvbt_pilot_experiment.py run /tmp/dvbt-pilots CASES.json --variants profile,roles-profile --repeat 1
```

The baseline probe and carrier model are pinned to `d45f7c8`; the other backend
objects are taken from `build/` and their hashes are recorded. `check` compares
both the isolated prototype and the current production model with the pinned
original: 1,123,400 role lookups, 829,672 data indices, all four phases, frame
boundaries, uint32 symbol wraparound, invalid inputs and both interleavers.
`make test-dvbt` also checks initialization through role lookup, the 1512 data /
17 TPS / 176 pilot counts, data-index ordering and the pilot precedence cases.

Profiling uses thread CPU clocks, separate from the uninstrumented A/B runs.
Its counters are inclusive: pilot estimation is inside both alignment scans
and demodulation, and scan time is inside demodulation. Do not sum those nested
counters. The profile identifies pilot classification as a worthwhile target;
it does not justify replacing the resampler or FFT.

Every A/B replay uses `fixed_iq_source.py` at the original sample rate. Its
bounded FIFO drops samples on overflow rather than slowing the source. The
harness checks full input consumption, source/IQ/FEC losses, TS hashes, RS
failures, and hashes of the per-stage synchronization, continuity, SNR/CFO and
outer-decoder decisions independently of thread log ordering.

For additional file-only DX scenarios from a prepared s16 recording:

```sh
python3 dev/dvbt_dx_fixtures.py CASES.json 40k-OAeYaF /tmp/dvbt-dx
python3 dev/dvbt_pilot_experiment.py run /tmp/dvbt-pilots /tmp/dvbt-dx/dx.json --repeat 2
```

The generator records seeds, hashes and clipping counts. It covers increasing
AWGN, 4 Hz QSB with 80 ms deep fades, a 12 us delayed rotating path, a recovery
interval, and a separate light-Doppler case with a common -6 to +6 Hz sweep
plus an 8 Hz offset on the weaker reflected path. These are controlled stress
scenarios, not a calibrated aircraft-scatter channel model. Added-noise values
refer to sampled wideband IQ power, not the decoder's reported SNR or calibrated
RF C/N. The reported SNR and actual decoded output are recorded separately.

## Origin and retained changes

Original author: **Rob Hardenberg / PE1ITR**. See [LICENSE](LICENSE).

Originally imported from the local `dvbt-rx` repository, version 0.1.5,
commit `fd9a44773f0d72bae6a569570636b36dad575233`, imported 2026-10-28.
The source checkout was clean when imported. The import revision and the
historical `RBDVBT_VERSION` value identify the origin; subsequent changes belong
to this project's history. On 2026-11-05 the existing source tree was promoted
from `third_party/rbdvbt` to this project component without replacing its code.

Changes already present at that migration in `probe_constellation.c` and
`dvbt_outer.c`:

- Hooks guarded by `LINRAD_EMBEDDED_DVBT` publish constellation, receiver status,
  and actual OFDM/FEC queue occupancy through the host's nonblocking monitor.
- IQ reader and FEC worker get descriptive Linux thread names.
- FEC 3/4, 5/6 and 7/8 use the existing continuous Viterbi path, with regression coverage for
  puncturing, byte identity, injected bit errors, chunk continuity and resets.
  Each 1512-cell OFDM symbol contains complete puncturing groups for every rate
  (1, 2, 3 or 4 dibits per group).
- The live TS output gate forwards repeated PAT/PMT packets after startup.
  Previously it suppressed these tables for the rest of reception, preventing
  players from discovering the service after a program switch or a late join.
  Only the PSI packet already emitted in the opening bootstrap is skipped;
  later packets, including section continuations, retain their original bytes.
  `test_dvbt_psi` covers startup and subsequent table delivery.

Keep the license with source and binary distributions. See the
[component development guide](../README.md) for the shared workflow.
