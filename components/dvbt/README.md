# DVB-T receiver (rbdvbt)

This is the actively maintained DVB-T component of `linrad_dvb`. Develop the
receiver directly in this directory. The former `dvbt-rx` repository is no
longer the development source; no checkout, installed `rbdvbt_rx` executable or
download from that repository is needed.

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
application's dependencies are checked by `python3 tools/build.py --check`.
[`test_dvbt_stream.c`](../../tests/test_dvbt_stream.c) covers streaming Viterbi
puncturing, errors, chunk continuity and reset behavior;
[`test_dvbt_psi.c`](../../tests/test_dvbt_psi.c) covers PAT/PMT bootstrap and
continued delivery. Run `make test` for host integration and replay a known
signal for changes to acquisition, demodulation or FEC.

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
