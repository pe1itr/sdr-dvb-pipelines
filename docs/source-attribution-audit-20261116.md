# Source attribution review — 2026-11-16

[README](../README.md) · [Credits](credits.md)

Reviewed after commits `5028d7f` (S2 GPU), `eb7559e` (bonding 1.6.0) and
`e6e1d3c` (MHz button). This review records source provenance and documentation
changes. It does not assign a new license to another source project.

## Scope and evidence

Checked the README, NOTICE, central credits, component source/license records,
source headers and reference comments, local project histories, the Rust lock
file, and the Linux/Windows package collection scripts. The local original
repositories were read without modification. No receiver or benchmark was
started for this review; the current Windows ZIP was not replaced.

| Code or dependency | Evidence checked | Attribution and local contribution |
| --- | --- | --- |
| LeanSDR / LeanDVB | [ORIGIN](../third_party/leansdr/ORIGIN), GPL headers and license | pabr's DVB-S/S2 core; Ondřej OK1CDJ acquisition patch. Local process supervision, telemetry, table fixes and BCH/LDPC integration extend that core. |
| CPU LDPC helper | [ORIGIN](../third_party/ldpc/ORIGIN), 0BSD license, helper headers | Ahmet Inan / xdsopl, helper adaptation by pabr. Local framing, SIMD selection, batching and worker management are identified separately. |
| DVB-T2 and GPU backend | [UPSTREAM.json](../components/dvbt2/UPSTREAM.json), component README and imported source comments | OnAir by Kristijonas Adomaitis / adswill and contributors, including C++ DSP, GPU backends and Rust FEC. Local clock/LDPC/channel optimizations and the cached Vulkan shader remain recorded as extensions. |
| P1 tables within OnAir | Original imported `t2_tables.cpp` comment and [gr-dvbt2 primary source](https://github.com/drmpeg/gr-dvbt2/blob/master/lib/p1insertion_cc_impl.cc) | The missing central acknowledgement was Ron Economos / W6RZ, Copyright 2014, GPL-3.0-or-later. The exact historical gr-dvbt2 revision used by OnAir was not recorded; no revision has been invented. |
| New DVB-S2 GPU helper | [Helper source](../src/dvbs2_gpu_helper.cpp), [experiment](dvbs2-gpu-pi6ehv-investigation-20261109.txt) and build definitions | Reuses the existing OnAir-derived LDPC decoder, tables and Vulkan backend. The S2 protocol, conversion, CPU fallback, GUI and comparison tests are local work. No new external CUDA/5G decoder was imported for this feature. |
| DVB-T / rbdvbt | [Component history](../components/dvbt/README.md#origin-and-retained-changes) and earlier local dvbt-rx README/NOTICE | Rob Hardenberg's receiver. Restored its acknowledgement of gr-dvbt and LeanSDR as implementation/testing references; these references are distinguished from copied decoder cores. |
| DAB / DAB+ | [ORIGIN](../components/dab/ORIGIN), source comments, local dab-monitor history | Rob Hardenberg's receiver and local streaming extension, with separate acknowledgement of OnAir resampler/DSP, FAAD2, libsamplerate and pinned specification/research references. |
| TSscatterFix | [README](../components/tsscatterfix/README.md#origin) and MIT license | Rob Hardenberg's recovery component; FFmpeg remains an external codec dependency. |
| Bonding library and receiver merger | [ORIGIN](../components/bonding/ORIGIN), local source project at `719963f`, its `docs/sources.md` and author history | Rob Hardenberg's separate dvb-channel-bonding project. Its source register identifies its transport, discovery, TS adapter and merger as local implementations, without an external FEC library. Receiver integration is maintained here. The source owner resolved the license choice with GPL-3.0-only commit `89100be`; see below. |
| Hardware adapters | Local source comments, earlier source-project history and package builders | Linrad AFEDRI/meter references retained; native FUNcube names SoapyFCDPP's protocol header as a reference. Pluto RX is adapted from Rob Hardenberg's atv-receiver-analog. libiio, UHD, LimeSuite, SoapySDR/device libraries, hidapi and PortAudio are external dependencies. |

The 93 OnAir import records all have corresponding files. Before the new
attribution comment, 72 still matched their original recorded SHA256 and 21
had local changes. The P1 table file was one of the exact matches: its
gr-dvbt2 reference was inherited, not introduced by the S2 GPU work. This
review adds only author/license/source comments to that C++ file; removing
comments and blank lines yields identical source text. The original import
hash remains preserved in `UPSTREAM.json`; inherited provenance and local
changes are recorded separately. After that comment change, 71 files match
and 22 differ from the original import, with none missing.

All **16** original bonding source/header/test hashes matched before the
license update. The source owner then committed GPL-3.0-only notices and
LICENSE in `89100bee201e5d00022673f38d6af468ea6bf44e`. The receiver copied those
16 files and LICENSE from that clean commit; all **17** current ORIGIN hashes
match. Removing the newly added leading copyright/license block from each of
the 16 files yields exactly the earlier bytes, including the frozen ABI header.
The updated license text is byte-identical to the receiver's full GPLv3 text.
The imported revision and inspected source-project history identify Rob
Hardenberg. The Rust lock file contains only the local bridge/onair crates,
with no registry packages.
Added component files outside the original OnAir manifest
are the local PLP helper, Rust bridge, cached shader/generated SPIR-V and
regression/benchmark tests; their introduction is in this repository's history.

## Documentation changes

- README now describes the S2 GPU checkbox, measured CPU relief, its Linux and
  frame-type limits, and the separate video-rendering control. It removes the
  obsolete statement that no GPU LDPC decoder is shipped.
- Bonding 1.6.0 and its receiver merger are listed among Rob Hardenberg's own
  developments, with their actual shared/separate modes and recovery limits.
- README, NOTICE and central credits name the inherited gr-dvbt2 P1 table
  source. OnAir's original import hashes remain intact.
- Central credits distinguish imported code, local extensions, protocol/design
  references and external runtime libraries, including libiio/hidapi/PortAudio.
- GPU performance claims retain the actual measurement boundaries: 54% less
  decoder/helper CPU time on the recorded Linux test, not a whole-machine CPU
  reduction, whole-receiver speedup or general sensitivity result.

## Bonding license item resolved

The original `719963f` tree had no LICENSE/COPYING and its source register
explicitly recorded that no distribution license had been chosen. During this
review Rob Hardenberg updated and committed the source project under
**GPL-3.0-only**, with his copyright notices and the full GPLv3 text, in
`89100bee201e5d00022673f38d6af468ea6bf44e`. The clean committed files were
verified and copied into the receiver, and ORIGIN now pins that commit.

README, NOTICE, credits and component documentation now state the owner's
chosen license. Linux and Windows package rules include the bonding license.
The earlier open item is resolved; this receiver review did not independently
choose or substitute a license. No executable bonding code was changed.

## Distribution records and limits of the check

`make package` copies NOTICE, credits and the existing component license texts.
The Windows packager additionally collects the SDK license directory, source
package manifests and the separate libiio/LimeSuite notices; its corresponding
source ZIP includes the component origins. The source packager includes
`src/`, `components/`, `third_party/`, build tools, tests and documentation.
The updated Linux collection command was checked with `make -n package` and
`build/bonding-LICENSE.txt` was copied and verified. The Windows license list
and packager syntax were checked without running or replacing a release ZIP.
No new release archive was generated.
A complete file-by-file verification of every transitive dependency in an
already published binary package was not performed here.

The named omissions above were corrected. The remaining author/origin
statements rely on the recorded imports, source headers and inspected project
histories; this review does not independently reconstruct authorship of every
line in those projects. The historical gr-dvbt2 revision is still unrecorded rather than filled in
by assumption; the bonding license is now backed by the owner's source commit.
