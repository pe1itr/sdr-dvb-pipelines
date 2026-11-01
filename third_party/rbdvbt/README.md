# Embedded rbdvbt_rx

Original author: **Rob Hardenberg / PE1ITR**. See [LICENSE](LICENSE).

Pinned source snapshot of the local `dvbt-rx` repository, version 0.1.5,
commit `fd9a44773f0d72bae6a569570636b36dad575233`, imported 2026-10-28.
The source checkout was clean when imported. Building this project needs neither
that checkout nor its executable. `src/main.c` is retained for provenance but
excluded from the integrated build; `src/dvbt_backend.c` in the host project
provides the entry point. No X11 decoder windows are compiled in.

Local changes are limited to `probe_constellation.c` and `dvbt_outer.c`:

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

For an update, replace this snapshot from a known revision, reapply these small
changes, update this file, and run `make test` plus a known-signal replay. Keep
the license with source and binary distributions. Avoid importing recordings,
build output, or the upstream external reference trees.
