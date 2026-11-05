# Project components

These components are developed and maintained in this repository together with
`linrad_dvb`. This is their source of truth. The former `dvbt-rx` and
`TSscatterfix` repositories are historical origins, not build dependencies or
update sources. Do not replace these directories with upstream snapshots.

| Component | Responsibility | Host integration | Focused checks |
|---|---|---|---|
| [DVB-T / rbdvbt](dvbt/README.md) | IQ to MPEG-TS: acquisition, OFDM, demapping, FEC and TS output | `src/dvbt_backend.c`, `src/decoder_monitor.h` | `make test-dvbt` |
| [TSscatterFix](tsscatterfix/README.md) | Reconstruct images from incomplete MPEG-TS; publish recovery status | `src/scatter_backend.c`, `src/scatter_view.c` | `make test-scatter` |

Keep signal processing and recovery algorithms in the component that owns them.
Keep GTK widgets, process supervision, source selection and application routing
in the root `src/` directory. Preserve the existing `rbdvbt_*`, `SfRecovery` and
`scatterfix_*` API names so each component remains recognizable. Changes to an
API and its callers belong in the same project change.

Build and test from the repository root:

```sh
python3 tools/build.py
make test-dvbt test-scatter
make test
make package
make dist
```

The root Makefile is the maintained build definition. Component objects live in
`build/components/dvbt/` and `build/components/tsscatterfix/`. Focused regression
tests and host integration tests live in the root `tests/` directory; see each
component README for the relevant files. For changes to reception or recovery,
also replay a known IQ or TS recording when available.

`make package` includes both component licenses with the binaries. `make dist`
includes their complete source directories, documentation and licenses; an
extracted archive can build without Git or neighbouring repositories. Each
component README records its original import revision. Subsequent development
uses this project's Git history and releases; those original revisions are
provenance, not the current component version.

The migration preserves all C sources and headers already integrated into
`linrad_dvb`, including the original CLI entry points. It does not import the
old repositories' Windows GUIs, deployment scripts, recordings, model files or
full Git histories. The old experimental root shell launcher is separate from
the supported `linrad_dvb` workflow.

External LeanSDR and LDPC sources remain under `third_party/` with their own
licenses and provenance.
