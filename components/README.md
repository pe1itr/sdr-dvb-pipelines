# Project components

These components are developed and maintained in this repository together with
`linrad_dvb`. This is their source of truth. The former `dvbt-rx` and
`TSscatterfix` repositories are historical origins, not build dependencies or
update sources. Do not replace these directories with upstream snapshots.

| Component | Responsibility | Host integration | Focused checks |
|---|---|---|---|
| [DVB-T / rbdvbt](dvbt/README.md) | IQ to MPEG-TS: acquisition, OFDM, demapping, FEC and TS output | `src/dvbt_backend.c`, `src/decoder_monitor.h` | `make test-dvbt` |
| [DVB-T2 broadcast](dvbt2/README.md) | 8 MHz IQ to MPEG-TS, automatic L1 parameters and PLP selection | `src/dvbt2_backend.cc`, `src/decoder_monitor.h` | `make test-dvbt2` |
| [DAB / DAB+](dab/README.md) | Mode-I FIC/TII, weak/DX recovery, DAB+ audio and MOT slides | `src/dab_worker.cpp`, `src/dab_view.c` | `make test-dab` |
| [DVB-T bonding](../docs/dvbt-bonding.md) | Discover related channels and merge their MPEG-TS, component 1.6.0 from PE1ITR's dvb-channel-bonding project | `src/dvbt_bonding.c` | `make test-dvbt-bonding` |
| [TSscatterFix](tsscatterfix/README.md) | Reconstruct images from incomplete MPEG-TS; publish recovery status | `src/scatter_backend.c`, `src/scatter_view.c` | `make test-scatter` |

Keep signal processing and recovery algorithms in the component that owns them.
Keep GTK widgets, process supervision, source selection and application routing
in the root `src/` directory. Preserve the existing `rbdvbt_*`, `SfRecovery` and
`scatterfix_*` API names so each component remains recognizable. Changes to an
API and its callers belong in the same project change.

Build and test from the repository root:

```sh
bash tools/build.sh
make test-dvbt test-dvbt2 test-dab test-scatter
make test
make package
make dist
```

The root Makefile is the maintained build definition. Component objects live in
`build/components/dvbt/`, `build/components/dvbt2/` and `build/components/tsscatterfix/`. Focused regression
tests and host integration tests live in the root `tests/` directory; see each
component README for the relevant files. For changes to reception or recovery,
also replay a known IQ or TS recording when available.

`make package` includes the component licenses with the binaries. `make dist`
includes their complete source directories, documentation and licenses; an
extracted archive can build without Git or neighbouring repositories. Each
component README records its original import revision. Subsequent development
uses this project's Git history and releases; those original revisions are
provenance, not the current component version.

The migration preserves all C sources and headers already integrated into
`linrad_dvb`, including the original CLI entry points. It does not import the
old repositories' Windows GUIs, deployment scripts, recordings, model files or
full Git histories. The retired shell launcher and its Python input helpers are
not part of this source tree; `linrad_dvb` uses the compiled helpers in `src/`.

External LeanSDR and LDPC sources remain under `third_party/` with their own
licenses and provenance.

The bonding import has its own [source record](bonding/ORIGIN) and
[GPL-3.0-only license](bonding/LICENSE), assigned by Rob Hardenberg / PE1ITR
in source commit `89100be`.
