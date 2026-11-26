# FM-ATV

Select an IQ input, tune the analog television carrier, choose **Mode → FM-ATV** and start reception. The embedded panel and the selected HDMI monitor display the same 4:3 picture. The receiver uses the FM0 detector, adaptive horizontal and vertical synchronization, PAL de-emphasis and automatic colour-burst detection. Each decoded PAL field supplies 640 × 288 pixels. The internal panel reports sync and burst lock.

On selection, HackRF, B210, Lime and Pluto inputs configured below 12 MS/s switch to 20 MS/s for colour. The video IF uses up to 18 MHz of the available bandwidth. The PAL decoder enables colour at 12 MS/s or more with at least 10 MHz of video IF. Narrower inputs show monochrome. The supported IQ rate is 5–64 MS/s, subject to the SDR limits. Configure the carrier frequency and RF gain in Input. The same decoder accepts IQ recordings via Play file.

Above **10 MS/s**, click **Frame stacking** beside the spectrum bandwidth control to average the last six PAL fields on the internal and HDMI displays. Click again to return to live video. The same switch is available via `PUT /control/fmatv/framestack/on` and `PUT /control/fmatv/framestack/off`.

## Implementation

The files under `analog/` come from the [atv-receiver-analog product](https://github.com/pe1itr/atv-receiver-analog/tree/e51bb5a/src). Consult that project for receiver development. The worker adapts Linrad s16/u8 streams to its video and alignment paths and forwards decoded fields with the receiver's sync and burst flags. GTK and HDMI share the same field buffer. FM-ATV currently provides video only.
