# MT Express implementation and sources

We thank the British Amateur Television Club (BATC) for making Serit's tuner
documentation and the MiniTiouner RF measurements publicly available on its
wiki. Special thanks to Dave (G8GKQ), whose measurements we use for the MT
Express S-meter. [Serit FTS-4334L specification](https://wiki.batc.org.uk/images/f/fc/FTS-4334L_Series_F1.pdf) · [G8GKQ's RF measurements](https://wiki.batc.org.uk/MiniTiouner_Power_Level_Indication)

This record covers the receiver introduced in commits `8cf889b` and `289e903`.
The Makefile builds `mt_express_rx` from this directory's `receiver.c`,
`control.c` and `monitor.c`. The Linux executable inspected for this review
links to libusb-1.0, libm and libc. Both tuning and transport-stream reception
run without installing LongMynd or MiniTioune. The source of several settings
and calibration values is recorded separately below.

| Function | Origin |
| --- | --- |
| USB control and transport stream | Project implementation using libusb. FTDI documents the MPSSE commands used for control. [FTDI application note AN_108](https://www.ftdichip.com/Support/Documents/AppNotes/AN_108_Command_Processor_for_MPSSE_and_MCU_Host_Bus_Emulation_Modes.pdf) |
| STV6120 tuning | Serit documents the I²C interface, PLL and register layout in sections 3.1–3.3. The 30 MHz reference and routing of input B to RF-C were checked on the local Express. Serit specifies 250–2150 MHz. The application's wider tuning range comes from user experience. [Serit FTS-4334L, revision F1](https://wiki.batc.org.uk/images/f/fc/FTS-4334L_Series_F1.pdf) |
| Demodulator and LNA registers | LongMynd register definitions were consulted for addresses and bit fields. The two reference headers matched the public BATC copies byte for byte. Demodulator setup values in the `setup` array were selected from local register readbacks during PI6EHV reception and comparison with reset state. [STV0910 register definitions](https://github.com/BritishAmateurTelevisionClub/longmynd/blob/master/stv0910_regs.h) · [STVVGLNA register definitions](https://github.com/BritishAmateurTelevisionClub/longmynd/blob/master/stvvglna_regs.h) |
| Low symbol rate and carrier detectors | Inspection of F6DZP's MiniTioune executable informed the 41.25 MHz clock divider combination and the mapping of the two detector selectors, including Costas and Citroen 1/2, to register codes. The automatic threshold of 125 ksym/s inclusive reflects the operator's reception experience. |
| DX recovery | The project worker remembers the last carrier offset, retries acquisition with it after 0.5 s of lock loss, then falls back to a centred search after another 2 s. |
| Constellation and MER | The worker reads symbol coordinates from the demodulator and estimates MER over 256 symbols using a fitted complex gain and reference constellation. DVB-S2 mapping and reference performance are specified in sections 5.4 and 6 of [ETSI EN 302 307-1 V1.4.1](https://www.etsi.org/deliver/etsi_en/302300_302399/30230701/01.04.01_60/en_30230701v010401p.pdf). |
| Spectrum and waterfall | The second STV6120 synthesizer selects the same RF input as the DVB receiver. Each scan reads POWERI and POWERQ from the other STV0910 demodulator. The plot uses the summed codes relative to their maximum of 510. Tuner calibration timeouts leave a gap. [Serit register map, §3.2](https://wiki.batc.org.uk/images/f/fc/FTS-4334L_Series_F1.pdf) · [STV0910 power registers](https://github.com/BritishAmateurTelevisionClub/longmynd/blob/master/stv0910_regs.h) |
| RF level / S-meter | The two interpolation tables in `mt_rf_estimate_dbm` use 24 AGC/indicated-level pairs from Dave G8GKQ's published measurements. The resulting scale estimates the broadband MiniTioune indication. Absolute power at the antenna connector needs a separate calibration. [G8GKQ's measurement method and table](https://wiki.batc.org.uk/MiniTiouner_Power_Level_Indication) |
| LNA gain in dB | Inspection of MiniTioune informed the gain-readout sequence and the eight numeric endpoints in `mt_lna_gain`. They correspond to the low and high values of four gain stages including the MiniTioune display offset of 3 dB. The linear interpolation is project code. The local observation of about 13 dB checks one operating point. An independent calibration of the four ranges or a public source for their eight endpoints is still missing. |

The MiniTioune reference was the operator-supplied F6DZP executable
`MiniTioune_V1_0_1_1c_beta.exe`, SHA-256
`789e90ed77849ccd31d04ba799bcb66dfe39cc54d8b5f8761b3759d365cf9768`.
The two commits contain neither this executable nor LongMynd source files.
This review establishes provenance only. Permission to redistribute
MiniTioune-derived calibration data has not been established.

As a check for literal copying, the five C source/header files here and the
added lines under `src/` and `tests/` were compared to the top-level C sources
and headers of the local LongMynd reference tree (excluding a local test
helper). After stripping comments and tokenizing identifiers, numbers, strings
and punctuation, there were no identical runs of 16 consecutive tokens.
This check cannot detect rewritten routines or establish independent origin
for constants, register sequences or algorithms.
