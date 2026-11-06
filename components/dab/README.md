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

The root Makefile builds `build/dab_worker`. With FDK-AAC/ALSA development
packages and a supported Linux architecture it also copies `libdabsdr.so.4`
beside the executable. `lib/` contains the MIT-licensed 4.0.1 binaries and
headers from AbracaDABra, not its GUI or external decoder sources. The
independent DX decoder builds without that library. The application clearly
reports when audio is unavailable. The current radio path supports DAB+
(AAC), as in dab-monitor; classic DAB MPEG Layer II audio is not implemented.
Windows DAB reception is not enabled by the Windows build helper.

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
TII settings alongside a continuous libdabsdr radio session. Weak/DX adds the
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
make test-dab
make build/dab_gui_smoke
python3 tests/test_linrad.py LifecycleTests.test_dab_gui_controls
# Optional existing receive-only recordings, never required to build:
DAB_FIXTURE_DIR=/path/to/dab-monitor/artifacts/fixtures python3 tests/test_dab.py
```

Core tests exercise TII collisions/false IDs, partial identity isolation,
filter rejection, dynamic labels, MOT and radio callback selection. UHD mocks
verify physical ports, the requested synthesizer frequency, tone/amplitude,
coercion failure and end-of-burst cleanup without opening a device. GTK tests
exercise mode/channel controls, station commands, TII confirmation/logging,
reset, stop and retune using a synthetic IQ helper. Optional IQ regressions
check 9C identity/TII and reject the known false 4615 detection on 7C.

Validation on the initial integration also replayed weak 5B with the channel
filter (UEId E38655, `5B NM Limburg`) and paced the 11C recording into the radio
path: 14 stations discovered, SLAM! selected, 96 audio frames decoded, zero
dropped radio IQ blocks. Audio output was disabled for this replay. These are
software replay results, not measurements of the current antenna/mixer setup.

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
