# LeanDVB receive helpers for multiple SDR sources

This repository contains a small collection of experimental command-line helpers for receiving narrowband DVB-S and DVB-S2 signals with **LeanDVB**.

The setup is built around the main launcher script:

- `rx_dvb_linrad_leandvb.sh`

That script selects a predefined receive profile and connects one of several SDR input paths to `leandvb`, usually followed by `cvlc` for MPEG-TS playback. The profiles are intentionally practical and experimental. There is currently **no polished user interface**; the expected way of working is still via shell commands, pipes, and profile numbers.

Useful upstream references:

- LeanDVB / LeanSDR: <http://www.pabr.org/radio/leandvb/leandvb.en.html>
- Linrad: <https://www.sm5bsz.com/linuxdsp/linrad.htm>

## Project goal

The goal of this repository is to make it easy to try DVB-S and DVB-S2 reception with different SDR devices while keeping a largely uniform downstream pipeline:

```text
SDR or Linrad output -> helper/converter -> leandvb -> VLC / TS tools
```

The helpers mainly solve practical integration issues such as:

- getting IQ into `stdin`
- converting between `s16`, `s8`, and `u8`
- controlling specific radios
- measuring IQ levels or SNR
- inspecting MPEG transport streams

## Main entry point

### `rx_dvb_linrad_leandvb.sh`

This is the central launcher for the whole setup. It defines a set of numbered profiles and hides most of the pipe plumbing.

Current profile groups include:

- Linrad UDP input to LeanDVB
- AFEDRI direct UDP input to LeanDVB
- RTL-SDR direct input to LeanDVB
- HackRF direct input to LeanDVB
- RFSpace SDR-IQ direct input to LeanDVB
- measurement-only profiles for input levels and SNR
- optional transport stream inspection via TSDuck

Show the available profiles with:

```bash
./rx_dvb_linrad_leandvb.sh --info
```

Run a profile with:

```bash
./rx_dvb_linrad_leandvb.sh --profile 4
```

The launcher currently includes profiles 1 through 18, covering DVB-S, DVB-S2, measurement, and multiple source devices. fileciteturn1file0

## Repository contents

### `rx_dvb_linrad_leandvb.sh`
The main profile-driven launcher script. It contains all predefined receive chains and is the preferred starting point. fileciteturn1file0

### `linrad_udp_to_stdout_v2.py`
Receives raw Linrad UDP IQ and writes the payload to stdout. It supports plain `s16` pass-through, optional `s16` gain, and conversion from `s16 IQ` to `u8 IQ` using:

```text
u8 = clip(offset + s16*scale/256)
```

It also supports one-shot level measurement for both input and output IQ. fileciteturn1file8

### `afedri-udp.py`
Receives AFEDRI UDP IQ packets and writes raw `s16` IQ payload to stdout. It supports optional source IP filtering and packet loss statistics based on sequence numbers. fileciteturn1file10

### `afedri-control.py`
Original AFEDRI control helper for setting frequency, rate, and gain and starting/stopping the stream. fileciteturn1file9

### `afedri-control-fixed.py`
Improved AFEDRI/AFE822x control helper with more complete command handling, AGC support, corrected RF gain coding, single-channel mode handling, and detailed reply logging. fileciteturn1file6

### `soapy_sdriq_to_stdout_v2.py`
Reads RFSpace SDR-IQ samples via SoapySDR and writes them to stdout as either `s16` IQ or converted `u8` IQ. It supports the same `u8` scaling philosophy as the Linrad helper. Supported sample rates are constrained to a defined set, and ATT gain is quantized to the device step values. fileciteturn1file1

### `hackrf_s8_to_u8.py`
Converts the HackRF signed 8-bit interleaved IQ stream to unsigned 8-bit IQ for use with `leandvb --u8`. Optional I/Q swap and throughput statistics are supported. fileciteturn1file3

### `iq_band_snr_meter.py`
Reads complex `s16` IQ from stdin and estimates signal and noise power in the frequency domain. It reports C+N/N, SNR, and equivalent SNR normalized to a reference bandwidth. fileciteturn1file7

### `tsp_monitor.py`
Consumes output from `tsp -P analyze`, stores the latest block, and logs newly observed service/provider combinations with one-hour suppression of duplicates. fileciteturn1file5

### `dvbs2_modcod.py`
Helper tool to look up DVB-S2 MODCOD values and combined `--modcods` masks for LeanDVB based on constellation and FEC or FEC ranges. fileciteturn1file2

## General receive architecture

The common downstream idea is always the same:

```text
Source IQ -> format helper -> leandvb -> VLC
```

or for measurement:

```text
Source IQ -> helper -> iq_band_snr_meter.py
```

or for MPEG transport stream inspection:

```text
Source IQ -> helper -> leandvb -> tee -> VLC
                                 -> tsp analyze -> tsp_monitor.py
```

## Device-specific sections

## 1. Linrad network output

Linrad is used here as an external frontend that already receives and conditions the signal. This repository then consumes the **UDP IQ output from Linrad**.

Related website:

- <https://www.sm5bsz.com/linuxdsp/linrad.htm>

### Driver / helper used

- `linrad_udp_to_stdout_v2.py`

### What it does

- listens to Linrad UDP IQ
- strips the Linrad UDP header
- outputs interleaved IQ on stdout
- can output `s16` directly
- can convert `s16` to `u8`
- can measure input and output IQ statistics fileciteturn1file8

### Relevant options

- `--ip` — bind/listen IP, default `127.0.0.1`
- `--port` — UDP port, default `50000`
- `--skip` — bytes skipped at the start of each Linrad UDP packet, default `24`
- `--u8` — convert Linrad `s16` IQ to LeanDVB `u8`
- `--scale` — u8 conversion scale, formula `u8 = clip(offset + s16*scale/256)`
- `--offset` — u8 DC offset, default `128`
- `--s16-gain` — linear gain for `s16` output mode
- `--measure N` — measure first N bytes of input and output IQ and print stats fileciteturn1file8

### Notes

This path is the basis of the “general input” idea in the project. Even though multiple devices are supported directly, the overall workflow still treats the Linrad-fed path as a primary reference path.

The launcher includes several Linrad-based profiles, including DVB-S, DVB-S2, level measurement, and SNR measurement. fileciteturn1file0

## 2. AFEDRI / AFE822x

AFEDRI is supported in two ways:

- direct control/programming of the device
- direct UDP IQ reception into the local pipeline

### Drivers / helpers used

- `afedri-control.py`
- `afedri-control-fixed.py`
- `afedri-udp.py`

### What they do

`afedri-control.py` is the basic original control utility for setting frequency, sample rate, gain, and starting/stopping the stream. fileciteturn1file9

`afedri-control-fixed.py` is the more complete and recommended control utility. It adds:

- main clock query
- real-rate calculation
- single-channel mode selection
- AGC read/write control
- corrected AFEDRI RF gain coding
- detailed TX/RX logging of control responses fileciteturn1file6

`afedri-udp.py` receives the AFEDRI UDP IQ stream and writes raw `s16` IQ to stdout for use with LeanDVB or the SNR meter. fileciteturn1file10

### Relevant options

#### `afedri-control-fixed.py`

- `--ip` — AFEDRI control IP
- `--port` — AFEDRI control TCP port, default `50005`
- `--freq` — center frequency in Hz
- `--rate` — requested sample rate in Hz
- `--gain` — requested RF gain in dB
- `--channel` — DDC to use in single-channel mode (`0` or `2`)
- `--freq-channel-field` — channel field used in the frequency command
- `--gain-channel-field` — channel field used in the RF gain command
- `--agc keep|on|off` — AGC handling for the selected DDC
- `--stop-first` — stop stream before programming
- `--verbose` — detailed logging of control traffic fileciteturn1file6

#### `afedri-udp.py`

- `--bind` — bind address, default `0.0.0.0`
- `--port` — UDP port, default `50005`
- `--source-ip` — optional source IP filter
- `--stats-every` — periodic packet statistics to stderr fileciteturn1file10

### Notes

The launcher contains AFEDRI-related measurement and decoding profiles, including direct SNR measurement and DVB-S2 decoding at different sample rates. fileciteturn1file0

## 3. RTL-SDR

RTL-SDR is supported directly from the command line via the standard `rtl_sdr` utility.

### Driver / helper used

- `rtl_sdr`
- no separate Python converter is needed here

### What it does

RTL-SDR already produces unsigned 8-bit interleaved IQ, which matches LeanDVB’s `--u8` input mode directly. In the current launcher, the RTL-SDR profile sends the raw stream straight into `leandvb --u8`. fileciteturn1file0

### Relevant options seen in the launcher

- `-f` — center frequency
- `-s` — sample rate
- `-g` — tuner gain
- `-` — write raw IQ to stdout fileciteturn1file0

### Notes

Current launcher profile:

- profile 13 — RTL-SDR, DVB-S2, sample rate 2.4 MS/s, symbol rate 125 kS/s fileciteturn1file0

## 4. HackRF

HackRF is supported directly via `hackrf_transfer`, followed by a small converter because HackRF outputs **signed 8-bit IQ**, while LeanDVB expects **unsigned 8-bit IQ** when `--u8` is used.

### Driver / helper used

- `hackrf_transfer`
- `hackrf_s8_to_u8.py`

### What they do

`hackrf_transfer` captures the raw signed 8-bit IQ stream.

`hackrf_s8_to_u8.py` converts the HackRF stream from signed int8 IQ to unsigned uint8 IQ using the byte-wise equivalent of adding 128. Optional I/Q swap and throughput statistics are supported. fileciteturn1file3

### Relevant options

#### `hackrf_transfer`

- `-r -` — write raw IQ to stdout
- `-f` — tuning frequency
- `-s` — sample rate
- `-l` — LNA gain
- `-g` — VGA gain fileciteturn1file0

#### `hackrf_s8_to_u8.py`

- `--chunk-size` — bytes processed per read
- `--iq-swap` — swap I and Q in each IQ pair
- `--stats` — print throughput statistics to stderr fileciteturn1file3

### Notes

The launcher includes HackRF-based DVB-S2 profiles on 436 MHz, 437 MHz, and a high-symbol-rate test on 1291 MHz. One profile is explicitly marked as not working yet, which underlines the experimental status of the setup. fileciteturn1file0

## 5. RFSpace SDR-IQ

RFSpace SDR-IQ is supported via SoapySDR using a dedicated helper script.

### Driver / helper used

- SoapySDR with the `rfspace` driver
- `soapy_sdriq_to_stdout_v2.py`

### What it does

The helper opens the SDR-IQ via SoapySDR, tunes it, sets the sample rate, optionally applies ATT gain, and writes the stream to stdout as either:

- `s16` IQ
- `u8` IQ for LeanDVB `--u8` mode fileciteturn1file1

Supported sample rates are restricted to a fixed list and requested rates are rounded to the nearest supported value. ATT gain is quantized to the supported step values `-20`, `-10`, `0`, and `10` dB. fileciteturn1file1

### Relevant options

- `--device` — SDR-IQ device path or full Soapy device string
- `-f`, `--freq` — center frequency in Hz
- `-r`, `--rate` — requested sample rate
- `-g`, `--gain` — ATT gain in dB
- `--seconds` — run for a limited duration
- `--buf_len` — complex samples per read
- `--u8` — output unsigned 8-bit IQ
- `--s16` — output signed 16-bit IQ
- `--scale` — u8 conversion scale
- `--offset` — u8 conversion offset
- `--progress` — status output to stderr fileciteturn1file1

### Notes

The launcher currently contains SDR-IQ profiles for DVB-S2 on 28 MHz at 125 kS/s and 66 kS/s. fileciteturn1file0

## Measurement and analysis helpers

### `iq_band_snr_meter.py`

Use this when you want a quick estimate of whether the input signal is likely decodable. It reads `s16` IQ, performs FFT averaging, and estimates signal and noise power based on a central signal band and outer-band noise estimate. It also calculates an equivalent SNR normalized to a reference bandwidth. fileciteturn1file7

Example:

```bash
python3 afedri-udp.py \
  | python3 iq_band_snr_meter.py --fs 1010526 --signal-bw 170000 --exclude-bw 300000 --raw-power --avg-blocks 256 --ref-bw 2500
```

### `tsp_monitor.py`

Use this together with `tsp -P analyze` when you want service/provider visibility from the resulting MPEG transport stream. The script writes the latest analysis block to `tsp_latest.txt` and logs newly seen service/provider combinations to `service_provider.log`. fileciteturn1file5

### `dvbs2_modcod.py`

Use this helper when you want to determine LeanDVB `--modcods` masks from human-readable DVB-S2 parameters. It is especially useful for QPSK/8PSK/16APSK/32APSK experiments and for building combined masks for FEC ranges. fileciteturn1file2

## Current profile map

The launcher currently defines these profiles: fileciteturn1file0

1. DVB-S 125k FEC 1/2 from Linrad, u8
2. DVB-S2 125k QPSK 1/2 from Linrad, u8
3. DVB-S 125k FEC 1/2 from Linrad, s16
4. DVB-S2 125k QPSK 1/2 from Linrad, s16
5. DVB-S2 125k multiple modcods from Linrad, s16
6. DVB-S2 66k QPSK 2/3 from Linrad, s16
7. Measure Linrad s16 levels
8. Measure Linrad SNR
9. Measure AFEDRI SNR
10. AFEDRI direct DVB-S2 decode
11. AFEDRI direct DVB-S2 decode at lower sample rate
12. AFEDRI direct DVB-S2 decode plus TS analysis
13. RTL-SDR DVB-S2 125k
14. HackRF DVB-S2 125k on 436 MHz
15. HackRF DVB-S2 333k on 437 MHz
16. HackRF DVB-S2 8PSK SR 5M test on 1291 MHz, marked as not working
17. SDR-IQ DVB-S2 125k FEC 1/2
18. SDR-IQ DVB-S2 66k FEC 2/3

## Typical examples

### Show available profiles

```bash
./rx_dvb_linrad_leandvb.sh --info
```

### Run a Linrad DVB-S2 profile

```bash
./rx_dvb_linrad_leandvb.sh --profile 4
```

### Measure Linrad input levels

```bash
./rx_dvb_linrad_leandvb.sh --profile 7
```

### Measure AFEDRI SNR

```bash
./rx_dvb_linrad_leandvb.sh --profile 9
```

### Run direct RTL-SDR receive

```bash
./rx_dvb_linrad_leandvb.sh --profile 13
```

### Run direct HackRF receive

```bash
./rx_dvb_linrad_leandvb.sh --profile 14
```

### Run direct SDR-IQ receive

```bash
./rx_dvb_linrad_leandvb.sh --profile 17
```

## Requirements

Typical dependencies are:

- Python 3
- `numpy`
- LeanDVB / LeanSDR build with `leandvb`
- VLC / `cvlc`
- TSDuck (`tsp`) for TS analysis profiles
- Linrad for the Linrad-based path
- device-specific command-line tools where applicable:
  - `rtl_sdr`
  - `hackrf_transfer`
  - SoapySDR with RFSpace support for SDR-IQ

Install NumPy if needed:

```bash
python3 -m pip install numpy
```

## Notes and caveats

- This repository is experimental and intentionally shell-oriented.
- The profiles are tuned for a specific local setup and should not be seen as universal defaults.
- Sample rates, symbol rates, `--modcods`, and `--ldpc-bf` settings are practical experiment values.
- The path to `leandvb` in the launcher is currently hardcoded and may need local adjustment. fileciteturn1file0
- VLC caching settings are intentionally tuned for live receive experimentation.
- Device setup may depend on prior configuration outside this repository.

## License

No license is stated in the scripts themselves beyond the upstream tools they interface with. Add a project license if you want to publish or share the repository more broadly.
