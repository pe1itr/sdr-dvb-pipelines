# DVB-T2 broadcast component

Separate receiver for 8 MHz DVB-T2 broadcasts, using CPU FEC by default. It does not change the
narrowband DVB-T or DVB-S/S2 receivers. Narrowband DVB-T2 is not supported by
the host adapter.

The initial source is the DVB-T2 core from
[OnAir](https://github.com/adswill/OnAir), revision
`25034214fa91adcdfd7ec843c51f9d79285472d8`. `UPSTREAM.json` records the original
file hashes and local changes. The source is maintained here from this point;
updating it requires review and regression testing. Its namespace remains
`dect2`. The GPL-3.0 license is preserved in `LICENSE`; this component retains
its upstream license and attribution within the GPLv3 application.

The imported P1 table source is also acknowledged: `src/t2/t2_tables.cpp`
identifies [gr-dvbt2](https://github.com/drmpeg/gr-dvbt2), by Ron Economos / W6RZ
(Copyright 2014 Ron Economos, GPL-3.0-or-later). The inherited source and its
unrecorded original revision are documented in `UPSTREAM.json`; the pinned
OnAir import and original hashes are retained. The Linux DVB-S2 GPU helper
also reuses this component's LDPC tables/decoder and Vulkan backend; its
S2 protocol and CPU fallback are local application integration.

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
dispatch selects the available CPU implementation.

## Optional GPU LDPC (experimental, Windows and Linux)

The mode dialog has **Use GPU for DVB-T2 LDPC**. It is off by default and is
saved as `[dvbt2] gpu_ldpc=false/true`. The selection takes effect at the next
receiver start. CPU mode does not load GPU libraries, probe adapters or compile
shaders. The CLI worker accepts `--ldpc cpu|gpu`; omission means CPU.

Windows GPU mode uses Direct3D 11 compute, imported from the same pinned OnAir
revision. It loads the Windows runtime dynamically, checks feature level 11,
compiles the shaders and tests known normal and short codewords. Initialization
failure retains CPU decoding. A failed GPU call is redone on CPU and disables
GPU use for that receiver run. Linux uses Vulkan compute with bundled SPIR-V
shaders (the GLSL sources and regeneration script are included). It dynamically
loads the Vulkan loader, selects a compute-capable GPU and runs the same T2
startup self-tests. Other platforms retain CPU fallback. Video rendering has
its own setting.

Only first-pass data-PLP LDPC runs on GPU; signalling, demapping, BCH and the
existing paced retries remain on CPU. The GPU receives already available
codewords without waiting for additional frames: at most 64 per Linux Vulkan
dispatch and 16 per Windows Direct3D dispatch, subject to device limits.
Queues remain bounded. Service Info and HTTP `decoder.ldpc_backend` report the
last completed frame's first-pass route (`waiting`, `cpu`, `gpu`). Cumulative
`ldpc_gpu_frames` and `ldpc_cpu_frames` count PLP jobs, not codewords; a GPU
frame can still need CPU retries. `ldpc_gpu_requested` records the selection.
The profile stages `t2.gpu-ldpc` and `t2.gpu-readback-wait` measure host CPU/wall
time, not GPU execution time; sum their exclusive times for GPU-call latency.
On Linux, `DECT2_GPU_TIMING=1` additionally reports cumulative GPU timestamp
duration and total host latency. Device duration includes the kernel's accesses
to mapped input/output buffers; it does not separate pure arithmetic from data
transfer. Profiling is optional and does not enable GPU decoding.

The Linux Vulkan kernel keeps FP32 arithmetic and the CPU's layered update
order. It stores scaled minima and packed signs/index instead of one float per
edge, specializes temporary arrays by check degree, and caches a bounded prefix
of posteriors in workgroup shared memory. The iteration limit, normalization,
BCH and CPU retry policy are unchanged. `DECT2_VULKAN_SCHEDULE=layered` selects
the previous kernel for A/B testing; the default is the compact cached kernel.
Regenerate its bundled SPIR-V with `python3 components/dvbt2/tools/gen_cached_spirv.py`
(requires libshaderc on the developer machine only).

Design references: [cuLDPC](https://github.com/robertwgh/cuLDPC) and
[Liu and Zhao, 2024](https://doi.org/10.1186/s13634-024-01190-8).
The latter studies 8-bit 5G/WiMAX codes on other GPUs; its throughput is not a
DVB-T2 prediction. Our implementation retains FP32. A multi-workgroup flooding
prototype was also measured, but the compact layered kernel was faster in the
heavy test and preserves the existing decoding schedule. Research artifacts
and measurements are in `build/t2-bench/gpu-parallel-20261109/`.
The final report is `benchmarks/dvbt2-gpu-2026-11-09.json`: on the Linux
RTX 3050, the new GPU kernel takes about 234 ms versus 535 ms previously for
183 normal 3/5 codewords at 50 iterations. Automatic-worker CPU decoding
takes about 184 ms in that same synthetic workload. Thus the GPU kernel
improves, but does not beat this CPU in the component test. A 3-second IQ
replay at 10 MS/s preserves all 46,257 TS packets and the reference hash with
zero input/FEC queue drops. Later short Linux live observations reached about
19,443 TS packets/s with GPU LDPC and the internal player, without new input
or FEC drops over 57 seconds. With the external GPU player, a preceding
43-second interval had 24 FEC-frame drops and visible stuttering. The cause
is still open. The [GPU/player investigation](../../docs/dvbt2-gpu-player-investigation-20261109.txt)
records the evidence and follow-up goal: make GPU LDPC and GPU video work
smoothly together. Sustained, controlled coexistence validation remains open.

`linrad_dvb.exe --decoder-worker=dvbt2 --gpu-check` probes the device and runs
the startup self-test without IQ input. Exit 0 means a usable, self-tested GPU;
exit 1 means CPU fallback. `test_dvbt2_gpu.exe`, built with `--tests`, compares
GPU output, convergence and iterations against the CPU for 14 rates and
normal/short frames, noise, ties and batch boundaries. Exit 77 is an explicit
skip, not a successful GPU test. Live GPU throughput and coexistence with GPU playback require validation on
each target GPU. Component or Wine timings are not native Windows speedups.

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

Known IQ gaps invalidate the P1 interval used for sample-clock tracking. The
predicted P1 cadence is retained for acquisition, but clock updates resume only
after two real P1 detections without an intervening gap. Otherwise repeated
resampler resets can accumulate a false clock correction and corrupt P2/L1
before LDPC. `test_dvbt2_gap_clock` reproduces this with a zero-clock-error
synthetic signal and an absent PLP (no LDPC work). This fixes recovery after
loss; it does not prevent the original input queue overflow.

The data stage retains its cell/noise buffers across frames and writes every
position, including erased symbols, before submitting a PLP. PLP assembly
copies the selected range directly from gathered P2 cells and data cells into
the job's own storage, avoiding a full concatenated intermediate stream.
Pilot observations are built once, after phase correction; the previous
uncorrected observations were discarded without being used.

Detailed `t2.ce.*` and `t2.eq.*` profile stages separate phase correction,
pilot/noise fitting, interpolation, cell processing and PLP submission.
All stage times are exclusive: include these children when comparing with
older `t2.chanest+eq` / `t2.eqloop` totals. Profiling remains optional.
`test_dvbt2_pipeline` also checks 42,318 PLP ranges against the original
concatenate-then-slice method, including P2/data boundaries, noise values,
reused output storage, invalid ranges and independent ownership.

On x86 with AVX2/FMA support, clock correction uses a runtime-selected AVX2
kernel for two output samples at a time. The 24 taps, 512 phases, sample
positions and generic floating-point accumulation order are preserved; the
kernel deliberately uses separate multiplication and addition. Other x86
processors retain the baseline implementation, and ARM retains its NEON path.
Startup logs `DVB-T2 clock correction: avx2` when selected. For an x86 A/B
comparison, set `DECT2_CLOCK_GENERIC=1` before starting the process to select
the original path. The streaming regression compares exact output checksums
across clock changes, small input blocks and history resets on Linux and
Windows. This optimization does not change the clock recovery loop itself.

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
the independent full-layer decoder: 1008 cases across all 14 normal/short code
rates, noise, erasures, the actual retry alphas 0.66/0.78/0.9 and iteration limits
1/2/50/150, plus tied magnitudes, signed zero, tiny values and the minimum
accumulator bound at limits 1/2/7. Failures are compared too. The same cases
also exercise a build with the explicit SIMD dispatch disabled. The original
504-case measurements and recording hashes
are in `benchmarks/dvbt2-ldpc-2026-11-05.json` at the repository root.

The 2026-11-09 AVX2 update removes the connection-index accumulator from both
LDPC layer kernels. The second minimum is updated as `min(m2, max(m1, a))`;
message generation selects it when the input magnitude equals `m1`. If
several inputs tie, `m2 == m1`, so the outgoing magnitude remains identical.
This preserves the finite-LLR arithmetic, layered update order, iteration
limits and retry policy. Startup reports `DVB-T2 LDPC: avx2 value-minima`.
Generic and NEON kernels retain their previous implementation.

Short alternating component tests at normal code rate 3/5 on Ryzen 7 5800H
measured 11-15% lower decoder time for Linux and 6-11% for the Windows binary
under Wine, covering examples that take 1, 6 and 50 iterations. These are
not native Windows 5500H live measurements or whole-receiver speedups.
Details: `benchmarks/dvbt2-ldpc-2026-11-09.json`.

To repeat with a local headerless recording:

```sh
make build/dvbt2_worker
python3 dev/bench_dvbt2.py --decoder build/dvbt2_worker --iq FILE --output build/t2-bench/run1
```

Use a fresh output prefix for each run. The helper
uses a bounded, fixed-clock source and records TS hashes and all drop counters;
`--profile` additionally reports component CPU stages. The optional NumPy tool
`dev/dvbt2_impair_fixture.py` makes reproducible noise/fade recordings offline.
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

On Linux use `./build/linrad_dvb --decoder-worker=dvbt2 --gpu-check`.
The optional extended correctness test is `./build/test_dvbt2_gpu`; stop live
reception before running it. `DECT2_GPU_ADAPTER` can select an adapter index
from the startup log when more than one GPU is present. No GPU is probed in
CPU mode. `DECT2_NOGPU=1` forces unavailable-GPU fallback for testing.

Validation on the local RTX 3050: the earlier 882 cases matched CPU decisions,
convergence and iteration counts on Linux Vulkan and Windows Direct3D under
Wine/DXVK. The new Linux compact kernel passes 1,142 cases, including the
64-codeword batch boundary, mixed noise and signed zeros. Correctness tests
are not throughput claims; native Windows validation is separate.
GPU context/shader initialization can briefly fill the live input queue at
startup. The recorded-IQ correctness comparison waits for backend readiness;
startup loss from a first attempt is retained separately in the validation
artifacts. Use interval deltas after acquisition for live comparisons.
