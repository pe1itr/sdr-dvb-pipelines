# TSscatterFix

This is the actively maintained TSscatterFix component of `linrad_dvb`. Develop
recovery and reconstruction directly here. The former `TSscatterfix` repository
is historical provenance, not an update source or build dependency.

Original author: **Rob Hardenberg / PE1ITR**. See [LICENSE](LICENSE) (MIT).

## Source layout and integration

- `src/recovery.h`, `src/recovery.c`: `SfRecovery` streaming API, image callbacks,
  service information and reconstruction progress.
- `src/media.c`, `src/rank_model.c`: media reconstruction, decoding and candidate
  ranking.
- `src/psi.c`, `src/ts_packet.c`: service tables and MPEG-TS packet handling.
- The remaining C sources and headers retain the original repair, parsing,
  model and CLI code for further development. They are not linked into the
  current `linrad_dvb` recovery path; `src/main.c` is not its entry point.

Only recovery, media, ranking, PSI and TS packet sources are compiled into
`linrad_dvb`. The `SfRecovery` API delivers RGB24 images and service/progress
snapshots directly; no Python helper or external executable is needed. FFmpeg
development libraries (`libavcodec`, `libavformat`, `libavutil`, `libswscale`)
are required to build this recovery code.

[`../../src/scatter_backend.c`](../../src/scatter_backend.c) manages the worker
thread and feeds a parallel copy of the received TS to this API.
[`../../src/scatter_view.c`](../../src/scatter_view.c) displays its results in GTK.
Keep application threads and widgets there. The player's original TS must
remain unchanged. The host's service/EIT/teletext handling lives in
`src/ts_info.c` and also uses libzvbi.

From the repository root:

```sh
make all
make test-scatter
make test-ts-info
```

[`test_scatter.py`](../../tests/test_scatter.py) generates H.264 and H.265 TS
fixtures with FFmpeg and exercises the component through
[`scatter_probe.c`](../../tests/scatter_probe.c), checking recovered images and
unchanged player output. These tests require an `ffmpeg` executable with
`libx264` and `libx265`. Run `make test` for the complete host integration suite;
also replay a known damaged TS recording when changing reconstruction behavior.

## Origin

Originally imported as unmodified C sources from `TSscatterfix`, revision
`e2fdd3cc03ed84954d350bb0793a6f9179b7d7ea` (2026-10-29). On 2026-11-05 this source
tree moved from `third_party/tsscatterfix` to this project component without
replacing its code. Future changes and releases are recorded in this project's
Git history. Keep the license with source and binary distributions. See the
[component development guide](../README.md) for the shared workflow.
