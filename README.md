# DVB-S / DVB-S2 Receive Pipeline for Linrad and AFEDRI

This repository contains a small set of command-line tools and helper scripts to receive, inspect, and decode narrowband DVB-S and DVB-S2 signals using either:

- **Linrad UDP IQ output**, or
- **direct AFEDRI UDP IQ streaming**

The scripts are built around a practical receive chain using:

- **LeanDVB / LeanSDR** for DVB-S and DVB-S2 demodulation
- **VLC (cvlc)** for MPEG-TS playback
- **TSDuck (`tsp`)** for transport-stream analysis
- Python helper scripts for:
  - UDP IQ ingest
  - AFEDRI control
  - basic input-level measurements
  - in-band SNR estimation
  - service/provider extraction from `tsp analyze`

The main entry point is:

- `rx_dvb_linrad_leandvb.sh`

## Included files

### `rx_dvb_linrad_leandvb.sh`
Main launcher script with multiple predefined receive profiles.

It can:
- decode DVB-S or DVB-S2 from Linrad UDP IQ
- decode DVB-S2 directly from AFEDRI UDP IQ
- measure raw input levels
- estimate SNR in a selected signal bandwidth
- tee MPEG-TS into TSDuck and log detected service/provider information

### `linrad_udp_to_stdout_v2.py`
Receives Linrad UDP IQ samples and writes the payload to stdout.

Features:
- strips the Linrad UDP header
- outputs interleaved IQ samples
- supports raw **s16** output
- can convert **s16 IQ** to **u8 IQ**
- optional s16 gain
- optional one-shot input/output IQ statistics with `--measure`

This is mainly intended as a pipe source for LeanDVB or the SNR meter.

### `afedri-udp.py`
Receives AFEDRI UDP IQ packets and writes raw **s16 IQ** payload to stdout.

Features:
- optional source IP filtering
- packet loss counting based on sequence numbers
- optional periodic packet statistics to stderr

Useful when the AFEDRI is already configured and streaming.

### `afedri-control.py`
Basic AFEDRI TCP control script.

This is the original control utility included in the set.

### `afedri-control-fixed.py`
Improved / fixed AFEDRI TCP control script.

Based on the code and comments, this version is intended to handle:
- single-channel mode selection
- AGC get/set
- frequency setting
- RF gain coding compatible with AFEDRI / Linrad expectations
- sample-rate related calculations

Use this if you need more reliable AFEDRI configuration handling than the original script.

### `iq_band_snr_meter.py`
Reads complex IQ samples from stdin and estimates signal and noise power in the frequency domain.

It reports values such as:
- signal + noise power
- estimated signal power
- C+N / N
- SNR
- equivalent SNR normalized to a reference bandwidth

This is useful for checking whether a captured signal is likely good enough for DVB decoding, or for comparing receive conditions between runs.

### `tsp_monitor.py`
Consumes the output of:

```bash
tsp -I file - -P analyze --interval 5 -O drop
```

and extracts useful service metadata.

It:
- stores the latest full `tsp analyze` block in `tsp_latest.txt`
- extracts `Service name` and `provider`
- appends new service/provider pairs to `service_provider.log`
- suppresses repeated log entries for one hour

## Typical signal flow

### Linrad path

```text
Linrad UDP IQ -> linrad_udp_to_stdout_v2.py -> leandvb -> VLC
```

or

```text
Linrad UDP IQ -> linrad_udp_to_stdout_v2.py -> iq_band_snr_meter.py
```

### AFEDRI path

```text
AFEDRI UDP IQ -> afedri-udp.py -> leandvb -> VLC
```

or

```text
AFEDRI UDP IQ -> afedri-udp.py -> iq_band_snr_meter.py
```

### AFEDRI + transport stream monitoring

```text
AFEDRI UDP IQ -> afedri-udp.py -> leandvb -> tee -> VLC
                                             -> tsp analyze -> tsp_monitor.py
```

## Requirements

At minimum, you will typically need:

- Python 3
- LeanSDR / LeanDVB build with `leandvb`
- VLC / `cvlc`
- TSDuck (`tsp`) for profile 12
- a working Linrad UDP IQ source or AFEDRI SDR

Python packages:
- `numpy` is required for `iq_band_snr_meter.py`

Example:

```bash
python3 -m pip install numpy
```

## Usage

### Show available profiles

```bash
./rx_dvb_linrad_leandvb.sh --info
```

### Run a profile

```bash
./rx_dvb_linrad_leandvb.sh --profile 4
```

## Available profiles

The main script currently defines 12 profiles:

1. **DVB-S 125 kS, FEC 1/2, u8 input**
2. **DVB-S2 125 kS, QPSK, FEC 1/2, u8 input**
3. **DVB-S 125 kS, FEC 1/2, s16 input**
4. **DVB-S2 125 kS, QPSK 1/2, s16 input, LDPC bitflips 100**
5. **DVB-S2 125 kS, multiple QPSK modcods, s16 input, LDPC bitflips 400**
6. **DVB-S2 66 kS, QPSK 2/3, s16 input, LDPC bitflips 50**
7. **Measure s16 input levels from Linrad UDP**
8. **Measure SNR from Linrad UDP for a defined bandwidth/sample rate**
9. **Measure SNR directly from AFEDRI UDP**
10. **Decode DVB-S2 directly from AFEDRI UDP**
11. **Decode DVB-S2 directly from AFEDRI UDP at lower sample rate**
12. **Decode DVB-S2 from AFEDRI UDP and monitor MPEG-TS services via TSDuck**

## Examples

### 1. Decode DVB-S2 from Linrad UDP

```bash
./rx_dvb_linrad_leandvb.sh --profile 4
```

### 2. Measure Linrad IQ levels

```bash
./rx_dvb_linrad_leandvb.sh --profile 7
```

### 3. Measure in-band SNR from AFEDRI

```bash
./rx_dvb_linrad_leandvb.sh --profile 9
```

### 4. Decode and inspect transport stream services

```bash
./rx_dvb_linrad_leandvb.sh --profile 12
```

This profile:
- decodes DVB-S2 using `leandvb`
- plays the MPEG transport stream in VLC
- runs `tsp analyze`
- stores the most recent analysis in `tsp_latest.txt`
- logs service/provider discoveries in `service_provider.log`

## Standalone script examples

### Receive AFEDRI IQ and print packet stats every 1000 packets

```bash
python3 afedri-udp.py --stats-every 1000 > /dev/null
```

### Restrict AFEDRI IQ input to one source IP

```bash
python3 afedri-udp.py --source-ip 192.168.1.100 > iq.raw
```

### Measure SNR from a raw IQ stream

Example with AFEDRI IQ piped into the SNR meter:

```bash
python3 afedri-udp.py \
  | python3 iq_band_snr_meter.py --fs 1010526 --signal-bw 170000 --exclude-bw 300000 --raw-power --avg-blocks 256 --ref-bw 2500
```

### Inspect Linrad IQ input and output levels

```bash
./linrad_udp_to_stdout_v2.py --measure 3200000 > /dev/null
```

### Convert Linrad s16 IQ to u8 IQ

```bash
./linrad_udp_to_stdout_v2.py --u8 --scale 128 > iq_u8.raw
```

## Notes on sample formats

These scripts use two sample representations:

- **s16 IQ**: signed 16-bit interleaved I/Q samples
- **u8 IQ**: unsigned 8-bit interleaved I/Q samples

`linrad_udp_to_stdout_v2.py` can either:
- pass through s16 IQ
- apply linear s16 gain
- convert s16 IQ to u8 IQ with configurable scale and offset

The selected LeanDVB command line must match the actual sample format being piped into it.

## Notes on paths

The launcher script currently expects `leandvb` at:

```text
.././leansdr/src/apps/leandvb
```

You may need to adjust this path to match your local build tree.

## Output files created by `tsp_monitor.py`

When profile 12 is used, the following files are created in the current working directory:

- `tsp_latest.txt` — latest full `tsp analyze` block
- `service_provider.log` — timestamped service/provider log

## Suggested workflow

A practical way to use this repository is:

1. Verify that IQ is arriving correctly.
   - Use profile 7 or `--measure`
2. Estimate whether the signal is usable.
   - Use profile 8 or 9
3. Try DVB decoding with a matching profile.
   - Start with profile 4, 5, 10, 11, or 12 depending on source and sample rate
4. If transport stream lock is obtained, inspect service metadata.
   - Use profile 12

## Caveats

- The profiles are tuned for a specific local setup and may need adjustment for your station.
- The sample rates, symbol rates, modcod masks, and LDPC settings are not generic defaults.
- VLC caching parameters are intentionally aggressive for live receive work.
- AFEDRI control and streaming may depend on prior device configuration.

## License

No license file is included in this repository at the moment.
Add your preferred license before publishing if needed.
