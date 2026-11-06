# Licenses and acknowledgements

[Back to README](../README.md)

## License

Linrad DVB is licensed under **GNU GPL version 3 only (`GPL-3.0-only`)**.
This is the default for the project-owned application, tools, tests and
documentation, and the DVB-T receiver. Explicit component or file licenses
take precedence. See [LICENSE](../LICENSE) and [NOTICE](../NOTICE).

Copyright (c) 2026 Rob Hardenberg / PE1ITR

Bundled components retain their own licenses and credits:

| Component | Credit | License and provenance |
| --- | --- | --- |
| LeanSDR / LeanDVB | pabr@pabr.org; acquisition corrections by Ondřej OK1CDJ ([issue #41](https://github.com/pabr/leansdr/issues/41)) | [GPL-3.0-or-later](../third_party/leansdr/LICENSE.txt), [ORIGIN](../third_party/leansdr/ORIGIN) |
| DVB-S2 LDPC helper | Ahmet Inan (xdsopl); external decoder adaptation by pabr@pabr.org | [0BSD](../third_party/ldpc/LICENSE), [ORIGIN](../third_party/ldpc/ORIGIN) |
| DVB-T / rbdvbt | Rob Hardenberg / PE1ITR | [GPL-3.0-only](../components/dvbt/LICENSE), [component history](../components/dvbt/README.md) |
| DVB-T2 / OnAir | [adswill and contributors](https://github.com/adswill/OnAir); integration and local optimizations maintained here | [GPL-3.0](../components/dvbt2/LICENSE), [UPSTREAM.json](../components/dvbt2/UPSTREAM.json), [component documentation](../components/dvbt2/README.md) |
| DAB DSP and integration | Rob Hardenberg / PE1ITR; developed from his own dab-monitor project | Project GPL-3.0-only; [provenance](../components/dab/ORIGIN) |
| DAB polyphase resampler and DSP support | OnAir, adswill and contributors; reuses the generic DSP sources bundled with the DVB-T2 component | [GPL-3.0](../components/dvbt2/LICENSE), [UPSTREAM.json](../components/dvbt2/UPSTREAM.json) |
| libdabsdr 4.0.1 | Petr Kopecký / AbracaDABra | [MIT](../components/dab/lib/LICENSE), [component documentation](../components/dab/README.md) |
| TSscatterFix | Rob Hardenberg / PE1ITR | [MIT](../components/tsscatterfix/LICENSE), [component history](../components/tsscatterfix/README.md) |

The combined executable is distributed under GPLv3; the component license
notices remain attached to their respective sources. `make package` copies
the project license, NOTICE, credits, historical MIT notice and all bundled
component license texts into `build/`.

External / upstream software used by this project, including LeanDVB / LeanSDR, VLC, Linrad, SoapySDR, rtl-sdr, and HackRF libraries, remains subject to its own license terms.

The DAB+ audio path uses Fraunhofer FDK-AAC for AAC decoding and ALSA for audio
output. Its sample-rate conversion uses the OnAir resampler above and
libsamplerate. These external libraries retain their own notices and licenses.
The optional NVIDIA video player uses FFplay from FFmpeg with CUDA decoding
and Vulkan/libplacebo rendering; embedded video playback uses libVLC.

Many thanks to these authors and maintainers, and to the contributors to GTK / GLib,
FFmpeg, FFTW, libzvbi, FDK-AAC, ALSA, libsamplerate, json-c, UHD, LimeSuite and the SoapySDR
drivers and device libraries, including SDRplay and Airspy support.

## Licensing policy

The current project uses standard GPLv3 without the former custom DVB-T
requirement to credit the author in every public use or publication. Preserve
the copyright, license and other notices required by the applicable licenses.
Acknowledgement of Rob Hardenberg / PE1ITR and upstream contributors is welcome.

The previous MIT notice for project-owned application code is preserved in
[MIT-legacy.txt](licenses/MIT-legacy.txt). Permissions granted under that license
for earlier versions are not revoked. Likewise, earlier standalone DVB-T
releases retain the terms under which they were distributed. Their former
repository location does not affect those grants.

You may use, study, modify and redistribute the current application under
GPLv3, including commercially. When distributing binaries, meet the
[GPLv3 corresponding-source requirements](https://www.gnu.org/licenses/gpl-3.0.html#section6),
including the source and build material needed for the version you distribute.
The source archive produced by `make dist` packages the current source tree;
keep it matched to the released binaries. `make package` alone only builds the
binaries and collects license notices.

The root GPLv3 license does not change the terms of upstream source files,
external libraries, drivers or media. Broadcast content shown in the screenshot
and linked videos remains subject to its own rights.
