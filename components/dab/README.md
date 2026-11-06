# DAB component

Mode-I DAB reception is maintained here independently of the DVB decoders.
The initial import is the project's dab-monitor revision
`ec7ced2e9619eab2c55c97ca59aaea9aa2aef4f5`; see [ORIGIN](ORIGIN).
The neighbouring repository is not a build or runtime dependency.

`src/` contains the independent FIC/TII decoder, PRS candidate search,
incremental identity accumulator, channel filters, DAB+ radio session,
dynamic-label parser and MOT slideshow reassembler. The host adapters are:

- `../../src/dab_worker.cpp`: IQ input, continuous radio audio, bounded DX
  capture queue and private state/control files.
- `../../src/dab_view.c`: GTK ensemble, stations, slideshow and TII popup.
- `../../src/b210_mixer.h`: B210 physical-port mapping and conducted LO output,
  called by the existing SDR input helper.
- `channels.h`: ordered Band III blocks 5A through 13F, matching dab-monitor.

The root Makefile builds `build/dab_worker` entirely from source. With FAAD2
and ALSA development packages it enables DAB+ audio. `libdxdabsdr` now handles
streaming OFDM/DQPSK, EEP-A/B depuncturing, soft Viterbi decoding, time
deinterleaving, energy dispersal, Reed-Solomon correction, DAB+ superframes
and PAD transport. The AAC codec itself is the external FAAD2 library.
The former libdabsdr binary adapter and bundled binaries are removed.
Without FAAD2/ALSA the independent FIC/TII path remains available.

This first radio implementation supports Mode I, EEP-A/B and unencrypted
mono/stereo DAB+ services up to 192 kbit/s. Classic DAB MPEG Layer II, UEP,
surround audio and seamless ensemble reconfiguration are not implemented.
Reconfiguration pauses audio and rebuilds routing from current FIGs. PAD
supports DLS/DL+ and the legacy MOT application types 12/13; arbitrary
FIG 0/13 application-type assignments are not yet supported. Windows DAB
reception is not enabled by the Windows build helper.

Protocol references are pinned to ETSI EN 300 401 V2.1.1 and TS 102 563 V2.1.1
(2017-01), with clauses/tables beside the implementation. See [ORIGIN](ORIGIN)
for source URLs and the distinction between protocol facts, original receiver
choices and reused project/third-party components. These references document
provenance; they do not establish a legal clean-room process or improved
sensitivity. The new continuous radio does not yet use the DX path's 2D fallback.

## Reception profiles

All profiles use 2.048 MS/s internally. Input accepts s16/u8 IQ from 2.048 MS/s
up to the selected hardware limit (B210 reception uses at least 4 MS/s).
The host's `dab_resampler.hpp` first applies the component's anti-alias FIR
and integer decimation. Its symmetric FIR uses SIMD accumulation. For an exact
small rational ratio, the host then uses OnAir's generic polyphase DSP utility
already vendored under `components/dvbt2`; it links only the resampler and DSP
support objects, not the DVB-T2 receiver or FEC. Unusual ratios retain the
libsamplerate conversion so a rounded rate cannot introduce cumulative drift. This supports rates such as 2.5, 4, 8, 10 and 20 MS/s. Spectrum monitoring
uses the full original rate; B210 RX bandwidth follows that rate.
At 2.048 MS/s the additional channel filter is optional.
Ordinary DVB narrowband filters must be disabled or wide enough for DAB.

Normal reception uses the independent adaptive FIC decoder and conservative
TII settings alongside a continuous libdxdabsdr radio session. Weak/DX adds the
original PRS timing/carrier-offset/inversion fallback and bounded accumulation
of CRC-validated identity fragments. Aircraft scatter additionally enables
temporal PRS ranking and six overlapping 2.5-second subscans of each 8-second
capture, following the imported fallback policy. These options increase CPU
work; they do not establish a new sensitivity or aircraft-scatter performance
claim. Phase/window selection, the final non-iterative 2D fallback and TII
collision/ambiguity checks remain in the component.

The first discovered DAB+ station is selected automatically after each channel
change. An explicit station click takes priority and stays selected as service
metadata is refreshed. Radio audio and slides continue while the DX decoder processes captures.
There is one pending DX capture; overload replaces it with the latest complete
capture and counts the drop. It never concatenates fragments across dropped
captures. The radio retains its separately bounded IQ/audio queues. State
reports queue drops, the FIC route, observation time and discovered identities.
The GUI distinguishes radio ensemble information from independent DX findings.

TII codes are attributed only when one ensemble identity was recovered from
the capture. Multiple co-channel identities suppress that attribution.
Candidates and ambiguous Main-ID fits remain visible; only confirmed,
unambiguous codes enable the popup's Log button. A click snapshots the ensemble,
RF frequency, TII and observation time into the existing reception editor.
Main-window logging captures the ensemble without a TII code. Logs use the
existing CSV schema and ADIF application fields.

Channel changes keep the SDR stream and worker processes alive. A generation
number accompanies the source tune command. After discarding 50 ms of tuner
settling samples, the source acknowledges the exact byte boundary in its IQ
output stream. The worker drains older queued IQ, resets channel-specific DSP
and the audio session, and rejects DX/scope results from older generations.
The GUI rejects stale snapshots as well. No device reopening or process-stop
timeout is needed. Arrow buttons do not wrap at band edges. Station clicks
within an ensemble also keep the pipeline running.

A separate bounded diagnostic thread extracts DQPSK points from three-frame
captures. These feed the existing constellation panel. The existing Service
info grid displays measured CP/SNR, CFO, MER, IQ levels, locks and counters;
missing measurements remain blank. TII codes open from the spectrum button row.

## B210 fixed IF and LO

Direct RF reception is the default. Mixer mode tunes RX to `if_hz`, while the
application continues to show and log the DAB channel's RF frequency. Physical
ports map as follows:

| Connector | UHD channel | Antenna |
|---|---:|---|
| TRX1 | 0 | TX/RX |
| RX1 | 0 | RX2 |
| TRX2 | 1 | TX/RX |
| RX2 | 1 | RX2 |

The separate `lo_output_enabled` switch supplies a conducted low-side LO on
TRX1 or TRX2. The LO connector must differ from the receive connector. The
defaults copied from dab-monitor are 53 MHz IF, 0 dB TX gain, IQ amplitude
0.05, 8.192 MS/s TX, 6 MHz TX bandwidth and a +2.5 MHz digital tone. Thus the
TX synthesizer is `RF − IF − 2.5 MHz`, and the wanted mixer LO is `RF − IF`.
RX rate is configured before TX; coerced rates/frequencies are rejected.
Full-duplex startup conditioning traverses two neighbouring blocks and returns
to the requested block, discarding 250 ms at each step. Shutdown sends an end
of burst and joins the LO thread before closing UHD.

Connect the LO port to the shielded external mixer's LO input through the
attenuation/isolation network used with dab-monitor. Mixer mode with LO output
off supports an external LO generator. No firmware, FPGA image or EEPROM is
modified. This integration's tests do not activate RF hardware; device timing,
LO spectrum and conducted power still require hardware validation.

## Tests

```sh
make test-dab-core  # DAB unit tests without the GTK/Rust application build
make test-dab
make build/dab_gui_smoke
python3 tests/test_linrad.py LifecycleTests.test_dab_gui_controls
# Optional existing receive-only recordings, never required to build:
DAB_FIXTURE_DIR=/path/to/dab-monitor/artifacts/fixtures python3 tests/test_dab.py
```

Core tests exercise TII collisions/false IDs, partial identity isolation,
filter rejection, EEP profiles, Viterbi decoding, time deinterleaving, RS
correction, superframes, PAD CRC/fragment handling, FAAD2 configuration and
bounded radio queues. UHD mocks
verify physical ports, the requested synthesizer frequency, tone/amplitude,
coercion failure and end-of-burst cleanup without opening a device. GTK tests
exercise mode/channel controls, station commands, TII confirmation/logging,
reset, stop and retune using a synthetic IQ helper. Optional IQ regressions
check 9C identity/TII and reject the known false 4615 detection on 7C.

Validation of the new radio replayed the 11C fixture with 14 discovered
stations and decoded AAC audio. The 12-second paced regressions at 10 and
20 MS/s decoded more than 200 audio frames each, with zero radio IQ drops;
the 20 MS/s source-filter replay also passed. Playback was disabled: these
are software replay results, not antenna/mixer or listening measurements.
Comparative weak-signal sensitivity and live ensemble changes need further
validation. An uncorrectable RS column currently rejects the whole superframe;
individual CRC-failed AUs use a short ramp to silence.

The paced reception regression (requires NumPy/SciPy and the recorded fixtures)
checks 12 seconds at both 10 and 20 MS/s, including station selection, audio
frames and the first 8-second DX job:

```sh
DAB_REALTIME_TEST=1 DAB_FIXTURE_DIR=/path/to/fixtures python3 tests/test_dab.py
```

Unlike a short offline decode, this also checks that input never falls more
than the source queue's half-second capacity behind the live clock.
It also exercises the production IQ file source with the common 1.778112 MHz
BW filter enabled at 20 MS/s. That regression checks source elapsed time,
decoder lag, audio and IQ/DX drops through the first eight-second analysis.

The optional `stream_acquisition` regression reports first CRC-valid FIC,
ECC/EId and CRC-valid audio AU separately, measured in delivered IQ duration
(2 ms chunks). Reducing the streaming acquisition buffer from three to two
frames changed first FIC from 288 to 192 ms on the 9C, 11C and 7D controls,
with identical valid-frame/AU counts. ECC/EId appeared at 356/950/634 ms and
the first valid AU at 644/662/634 ms respectively. These are fixture-specific
results, excluding tuner settling and processing delay. An AU is compressed
audio, not yet audible PCM; identity depends on received FIG repetition.
The application still runs its deeper DX analysis on separate longer captures.
The streaming radio's 192 ms FIC result does not shorten that DX job or prove
recovery from a 192 ms weak burst. See the research/provenance notes in ORIGIN.
