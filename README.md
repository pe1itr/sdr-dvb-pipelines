# Linrad DVB

A Linux SDR receiver for **DVB-S, DVB-S2, narrowband DVB-T and 8 MHz DVB-T2**,
with a GTK interface for live spectrum, waterfall, constellation, decoder status
and television playback. Developed by **Rob Hardenberg / PE1ITR** for amateur
DATV, weak-signal experiments and broadcast reception.

This is the main development repository for **linrad_dvb**, the **DVB-T receiver
(rbdvbt)** and **TSscatterFix**. Their source code, integration, tests and future
development are maintained together here.

![Linrad DVB receiving an 8 MHz DVB-T2 multiplex, with spectrum, constellation, service information and live video](docs/images/linrad-dvb-dvbt2.png)

*DVB-T2 reception at 570 MHz. Screenshot from an actual receive session.*

## What it does

Linrad DVB turns raw radio samples (IQ) into digital television. It tunes a
supported SDR, or accepts samples from Linrad or an IQ recording, synchronizes
and decodes the DVB signal, and produces an MPEG transport stream. You can watch
the received programme in the same window while checking signal strength,
spectrum, constellation and decoder errors.

Use DVB-S/S2 and narrowband DVB-T for amateur digital television and weak-signal
experiments; use DVB-T2 for supported 8 MHz broadcast multiplexes. You can also
save a reception, replay it later, or forward the decoded stream to another
player. TSscatterFix provides a separate image-recovery view for incomplete
receptions. A headless mode runs the receiver without the graphical interface.

```text
SDR / Linrad UDP / IQ file -> DVB decoder -> television, recording or network output
```

## Videos

- [Watch video 1 on YouTube](https://youtu.be/F3bMFZxpWrI)
- [Watch video 2 on YouTube](https://youtu.be/3F3bCIXOU2w)

## Features

- Receive IQ from Linrad over UDP, a supported SDR, or a recorded IQ file.
- Built-in DVB decoders, with parallel LDPC decoding for DVB-S2 and DVB-T2.
- Embedded VLC playback, service selection, EIT programme information and teletext.
- TSscatterFix image recovery from incomplete transport streams.
- IQ and MPEG-TS recording, UDP transport-stream output and headless SRT output.
- Parallel USB, FM and FM-DATA audio monitoring, TCI and optional PCM UDP output.

Inputs include RTL-SDR, HackRF, USRP B210, RFSpace SDR-IQ, AFEDRI / AFE822x,
FUNcube Dongle Pro+, LimeSDR Mini v1.x, SDRplay RSP1A/B, Airspy HF+ Dual Port
(VHF) and Airspy R2. Hardware support depends on the installed drivers; see
[receiver setup](docs/receivers.md).

DVB-T2 currently supports **8 MHz broadcast channels**, starting at 10 Msps IQ
input. MISO, inter-frame interleaving and type-2/sub-sliced PLPs are not supported.
See the [DVB-T2 component](components/dvbt2/README.md) for the supported scope.

## Build and run

The complete application builds on Linux. You need:

- A C compiler, a C++20 compiler, Make, Bash, `pkg-config` and Rust/Cargo 1.70+.
- Development libraries for GTK 3, GLib, json-c, single-precision FFTW3,
  SoapySDR, UHD, ALSA, libsamplerate, libVLC 3, libzvbi and FFmpeg
  (`libavcodec`, `libavformat`, `libavutil`, `libswscale`).
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
| `components/` | Maintained DVB-T, DVB-T2 and TSscatterFix components |
| `third_party/` | Bundled LeanSDR and LDPC sources, licenses and provenance |
| `tests/` | Regression tests, fixtures and hardware simulators |
| `tools/` | Bash scripts for Linux and ODROID-N2 builds |
| `dev/` | Source packaging, test fixtures and decoder experiments |
| `benchmarks/` | Decoder measurement reports |
| `docs/` | Detailed documentation and the README screenshot |
| `*.example.ini`, `direwolf-pcm.conf` | Example receiver and audio configurations |

The retired shell pipeline and its Python input helpers are no longer shipped.
Local settings, recordings, builds, logs and other screenshots are excluded by
`.gitignore`. The application and its tests do not depend on those retired files.

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
