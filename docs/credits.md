# Licenses and acknowledgements

[Back to README](../README.md)

## Where the name comes from

Linrad DVB began as an add-on to the network IQ output of
[Linrad](https://www.sm5bsz.com/linuxdsp/linrad.htm), the SDR receiver created by
Leif Åsbrink / SM5BSZ. Linrad was initially the SDR frontend: it controlled
the radio hardware and sent IQ samples over the network to linrad_dvb,
which provided digital television decoding. The project later developed
into a standalone receiver with direct SDR support.

Rob Hardenberg has kept the name both to acknowledge those origins and
out of personal appreciation for Linrad, a program he enjoys using.
Linrad UDP input remains supported.

Linrad also served as a reference for the AFEDRI network control and the
signal meter. The current [AFEDRI implementation](../src/afedri.c) follows
the device protocol and Linrad's `afedri.c`; the [meter](../src/signal_meter.h)
adopts Linrad's visual layout and scrolling behaviour. These are acknowledgements
of implementation references and design influence, rather than claims that
the current files are unmodified copies of Linrad sources.

## Development by Rob Hardenberg / PE1ITR

Rob Hardenberg develops and maintains Linrad DVB, including its application
architecture, GTK interface, direct SDR control, receiver settings, signal
displays, recording, network output and player integration. The DVB-T receiver,
TSscatterFix and the DAB/DAB+ receiver developed from his dab-monitor project
are his own receiver and recovery components.

His work on the imported LeanSDR and OnAir components includes application
integration, recovery and error-correction fixes, CPU optimizations, optional
GPU integration and an additional optimized Vulkan kernel, Windows support
and live performance diagnostics. Those contributions extend the original
decoders; their authors retain credit for the underlying receiver cores.
The component records below document both origins and subsequent local changes.

## License

Linrad DVB is licensed under **GNU GPL version 3 only (`GPL-3.0-only`)**.
This is the default for the project-owned application, tools, tests and
documentation, and the DVB-T receiver. Explicit component or file licenses
take precedence. See [LICENSE](../LICENSE) and [NOTICE](../NOTICE).

Copyright (c) 2026 Rob Hardenberg / PE1ITR

Bundled components retain their own licenses and credits:

| Component | Authors | License / source record |
| --- | --- | --- |
| LeanSDR / LeanDVB | pabr | [GPL-3.0+](../third_party/leansdr/LICENSE.txt) · [Origin](../third_party/leansdr/ORIGIN) |
| DVB-S2 LDPC helper | Ahmet Inan / xdsopl; pabr | [0BSD](../third_party/ldpc/LICENSE) · [Origin](../third_party/ldpc/ORIGIN) |
| DVB-T / rbdvbt | Rob Hardenberg / PE1ITR | [GPL-3.0-only](../components/dvbt/LICENSE) · [History](../components/dvbt/README.md) |
| DVB-T2 / OnAir | Kristijonas Adomaitis / adswill and contributors | [GPL-3.0](../components/dvbt2/LICENSE) · [Origin](../components/dvbt2/UPSTREAM.json) |
| DAB / DAB+ | Rob Hardenberg / PE1ITR | GPL-3.0-only · [Origin](../components/dab/ORIGIN) |
| DAB shared DSP | OnAir contributors | [GPL-3.0](../components/dvbt2/LICENSE) · [Origin](../components/dvbt2/UPSTREAM.json) |
| TSscatterFix | Rob Hardenberg / PE1ITR | [MIT](../components/tsscatterfix/LICENSE) · [History](../components/tsscatterfix/README.md) |

LeanSDR includes DVB-S2 acquisition corrections by Ondřej OK1CDJ
([issue #41](https://github.com/pabr/leansdr/issues/41)). The LDPC helper
combines Ahmet Inan's decoder with pabr's external decoder adaptation.
OnAir supplies the DVB-T2 receiver core, its original GPU LDPC implementations
and the generic resampler and DSP support reused by DAB. Integration, fixes
and local optimizations are maintained in this repository. The DAB component
was developed from Rob Hardenberg's own dab-monitor project.

The combined executable is distributed under GPLv3; the component license
notices remain attached to their respective sources. `make package` copies
the project license, NOTICE, credits, historical MIT notice and all bundled
component license texts into `build/`.

External / upstream software used by this project, including LeanDVB / LeanSDR, VLC, Linrad, SoapySDR, rtl-sdr, and HackRF libraries, remains subject to its own license terms.

The DAB+ audio path uses the system [FAAD2](https://github.com/knik0/faad2)
library (GPL-2.0-or-later) for AAC decoding and ALSA for audio output.
Code from FAAD2 is copyright (c) Nero AG, www.nero.com.
Our libdxdabsdr implements the DAB demodulation and transport; its versioned
ETSI references and implementation provenance are recorded in
[components/dab/ORIGIN](../components/dab/ORIGIN) and next to the algorithms. Its sample-rate conversion uses the OnAir resampler above and
libsamplerate. These external libraries retain their own notices and licenses.
The optional NVIDIA video player uses FFplay from FFmpeg with CUDA decoding
and Vulkan/libplacebo rendering; embedded video playback uses libVLC.

Many thanks to these authors and maintainers, and to the contributors to GTK / GLib,
FFmpeg, FFTW, libzvbi, FAAD2, ALSA, libsamplerate, json-c, UHD, LimeSuite and the SoapySDR
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
