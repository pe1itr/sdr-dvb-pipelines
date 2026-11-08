# Linrad DVB

A Linux SDR receiver for **DVB-S, DVB-S2, narrowband DVB-T, 8 MHz DVB-T2 and DAB/DAB+**,
with a GTK interface for live spectrum, waterfall, constellation, decoder status
and television playback. Developed by **Rob Hardenberg / PE1ITR** for amateur
DATV, weak-signal experiments and broadcast reception.

This is the main development repository for **linrad_dvb**, the **DVB-T receiver
(rbdvbt)** and **TSscatterFix**, with DAB reception in its own [component](components/dab/README.md). Their source code, integration, tests and future
development are maintained together here.

![Linrad DVB receiving an 8 MHz DVB-T2 multiplex, with spectrum, constellation, service information and live video](docs/images/linrad-dvb-dvbt2.png)

*DVB-T2 reception at 570 MHz. Screenshot from an actual receive session.*

## What it does

Linrad DVB turns raw radio samples (IQ) into digital television and DAB+ radio.
It tunes a supported SDR, or accepts samples from Linrad or an IQ recording.
For DVB, it synchronizes and decodes the signal into an MPEG transport stream. You can watch
the received programme in the same window while checking signal strength,
spectrum, constellation and decoder errors. In DAB mode, it identifies the
ensemble and services, plays a selected DAB+ station and displays transmitted
text and MOT slideshow images. DAB+ audio requires FAAD2 support; legacy
DAB/MP2 audio is not decoded.

Use DVB-S/S2 and narrowband DVB-T for amateur digital television and weak-signal
experiments; use DVB-T2 for supported 8 MHz broadcast multiplexes. You can also
save a reception, replay it later, or forward the decoded stream to another
player. TSscatterFix provides a separate image-recovery view for incomplete
receptions. A manual reception log records stations and confirmed DAB TII codes,
with editing and ADIF export. Your saved station locator also supports distance
and antenna-bearing calculations. A headless mode runs the receiver without
the graphical interface.

```text
SDR / Linrad UDP / IQ file -> DVB decoder -> television, recording or network output
                          -> DAB decoder -> DAB+ audio, text, slideshow and ensemble/TII info
```

## Videos

- [Watch video 1 on YouTube](https://youtu.be/F3bMFZxpWrI)
- [Watch video 2 on YouTube](https://youtu.be/3F3bCIXOU2w)

## Features

- Receive IQ from Linrad over UDP, a supported SDR, or a recorded IQ file.
- Live spectrum, waterfall, constellation and decoder status, with an adjustable
  IQ bandpass filter, receiver/converter offsets and PNG screenshots.
- DAB Band III or free-frequency tuning, clickable DAB+ stations, dynamic text,
  MOT slideshow, ensemble/country information and confirmed TII logging.
- Normal, weak-signal/DX and aircraft-scatter DAB profiles; optional B210 fixed-IF mixer support.
- Built-in DVB decoders, with parallel LDPC decoding for DVB-S2 and DVB-T2.
- Embedded VLC playback, service selection, EIT programme information and teletext.
- Switch live DVB video to external FFplay with NVIDIA CUDA decoding and Vulkan rendering.
- TSscatterFix image recovery from incomplete transport streams.
- IQ and MPEG-TS recording, IQ replay, UDP transport-stream output and SRT output.
- Parallel USB, FM and FM-DATA audio monitoring, TCI and optional PCM UDP output.
- Editable CSV reception log: UTC time, RF frequency, service/provider, mode,
  decoder settings and DAB TII, with selected/all ADIF export as reception (SWL) records.
- My Station callsign/locator settings and a Maidenhead distance/bearing calculator.
- Headless operation and command-line pipeline inspection for automated receiver setups.

Linux input profiles cover HackRF One, USRP B210, RFspace SDR-IQ, AFEDRI Net,
RTL-SDR, FUNcube Pro+, LimeSDR Mini v1.2, SDRplay RSP1A/RSP1B, Airspy R2 and
Airspy HF+ Dual Port (VHF), and Pluto / PlutoSky RX1 via native libiio. Available receive modes depend on the device's
frequency range and sample rate; some integrations still need live hardware
validation. See [receiver setup and test status](docs/receivers.md).

The complete receiver is supported on Linux. DVB-T2 is limited to the supported
8 MHz broadcast modes; DAB audio supports DAB+ with EEP-A/B protection. The
[Windows port](docs/windows.md) includes DAB+, direct FUNcube Pro+, LimeSDR Mini and Pluto / PlutoSky RX1
support; native hardware/drivers still require Windows validation. See [decoder details](docs/decoders.md) and
[DAB limitations](components/dab/README.md) before choosing a receive mode.

For GPU video playback, select **Renderer → Embedded player** and click
**Extern op GPU** below **Received video** during reception. **Terug in venster**
or closing FFplay returns to embedded VLC while reception continues. This needs
FFplay with CUDA and Vulkan/libplacebo support; see the
[GPU player instructions](docs/usage.md#tijdens-ontvangst-overschakelen-naar-de-nvidia-speler).
DAB uses its own audio player and MOT slideshow.

Click **Log**, directly beside **FM-DATA**, while receiving to capture the current
digital service. Review the service and provider names (together identifying the
station), then click **Save reception**. The timestamp and tuning are captured
when the dialog opens. Nothing is logged automatically. The default file is `reception-log.csv`
beside the configuration file; override it with `[logging] file=/path/to/log.csv`.
Existing entries are preserved, and each saved entry is flushed and closed.
Frequency includes converter LO and digital receiver offset. With Linrad input,
check the manually configured frequency; it can be corrected in the log dialog.
IQ replay is marked `source=file`, with the current logging time.
The CSV `tii` field holds DAB transmitter identifiers and stays empty for DVB.
In DAB mode the main Log button captures the ensemble. Open **TII** in
the spectrum button row and use the **Log** button beside a confirmed code to capture that
transmitter, its ensemble, RF frequency and observation time. Candidates and
ambiguous codes remain visible but cannot be logged as confirmed detections.
The TII button turns green when the latest capture contains codes. The popup
aligns code, confidence, hits, detection status and log history in columns.
**✓ Logged** matches the ensemble name and TII against the saved reception log;
**New** identifies combinations not yet saved. Cancelling the editor does not
mark a transmitter as logged. **Country** in Service info uses the received
ensemble ECC and EId country bits; its tooltip shows both identifiers.

Open **Log → My Station** to enter **My Callsign** and **My Locator**, then click
**Save**. These settings are remembered across restarts and copied into new
receptions (including DAB TII entries). Both fields are optional; locators may
contain 2, 4, 6 or 8 characters. Each saved reception keeps its own station data,
even if you later change My Station. Older CSV logs remain readable and are
upgraded on the next save, leaving unknown station fields empty. Use the RX log
editor to fill those fields for older receptions when needed.

Open **Log → Bearing** and enter a destination Maidenhead locator to calculate
antenna direction from the saved **My Locator**. Results update as you type:
distance rounded to whole kilometres and initial bearing rounded to whole
degrees (0–359°, clockwise from true north: 0° north, 90° east). The short-path
calculation uses locator centres and a spherical Earth, so distances are approximate.
Locators of 2, 4, 6 or 8 characters are accepted, including lowercase input.

Open **Log → RX log** in the top menu to view and sort saved receptions, including
while the receiver is stopped. Double-click a row or choose **Edit selected** to
correct reception data and complete the optional ATV points/direction fields.
Edits replace that record atomically and reject stale edits if it changed on disk.
Select rows with Ctrl/Shift and use **Export
selected ADIF**, or **Export all ADIF** to save the entire list as an `.adi` file.
This follows the selected/all export workflow in [MSHV](https://lz2hv.org/node/10).
**Log → Application log** opens the pipeline/debug log (also available with Alt+L).

ADIF exports mark receptions as `SWL=Y`, use UTC `QSO_DATE`/`TIME_ON` and MHz
`FREQ`/`FREQ_RX`. My Callsign exports as `STATION_CALLSIGN`, and My Locator as
`MY_GRIDSQUARE`, using the values saved in each reception. DVB uses `MODE=ATV`; the precise DVB standard, service, provider,
TII, source and full timestamp are retained in `APP_LINRADDVB_*` fields.
No callsign is inferred from a service name. Loggers requiring `CALL` may reject
broadcast reception records. ADI requires ASCII: readable names are transliterated,
with lossless UTF-8/base64 originals in `*_UTF8_B64` application fields when needed;
the CSV keeps the original UTF-8. See the [ADIF specification](https://adif.org.uk/318/ADIF_318.htm).
Symbol rate (DVB-S/S2), bandwidth (DVB-T/T2) and FEC are captured in the logbook
and included in the ADIF `COMMENT`, rather than additional ADIF fields. FEC uses
fresh decoder telemetry when locked, otherwise the configured value (`auto` for DVB-T2).
The optional contest fields export as `APP_VERON_ATV_POINTS` and
`APP_VERON_ATV_DIRECTION` (`ONE-WAY` / `TWO-WAY`), matching the existing
contestrobot ATV spreadsheet-to-ADIF converter. Empty fields are omitted.
This is a reception log; a complete contest submission still needs callsigns,
locators, reports and exchanges in the contest logging workflow.

Inputs include RTL-SDR, HackRF, USRP B210, RFSpace SDR-IQ, AFEDRI / AFE822x,
FUNcube Dongle Pro+, LimeSDR Mini v1.x, SDRplay RSP1A/B, Airspy HF+ Dual Port
(VHF) and Airspy R2. Hardware support depends on the installed drivers; see
[receiver setup](docs/receivers.md).

DVB-T2 currently supports **8 MHz broadcast channels**, starting at 10 Msps IQ
input. MISO, inter-frame interleaving and type-2/sub-sliced PLPs are not supported.
See the [DVB-T2 component](components/dvbt2/README.md) for the supported scope.

## IQ channel filter

The optional BW filter suppresses signals outside the selected channel before
the decoder. DAB and DVB share the same 8193-tap filter, with a fixed delay of
4096 samples and no sample-rate change. FFT plans are measured once before
reception, including when initially bypassed, so live enabling does not pause
the SDR for planning. On x86-64, SSE2 converts and rounds batches of samples;
other architectures retain scalar conversion. Filter coefficients and bandwidth
are unchanged. Bypass remains bit-exact and retains history for live enabling.

On the Ryzen 7 5800H, a 1.778112 MHz filter at 20 MS/s took a median **0.236 s
per second of IQ**, down from **1.044 s** (three runs, random s16 input, 65536-sample
blocks). Initial planning took about 0.7 s. These are standalone filter timings;
SDR/USB, decoding and display costs are additional. See the
[benchmark results](benchmarks/bandfilter-2026-11-06.json) and
[reproducible benchmark](dev/bench_bandfilter.c). A different FFT plan may change
rounding by one integer sample count; response, delay and strong-adjacent-signal
rejection are checked by `make test-bandfilter test-iq-shift`.

## DAB reception

Select **Mode → DAB**, then a Band III channel. The display shows, for example,
`12C · 227.360000 MHz`; **← / →** and the on-screen arrow buttons move one
channel at a time without restarting the SDR or worker. Sample rates follow the
selected hardware: **4096000 samples/s for B210** is the default when a lower
rate was configured; **2048000 for RTL-SDR** is supported. Wider rates such as
**8, 10 or 20 MS/s**, including non-integer multiples of 2.048 MS/s, are accepted
within the SDR's limits. The full sampled band remains visible in the spectrum;
only the decoder input is reduced to 2.048 MS/s. The connected device must
accept the requested rate; a coerced rate is reported instead of silently used.
Direct reception tunes the channel RF frequency. IQ replay and Linrad input
must already contain the selected channel; their channel labels do not retune
an external source.

The constellation and Service info panels keep their existing positions.
Service info shows the ensemble, station, Sync/FIC/Audio/Ensemble locks,
SNR, CP correlation, CFO, IQ levels, MER and FIC/audio counters. FIC counters
refer to the most recent DX capture. **TII** occupies the button row;
TSscatterFix, EIT and Teletext are hidden in DAB mode. The first discovered
DAB+ station starts automatically; click another station to switch.
Transmitted MOT slides replace the video panel and dynamic text appears
beneath the image. **M** mutes audio. No substitute station image is fabricated
when no slideshow is broadcast. DAB uses its own audio output; the DVB MPEG-TS
renderer and TSscatterFix do not apply.

DAB diagnostics are available through the receiver’s HTTP `/status` endpoint
(see below), without extra rows in the main window. The application receives
worker snapshots through a pipe and keeps them in memory; it does not write a
status JSON file. These include **IQ queue / IQ peak** in milliseconds
(2000 ms capacity), **Audio queue / Audio peak** in encoded frames (64-frame
capacity), **Audio drops**, **ALSA underruns**, **DX drops** and the last **DX time**
in milliseconds. Peaks and loss counters reset on channel change, and persist
across station changes. IQ drops count discarded radio IQ blocks; DX drops count
complete pending identification captures replaced under load. ALSA underruns
count playback EPIPE errors before recovery, including possible resume-after-mute
events; they are unavailable with audio output disabled. DAB playback requests a
300 ms device buffer to absorb burst delivery of decoded superframes, at the
cost of additional audio latency.

The endpoint also reports `rejected_superframe_candidates` (failed sliding
synchronization candidates, not a count of missing audio frames),
`damaged_audio_units` (audio CRC failures), `aac_decode_errors` (FAAD2 errors)
and `concealed_audio_frames` (access units replaced by a short fade to silence).
These counters also reset on channel change. They distinguish received or
decoded audio damage from output underruns; `dropped_audio` only counts overflow
of the encoded audio queue. Queue values exclude
work in progress, source/USB buffers and the sound device's playback buffer.
Unavailable measurements are omitted or null; check `telemetry_fresh` and
`telemetry_age_ms` before treating a snapshot as live.
The **Capture** percentage remains the progress of the eight-second identification
recording for short signal openings/scatter, not a queue-fill measurement.
Its length and analysis frequency are unchanged; compare DX time with the
8000 ms capture interval when checking whether analysis keeps up.

**Normal**, **Weak signal / DX** and **Aircraft scatter** profiles are available.
The latter two reuse dab-monitor's PRS fallback and partial FIC accumulation;
aircraft scatter also enables temporal ranking and subscans. TII decoding runs
on eight-second captures, separately from continuous station audio. See the
[DAB component](components/dab/README.md) for processing details and limitations.

For the B210 Kintex-7 external mixer, Mode also offers **fixed IF**, receive-port
selection and a separate **conducted LO output** switch/port. Both mixer and LO
output default to off. With 53 MHz IF and channel 12D, the requested LO is
176.072 MHz. The LO port connects to the mixer LO input; receive and LO cannot
use the same physical connector. The measured dab-monitor clock/tone settings
and startup conditioning are preserved. Hardware validation of this integration
is still required; the software tests use IQ recordings and UHD mocks.

For DAB outside the Band III channel grid, open **Mode**, check **Free
frequency**, and enter the frequency in MHz. Uncheck it to use the channel
list again. During reception, left/right tunes by the SDR tuning step in free
mode. The receiver's hardware range and DAB sample-rate requirements still apply.

### Network diagnostics

Open **Log → Status endpoint…** to switch the endpoint on or off immediately,
for any receiver mode. Set the listen address to `0.0.0.0` for access from other
computers, or `127.0.0.1` for local access. The default port is `8877`. Switch the
endpoint off before changing its address or port. Close the dialog and press
**W** in the main window to save the setting for the next start.

Enable network diagnostics on Linux or Windows with:

```sh
linrad_dvb --status-bind 0.0.0.0 --status-port 8877
```

Read `http://RECEIVER-IP:8877/status` in a browser, with `curl`, or from PowerShell:

```powershell
Invoke-RestMethod http://RECEIVER-IP:8877/status | ConvertTo-Json -Depth 12
```

Alternatively configure `[status]` with `enabled=true`, `bind=0.0.0.0` and
`port=8877`. Without configuration the endpoint is disabled; the default bind
address is `127.0.0.1`. `--status-port 0` disables a saved setting. On Windows,
allow the chosen TCP port through the firewall for the private network.
This is an unauthenticated, read-only diagnostic endpoint for a trusted LAN.

The versioned JSON response contains `state`, `message`, `exit_code`, source,
standard, stage activity, `telemetry_age_ms`, `telemetry_fresh` and `recent_log`
(the latest 64 KiB of pipeline messages and worker stderr). DAB measurements,
including its `error` field, are under `dab`; DVB measurements and error counters
are under `decoder`. Missing snapshots are null. A stopped or stale snapshot is
marked `telemetry_fresh=false`; HTTP 200 means the status was retrieved, not that
reception is healthy. The endpoint stays available when reception stops or fails
in the GUI, until the application closes. With `--run`, it ends when the receiver
process exits. Errors before application startup remain on stderr.

Slow clients have a three-second deadline; at most sixteen connections are kept
open. Requests never wait for new IQ or decoder data. The standalone `dab_worker`
retains its legacy file output for existing scripts; `--status-stdout` selects the
same newline-delimited JSON stream used by the application. Slideshow and tuning
control files are separate from status and remain in use.

## Build and run

The complete application builds on Linux. You need:

- A C compiler, a C++20 compiler, Make, Bash, `pkg-config` and Rust/Cargo 1.70+.
- Development libraries for GTK 3, GLib, json-c, single-precision FFTW3,
  SoapySDR, UHD, ALSA, libsamplerate, libVLC 3, libzvbi and FFmpeg
  (`libavcodec`, `libavformat`, `libavutil`, `libswscale`).
- For DAB+ audio and slideshow: FAAD2 development files (`pkg-config faad2`).
  Our source-built libdxdabsdr handles Mode-I demodulation and EEP-A/B transport;
  FAAD2 decodes AAC. Without FAAD2/ALSA, FIC/TII decoding remains available.
- VLC codec plugins for playback and the drivers/firmware for your SDR.
  Native LimeSDR Mini support additionally requires LimeSuite at build time.

All decoder sources and Rust dependencies are included. No neighbouring
repositories or separately installed LeanDVB executable are needed for the
built-in decoders.

From the repository root:

```sh
bash tools/build.sh --check
bash tools/build.sh -j 4
cp -n linrad-dvb.example.ini linrad-dvb.ini
./build/linrad_dvb
```

The build helper checks headers and link dependencies before building. Missing
optional drivers produce warnings. Select your receiver in **Input**, choose the
standard and parameters in **Mode**, then press **Start SDR**. Press **W** to
save settings to your local configuration. With Linrad input, tune the radio in
Linrad and match its UDP port, sample rate and IQ format.

An additional [DVB-T2 example](linrad-dvbt2-eindhoven.example.ini) contains the
570 MHz / 8 MHz settings used for reception in Eindhoven. Copy it to a local
configuration in the repository root and adjust the hardware settings:

```sh
cp -n linrad-dvbt2-eindhoven.example.ini linrad-dvbt2-eindhoven.ini
./build/linrad_dvb -c linrad-dvbt2-eindhoven.ini
```

Configuration paths containing `/` are relative to the configuration file.
Keep the example copies in the repository root so `build/` resolves correctly.

## Documentation

The detailed operating and development notes are mostly in Dutch.

- [Operation, playback, recording and audio](docs/usage.md)
- [Receiver inputs and hardware setup](docs/receivers.md)
- [Headless reception, SRT and ODROID-N2](docs/headless.md)
- [Decoder implementation and measurements](docs/decoders.md)
- [Component development](components/README.md)
- [Licenses and acknowledgements](docs/credits.md)

To inspect a pipeline without opening a receiver:

```sh
./build/linrad_dvb --print-pipeline --standard DVB-S2 --sr 125000 --fec 1/2
```

Use `--headless` to run without the GUI, and `--help` for command-line options.
The compiled `linrad_udp_to_stdout`, `sdr_to_stdout` and `iq_file_to_stdout`
programs in `build/` supply IQ to the application and can also be used directly.

## Repository layout

| Path | Contents |
| --- | --- |
| `src/` | Application, GTK interface, hardware inputs and component adapters |
| `components/` | Maintained DVB-T, DVB-T2, DAB and TSscatterFix components |
| `third_party/` | Bundled LeanSDR and LDPC sources, licenses and provenance |
| `tests/` | Regression tests, fixtures and hardware simulators |
| `tools/` | Linux/ODROID-N2 build scripts and Windows cross-build/dependency tools |
| `dev/` | Source packaging, test fixtures and decoder experiments |
| `benchmarks/` | Decoder and IQ-filter measurement reports |
| `docs/` | Detailed documentation and the README screenshot |
| `*.example.ini`, `direwolf-pcm.conf` | Example receiver and audio configurations |

The retired shell pipeline and its Python input helpers are no longer shipped.
Local settings, recordings, builds, logs and other screenshots are excluded by
`.gitignore`. The application and its tests do not depend on those retired files.
Repository paths in published benchmark reports are relative to the repository root;
`../` identifies optional neighbouring research checkouts. The referenced local
IQ recordings are not included and are not needed to build the application.

## Development and source packages

```sh
make test        # full regression suite
make package     # build and copy component licenses into build/
make dist        # create dist/linrad_dvb-source.zip
```

Building uses Bash and Make; Python is only needed for development utilities,
source packaging and tests. Tests require Python with NumPy; media tests also use FFmpeg with libx264/libx265.
GUI checks use `broadwayd` when available. Tests use synthetic data and local
simulators; they do not replace reception checks with real signals.

An experimental [native Windows x64 package](docs/windows.md) can also be built
with `tools/build-windows.py` and `dev/package_windows.py`. It bundles its DLLs
and plugins and does not require WSL. See the Windows notes for input support
and validation limits.

The source ZIP includes documentation, the screenshot, tests, decoder sources
and licenses. It uses the current files on disk, including uncommitted changes,
and builds without Git. It excludes local settings, recordings, build output,
the retired workflow and Git history.

## License and credits

Linrad DVB and its DVB-T receiver are licensed under **GNU GPL version 3 only
(`GPL-3.0-only`)**. Copyright (c) 2026 Rob Hardenberg / PE1ITR.
See [LICENSE](LICENSE) for the full terms and [NOTICE](NOTICE) for the scope.
You may use, modify and redistribute the software, including commercially,
under those terms. When distributing the combined application, preserve the
notices and provide the corresponding source as required by GPLv3.

Bundled components retain their own licenses and author notices. The earlier
MIT notice for project-owned code is preserved in
[MIT-legacy.txt](docs/licenses/MIT-legacy.txt); permissions already granted for
earlier releases remain in effect.

Thanks to the authors and contributors of LeanSDR / LeanDVB, xdsopl LDPC,
OnAir, Linrad and the SDR, media and GUI libraries used here. Full attribution,
component licenses and source provenance are in
[licenses and acknowledgements](docs/credits.md).
