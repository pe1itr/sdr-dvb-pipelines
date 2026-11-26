# FM-ATV

Select an IQ input, tune the analog television carrier, choose **Mode → FM-ATV** and start reception. The embedded panel and the selected HDMI monitor display the same 4:3 picture. The receiver defaults to the FM0 detector and uses adaptive horizontal and vertical synchronization, PAL de-emphasis and automatic colour-burst detection. Each decoded PAL field supplies 640 × 288 pixels. The internal panel reports sync and burst lock.

Choose the detector before or during reception using `PUT /control/fmatv/fmdetector/fm0`, `fm1` or `fm2` as the last path segment. FM1 uses the eight-point FFT phase detector. FM2 adds multi-candidate vertical sync to FM1. Service info displays the detector of the current picture, and `GET /status` reports the selected `fmatv_fmdetector`.

On selection, HackRF, B210, Lime and Pluto inputs configured below 12 MS/s switch to 20 MS/s for colour. The video IF uses up to 18 MHz of the available bandwidth. The PAL decoder enables colour at 12 MS/s or more with at least 10 MHz of video IF. Narrower inputs show monochrome. The supported IQ rate is 5–64 MS/s, subject to the SDR limits. Configure the carrier frequency and RF gain in Input. The same decoder accepts IQ recordings via Play file.

Above **10 MS/s**, click **Frame stacking** beside the spectrum bandwidth control to average the last six PAL fields on the internal and HDMI displays. Click again to return to live video. The same switch is available via `PUT /control/fmatv/framestack/on` and `PUT /control/fmatv/framestack/off`.

**Service info** shows grey for an inactive detector, blue while monitoring and green for detection. The 6 MHz FM and 6.552 MHz NICAM carrier indicators require at least 14 MS/s and 14 MHz of video IF. The 7.02, 7.22, 7.38 and 7.56 MHz carriers require 16 MS/s and 16 MHz. NICAM turns green after the receiver locks to valid NICAM frames. The frame stacking indicator turns green once six fields are buffered.

Line and raster frequencies are measured from horizontal and vertical sync. Video SNR is measured on synchronized PAL lines. A dash means the measurement is unavailable. The frame counter counts received PAL fields since playback started.

## Implementation

The reference receiver sources under `analog/` come from the [atv-receiver-analog product](https://github.com/pe1itr/atv-receiver-analog/tree/e51bb5a/src). Consult that project for receiver development. The worker adapts Linrad s16/u8 streams to its video and alignment paths and forwards decoded fields with their current detector states and PAL measurements. GTK and HDMI share the same field buffer. FM-ATV currently provides video only.
