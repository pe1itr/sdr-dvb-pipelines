# Receiver inputs

[Back to README](../README.md)

Commands below run from the repository root. The detailed notes are in Dutch unless stated otherwise.

### Native Afedri Net input

Kies **Input → Afedri Net** en druk op **Start SDR**. De standaardinstellingen
zijn `192.168.1.100`, TCP/UDP-poort `50005`, single-channel DDC1, 16-bit IQ,
`1010526` samples/s en 28 MHz. IP, poort en UDP-bindadres zijn instelbaar in Input;
**W** slaat de instellingen op in `[afedri]` van `linrad-dvb.ini`.
Vul het adres en de samplerate van je eigen ontvanger in. Er is geen SoapyAfedri-plugin of los Python-programma
nodig voor deze invoerbron.

De aansluiting opent TCP, controleert het apparaatantwoord en zet de UDP-ontvanger
klaar vóór het starten. Daarna volgen STOP, single-channel, samplerate, lange
UDP-pakketten, frequentie en RF-gain. De besturing wacht 100 ms per uitwisseling en 200 ms extra vóór START.
START gebruikt `08 00 18 00 80 02 00 01`; STOP gebruikt `08 00 18 00 80 01 00 01`.

UDP wordt door een eigen ontvangstthread in batches gelezen, met een begrensde
buffer van één seconde. Deze thread wacht niet op TCP-afstemming, spectrum, filter,
audio of decoder. De verwerking bundelt ongeveer 5 ms IQ (maximaal 8192 samples)
in plaats van per UDP-pakket van 256 samples een volledige 16384-punts filter-FFT
uit te voeren. Een onvolledig blok wordt binnen 10 ms doorgegeven. Dezelfde grotere
blokken gaan naar de USB/FM-monitor, zodat de 128 audio-bufferslots ook bij
piekgewijze netwerkontvangst bruikbaar blijven. De filterkwaliteit blijft gelijk.

Het log toont iedere seconde aparte tellers:

- `lost`: gaten in de ontvangen UDP-volgnummers; dit omvat eventuele socketdrops.
- `socket-drops`: door Linux gemelde overflow van deze UDP-socket.
- `input-queue-drops`: blokken die niet meer in onze eigen ontvangstbuffer passen.
- `zero-filled`: ontbrekende pakketten waarvoor nul-IQ is ingevoegd.
- `queued` / `peak`: actuele en maximale vulling van de ontvangstbuffer.

Zoals Linrad vult de ontvanger kleine gaten (minder dan 25 pakketten) met nul-IQ
om de sampletijd te behouden. Dat herstelt geen verloren signaal; verlies blijft
zichtbaar in `lost`. Grote sprongen worden niet met onbeperkte stilte opgevuld.
De decoderuitvoer en USB-monitor houden daarnaast hun eigen droptellers bij.

De status toont afzonderlijk **Online**, bevestiging van START en **Receiving IQ**.
De start/stopknop blijft geel totdat geldige UDP-IQ binnenkomt en wordt dan groen.
TCP-uitval, ontbrekende/ongeldige antwoorden en drie seconden zonder geldige IQ
geven een fout. Bij stoppen wordt STOP verzonden en worden beide sockets gesloten.
Gebruik de Afedri op dat moment niet tegelijk vanuit Linrad of een ander programma.

De ADC-klok wordt uitgelezen. Als de ingestelde samplerate niet overeenkomt met de
hardwaredeling, stopt de aansluiting met de juiste waarde in het log; vul die in
bij Input. Frequentie (0–35 MHz), LO-offset, gain (-10 tot +35 dB in stappen van
3 dB), spectrum, decoderfilter, audio-monitor en IQ-opname werken via de bestaande
pipeline. De aanwezige hardware-AGC-instelling blijft behouden. De ontvangst is
single-channel s16; `u8` is een uitvoerconversie. Ongeldige UDP-pakketten worden
verworpen, verlies wordt gemeld en dubbele/late pakketten worden overgeslagen.

Losse ontvangst, met Ctrl+C om te stoppen:

```bash
./build/sdr_to_stdout --source afedri --ip 192.168.1.100 --port 50005 \
  --sample-rate 1010526 --frequency 28000000 --gain 14 --format s16 > afedri.iq
```

`make test-afedri` test met een lokale TCP/UDP-simulator de exacte startvolgorde,
wachttijden, gefragmenteerde antwoorden, vroege IQ, s16/u8, opname,
pakketnummers, live afstemmen en fouten. Een vertraagd TCP-afstemantwoord wordt
getest met 6500 doorgaande IQ-pakketten. Een aparte test voert een bekende toon
via UDP, de echte USB-demodulator en PCM-uitvoer; ook bij 100-ms netwerkbursts
blijven sampleaantal en toonamplitude intact, zonder monitorbufferverlies.

Op de AFE822x-SDR op het ingestelde adres zijn TCP-identificatie, START, echte
UDP-IQ en STOP bevestigd bij 28 MHz en 1010526 samples/s. In twee korte metingen
met hetzelfde filter en spectrum daalde de CPU-tijd van de bron van 18,87 naar
2,97 seconden per 20 seconden meettijd (ongeveer 94% naar 15% van één kern).
Bij een vervolgmeting met de echte USB-audioketen bleven socket-, ontvangstbuffer-,
uitvoerbuffer- en monitorbufferdrops op nul. Er waren nog gaten in de aangeleverde
UDP-stroom over de wifi-verbinding; deze aanpassing garandeert dus geen verliesvrije
radio- of netwerkverbinding. Een onafhankelijke meting met de oorspronkelijke
Python-besturing en minimale UDP-lezer zag eveneens gaten (806 pakketten in
20 seconden), zonder deze C-filter- of audiocode. Dit is geen DVB-lock- of duurtest.

Na omschakelen van de laptop naar Ethernet daalde het pakketverlies in de
aangeleverde logs van circa 2,54% naar 0,38%. Alle gaten in die Ethernet-log
werden met nul-IQ opgevuld; socket- en ontvangstbufferdrops bleven nul.
Een passieve vervolgmeting tijdens ontvangst liet geen toename van Ethernet-,
IP- of UDP-fouttellers zien. De oorzaak van het resterende verlies is nog niet
vastgesteld; Ethernet heeft het verlies verminderd, maar niet volledig opgelost.

Protocolreferentie: [AFE822x / AFEDRI protocol extensions](https://www.afedri-sdr.com/index.php/downloads/category/2-documentation?download=242:afe822x-sdr-protocol-extensions).

### SDRplay RSP1A/RSP1B en Airspy HF+ Dual Port (VHF)

De C GUI heeft aparte ingangen **SDRplay RSP1A**, **SDRplay RSP1B** en
**Airspy HF+ Dual Port (VHF)**. CLI/config-namen: `rsp1a`, `rsp1b`, `airspyhf`.
Bestaande configuraties krijgen de ontbrekende secties automatisch met defaults;
zie ook `linrad-dvb.example.ini`. De integratie is met gesimuleerde drivers getest;
een praktijktest met deze ontvangers is nog nodig.

| Ingang | SDR/IF-afstembereik | Driver | Default sample rate |
| --- | --- | --- | --- |
| SDRplay RSP1A | 1 kHz–2 GHz | SoapySDRPlay3 + SDRplay API v3 | 2 MS/s |
| SDRplay RSP1B | 1 kHz–2 GHz | SoapySDRPlay3 + SDRplay API ≥3.14 | 2 MS/s |
| Airspy HF+ Dual Port, VHF | 64–260 MHz | SoapyAirspyHF + libairspyhf | 768 kS/s |

Installeer de betreffende driver apart. De buildcontrole meldt ontbrekende
Soapy-modules als waarschuwing. Gebruik voor SDRplay de API-v3-module, niet de
oude API-v2-module met dezelfde drivernaam. API 3.15 ondersteunt beide RSP's.
`args=driver=sdrplay,serial=SERIAL` of `args=driver=airspyhf,serial=HEX`
selecteert een apparaat; laat het serial-veld weg voor één aangesloten ontvanger.
Een ander aangesloten RSP-model dan de gekozen ingang blokkeert het starten.
SoapyAirspyHF identificeert de HF+-familie, maar onderscheidt Dual Port en
Discovery niet; dit profiel houdt altijd de Dual Port **VHF**-grenzen aan.
Sluit de antenne/converter dus aan op de VHF-connector.

Bij SDRplay is `gain` de **IF gain reduction**, 20–59 dB: hoger betekent minder
gain. RFGR staat op 0. De HF+ VHF-ingang gebruikt automatische gain (`gain=0`);
de gainpijlen veranderen daar niets. Beide ingangen gebruiken CF32 van de driver
met conversie naar `s16` of `u8` voor de bestaande decoderketen. De helper
controleert de werkelijke sample rate; een stilzwijgend afgeronde rate wordt
geweigerd. HF+-rates worden bij starten gecontroleerd tegen de lijst uit het apparaat.

De frequentiebeveiliging controleert de **SDR/IF-frequentie** en de digitaal
gekozen kanaalfrequentie binnen die IF-band, zowel bij starten als bij live
afstemmen. De converteroffset verandert alleen de RF-aanduiding:

`ontvangstfrequentie = SDR/IF + converteroffset + DVB receiver offset`

Voor 437 MHz via een converter naar 144 MHz: **SDR/IF = 144000 kHz**,
**LO offset = +293000 kHz**, **DVB receiver offset = 0**. Dat is geldig voor de
HF+ VHF-ingang; rechtstreeks 437 MHz instellen wordt geweigerd. Negatieve
converteroffsets werken eveneens; spectrumsinversie bij high-side mixing is
hiermee niet geïmplementeerd.

Bij alle directe SDR-ingangen moet de DVB-bandbreedte passen, **ook bij receiver
offset 0**. DVB-S/S2 gebruikt conservatief 35% roll-off: benodigde bandbreedte
= `1.35 × symbolrate`. DVB-T gebruikt de ingestelde kanaalbandbreedte. De
beschikbare breedte is maximaal `0.95 × sample rate`, verminderd met tweemaal de
absolute receiveroffset. Voor de HF+ geldt bovendien maximaal 660 kHz; bij
SDRplay telt ook de door de driver gekozen analoge filterbreedte mee.
Onmogelijke symbolrates/BW's staan grijs in de keuzelijst. Een ongeldige opgeslagen
instelling blijft zichtbaar met een foutmelding en blokkeert **Start SDR**;
ook `--run` en `--print-pipeline` weigeren deze combinaties.

Voorbeeld SDR-IQ: bij 196078 samples/s past 125 ksym/s (168750 Hz), maar
333 ksym/s (449550 Hz) niet. Bij 111111 samples/s past 66 ksym/s wel en
125 ksym/s niet. Een converteroffset vergroot de beschikbare bandbreedte niet.

Bronnen: [SDRplay API-specificatie](https://www.sdrplay.com/docs/SDRplay_API_Specification_v3.15.pdf),
[HF+ Dual Port-specificaties](https://airspy.com/airspy-hf-plus/),
[SoapySDRPlay3](https://github.com/pothosware/SoapySDRPlay3),
[SoapyAirspyHF](https://github.com/pothosware/SoapyAirspyHF).

### Airspy R2 in de C GUI

Kies **SDR → Input / SDR → Airspy R2**. De defaults zijn 437 MHz,
10.000.000 samples/s, gain setting 30 en `s16` IQ. Installeer **SoapyAirspy**
en **libairspy**; de HF+-driver is een aparte module. CLI/config-naam:
`airspyr2`. Bestaande configuraties krijgen de sectie automatisch.

- Afstembereik: **24–1700 MHz**, ook gecontroleerd tijdens live afstemmen.
- Sampleratekeuze: **10 MS/s** of **2,5 MS/s**. Die laatste modus noemt Airspy
  experimenteel; gebruik 10 MS/s als uitgangspunt. Bij starten moet de gekozen
  rate ook voorkomen in de lijst die het aangesloten apparaat meldt.
- Gain setting **0–45**, in gehele stappen. Dit is de gecombineerde
  Soapy-instelling voor LNA, mixer en VGA, geen gekalibreerde dB-schaal.
  AGC staat uit; de gainpijlen werken ook tijdens ontvangst.
- De driver levert native `CS16`; de helper behoudt de samples voor `s16`
  of converteert ze naar `u8`. Beide uitvoerformaten zijn ondersteund.
- `args=driver=airspy,serial=HEX` selecteert een specifieke ontvanger;
  zonder serial wordt een beschikbaar apparaat gebruikt. Bias tee staat
  standaard uit; `biastee=true` in de device arguments schakelt hem in.

Voor de bandbreedtebeveiliging gebruiken we **90% van de samplerate**:
9 MHz bij 10 MS/s, en conservatief 2,25 MHz bij 2,5 MS/s. De gekozen
symbolrate, 35% roll-off en digitale receiveroffset moeten daarin passen.
De converteroffset verandert alleen de RF-aanduiding, niet deze grenzen.
Voorbeelden: 5 Msym/s past bij 10 MS/s; 2 Msym/s past niet bij 2,5 MS/s.

SoapyAirspy meldt de familie als `Airspy`; het onderscheid R2/Mini wordt
niet door deze interface gerapporteerd. Het profiel gebruikt daarom de
R2-specificaties en controleert de beschikbare rates bij het aangesloten
apparaat. Firmware met andere rates valt buiten dit profiel.
De integratie is getest met gesimuleerde drivers; een praktijktest met de
R2 is nog nodig. Begin bijvoorbeeld op 437 MHz, 10 MS/s, DVB-S2 333 ksym/s,
met de FEC passend bij het ontvangen signaal.

Bronnen: [Airspy R2-specificaties](https://airspy.com/airspy-r2/),
[SoapyAirspy-instellingen](https://github.com/pothosware/SoapyAirspy/blob/master/Settings.cpp),
[SoapyAirspy-streamformaten](https://github.com/pothosware/SoapyAirspy/blob/master/Streaming.cpp).

### RTL-SDR in de C GUI

Kies **SDR → Input / SDR → RTL-SDR** en druk op **Start SDR**. De standaard
is 437 MHz, 2.400.000 samples/s, gain 30 dB en `u8` IQ. De SoapyRTLSDR-driver
moet geïnstalleerd zijn; er is geen apart `rtl_sdr`-proces nodig. De aangesloten
RTL2838 met R820T-tuner wordt via `driver=rtlsdr` geopend. Bij meerdere dongles
kun je in **Device arguments** bijvoorbeeld `driver=rtlsdr,serial=1007` invullen.

Deze invoer gebruikt de normale tunermodus voor R820T/R820T2/R828D:
24–1764 MHz en handmatige gain 0–49,6 dB. Pijltjes links/rechts stemmen af;
pijltjes omhoog/omlaag wijzigen de gevraagde gain. De driver past de gain aan
de hardware aan. **W** bewaart de instellingen in `[rtlsdr]`.
De sample-ratekeuzelijst biedt onder andere 250.000, 1.024.000, 2.048.000 en
2.400.000 samples/s. Via het configuratiebestand zijn ook andere gehele rates
binnen 225.001–300.000 en 900.001–3.200.000 mogelijk. De ongeldige tussenruimte
wordt geweigerd. Controleer bij hogere rates de overflow- en dropmeldingen.

Spectrum, DVB-S/S2/T, bandfilter, audiomonitor en **Record IQ** gebruiken dezelfde
invoerroute als de andere ontvangers. `u8` en `s16` worden beide ondersteund.
SoapyRTLSDR levert signed CS8 voor `u8`; de helper zet dit om naar unsigned IQ.
Zie de [SoapyRTLSDR-streamimplementatie](https://github.com/pothosware/SoapyRTLSDR/blob/master/Streaming.cpp)
en [rate-instellingen](https://github.com/pothosware/SoapyRTLSDR/blob/master/Settings.cpp).

```sh
./build/linrad_dvb --source rtlsdr
./build/sdr_to_stdout --source rtlsdr --sample-rate 2400000 \
  --frequency 437000000 --gain 30 --format u8 > rtl.u8
```

### FUNcube Dongle Pro+ in de C GUI

Selecteer **FUNcube Pro+** onder **SDR → Input / SDR** (`--source fcdpp`).
De Pro+ heeft een vaste bronrate van **192000 samples/s**, stereo 16-bit IQ.
De keuzelijst biedt uitsluitend die rate; de oorspronkelijke Pro met 96 kS/s
is een ander model en wordt door deze bron niet geopend. Begin bijvoorbeeld met
**DVB-T, bandwidth 40000, FEC 1/2, GI 1/32**. De DVB-bandbreedte/symbolrate is
een aparte instelling: een breed DVB-signaal past niet in de smalle FUNcube-band.

De C-helper gebruikt Linux **hidraw** voor de tuner en de bijbehorende **ALSA
hardware-capturekaart** voor IQ. Er is geen extra SoapyFCDPP-plugin nodig.
De USB-identiteit `04d8:fb31` en de gedeelde USB-parent koppelen beide interfaces,
ook als de ALSA-kaartnummers veranderen. De audio wordt rechtstreeks op 192 kS/s
gelezen, zonder desktop-audiomixer of resampling. Toegang tot zowel `/dev/hidrawN`
als de ALSA-capturekaart is nodig; een bezet apparaat of ontbrekende toegang
wordt in het log gemeld.

Standaard: 437 MHz, IF gain 20 dB, `s16`, LNA aan en mixer hoog. De gainpijlen
regelen **IF gain 0–59 dB** in stappen van 1 dB. Onder **Device arguments** kun
je `lna=0,mixer=1` gebruiken; beide schakelaars accepteren `0` of `1`.
Bij meerdere Pro+-dongles selecteer je bijvoorbeeld
`hid=/dev/hidraw2,lna=1,mixer=1`; de audiokaart wordt daarbij automatisch gevonden.
Frequentiebereik: 150 kHz–240 MHz en 420–1900 MHz. Bias-tee wordt niet gewijzigd.
De tunercommando's volgen het [FUNcube Pro+-protocol in SoapyFCDPP](https://github.com/pothosware/SoapyFCDPP/blob/master/SoapyFCDPP/fcdcmd.h).

**Spectrum valt weg bij hogere IF gain:** verlaag eerst de IF gain; 20 dB is
de standaard. Stop de ontvangst en probeer desgewenst `lna=1,mixer=0` onder
**Device arguments**, en start opnieuw. Het toegestane instelbereik 0–59 dB
is geen garantie dat iedere combinatie van versterkerstanden bruikbaar is.
Op de aangesloten Pro+ met firmware 20.03 viel bij mixer hoog rond 29 dB eerst
het I-kanaal weg en bij 30 dB ook Q. Met mixer laag bleven beide kanalen bij
30 en 40 dB werken; bij 50 dB viel I opnieuw weg. Dit is gemeten in ruwe
`s16`-samples op 144,600 en 437 MHz, met correcte gain-teruglezing, en wijst
op interne oversturing. Alleen de LNA uitschakelen hielp hierbij niet.
Een DVB receiver offset of DC-correctie kan een al weggevallen ingangssignaal
niet herstellen. De genoemde grenzen zijn metingen aan dit exemplaar,
geen algemene gainlimieten voor alle Pro+-dongles of antennesituaties.

Live afstemmen, IF gain, IQ-filter, spectrum/waterval, audiomonitoring en de
IQ-opnameknop gebruiken dezelfde pipeline als de andere directe bronnen.
`u8` is beschikbaar voor decoderinvoer; IQ-opnamen bewaren de originele `s16`
samples met 192000 Hz in de JSON-metadata. **W** bewaart de instellingen in
`[fcdpp]`. Een oudere configuratie krijgt automatisch de nieuwe standaardwaarden.

```bash
./build/linrad_dvb --source fcdpp --standard DVB-T --sr 40000 --fec 1/2 --gi 1/32
```

### Direct HackRF, B210 and SDR-IQ inputs in the C GUI

Build with `make all` and launch `./build/linrad_dvb`. The build requires GTK 3,
GLib, json-c, FFTW3 single precision (fftw3f), SoapySDR and UHD development packages; HackRF reception also requires the
SoapyHackRF driver; SDR-IQ requires the SoapyRFSpace driver. Select `linrad`,
`hackrf`, `b210`, `sdriq` or **Bestand (IQ)** in **SDR → Input / SDR**. With reception
stopped, set sample rate (samples/s), frequency (kHz), and IQ format.
The gain readout is in the top bar; use the up/down arrow keys to adjust it.
Frequency values in `linrad-dvb.ini` remain in Hz.
Start opens the selected SDR; Stop closes the receive pipeline. Frequency and gain
are disabled for Linrad, where tuning is done in Linrad itself. GUI edits are kept per source; press **W** to save them to the active INI file
and restore them on the next start.

The frequency readout in the top bar shows **DVB receive frequency = SDR frequency + LO offset + DVB receiver offset**.
Set **LO-offset (kHz)** in the **SDR** menu for the converter: 28 MHz IF plus 408000 kHz offset
shows 436.000000 MHz. The **SDR / IF (kHz)** field is also in the **SDR** menu.
Below these settings, a live calculation shows the hardware frequency, converter
LO offset, DVB receiver offset, receive frequency and spectrum center. The top bar
shows the received RF frequency and gain alongside Start/Stop.
The offset changes the readout and spectrum/waterfall axes; the hardware always receives the IF frequency.
Signed offsets are supported; this is an additive offset, not a high-side mixer
inversion mode.

**DVB receiver offset (kHz)** is a separate setting in **SDR**, available for
direct hardware sources while stopped. It selects a DVB channel above (positive)
or below (negative) the hardware center. **0 bypasses the frequency conversion
entirely**, retaining the existing sample processing. **W** saves the offset per
source as `receiver_offset` in **Hz**; older configurations default to zero.

For example, with no converter LO offset:

| Action | SDR / spectrum center | DVB frequency in the top bar |
| --- | --- | --- |
| Offset 0 | 144.600 MHz | 144.600 MHz |
| Set DVB offset +50 kHz | 144.600 MHz | 144.650 MHz |
| Tune down 50 kHz with the arrows | 144.550 MHz | 144.600 MHz |

The spectrum/waterfall retains its symmetric full hardware span. The DVB filter
outline and channel marker move by the receiver offset. The IQ samples sent to
the DVB decoder are digitally shifted in the opposite direction, **before** the
existing channel filter; the sample rate stays the same. No new DC-removal or
notch filter is added. Mixing and filtering use floating point before the final
s16/u8 quantization, preserving headroom; u8 is rounded symmetrically to avoid
introducing a new DC bias. Offset 0 follows the original path bit for bit.

The offset plus occupied DVB bandwidth and any enabled IQ-filter width must fit
inside the source band, with 2.5% of the sample rate reserved at each edge. For
DVB-S/S2 the check uses 0.35 roll-off (occupied width = 1.35 × symbol rate).
Invalid combinations are reported and cannot start reception; the GUI stays
available to correct them. At 192 kS/s, +50 kHz with DVB-T 40 kHz fits;
DVB-S 125 ksym/s with that offset does not. A live IQ-filter widening that would
cross the source-band edge is rejected. Enable a suitable IQ filter when needed
to reject the displaced DC peak before the decoder.

This offset applies **only to DVB**. USB/FM monitoring continues to use raw IQ
and its own frequency selection. IQ recordings also remain raw, before shifting
or filtering: their frequency metadata describes the hardware center (plus any
converter LO), not the offset DVB channel. Full-band playback therefore still
shows that original center; the DVB offset is not automatically applied to an IQ
file. Screenshot filenames use the displayed DVB receive frequency.

Use the keyboard left/right arrow keys to tune by **Afstemstap (Hz)**,
configured in the **SDR** menu (default 1000 Hz). Plain arrows tune whenever the launcher window is active,
including when a text/number field has focus. Ctrl/Alt-modified arrows retain
their normal editing behavior. **Up/down** increases/decreases RX gain, also
while receiving: SDR-IQ uses its ATT steps (-20, -10, 0, +10 dB), while HackRF
and B210 requests change by 1 dB within their allowed ranges. Hardware may
quantize gain; the helper logs the applied gain. The gain label shows the
requested setting. Direct
HackRF, B210 and SDR-IQ inputs can retune while receiving without restarting the
pipeline. Samples already buffered can still belong to the previous frequency,
and the decoder may need to reacquire. Successful device tuning is logged as
`[SDR] Tuned to ... Hz`; the large readout shows the requested setting, not a
measured RF frequency. Linrad UDP tuning remains in Linrad and the readout says so.

`lo_offset` and `tune_step` in each SDR section of `linrad-dvb.ini` are in **Hz**.
Both default to 0 and 1000 respectively for older configs.
Press **W** (or **w**) in the active launcher window to save the current settings,
including all edited SDR sections, selected input, DVB standard, rate/bandwidth,
constellation, FEC, guard interval and Scatterfix. This also works during reception.
The active INI file (including one selected with `--config`) is replaced atomically;
comments and unrelated settings are preserved. Success or failure appears in the
status line. On the next start the `[session]` selection is restored, without
starting reception automatically. Explicit command-line selections override saved
values; `--no-scatterfix` disables a saved Scatterfix selection. Changing the
standard via CLI resets incompatible mode defaults unless explicitly supplied.
Closing without W leaves the previous saved settings intact.
The internal `sdr_to_stdout --control-stdin` option accepts a frequency in Hz or `gain DB`
per line on a private pipe from the launcher; stdout remains IQ only.
Live gain commands always use a decimal point, including with Dutch or other
comma-decimal desktop locales, so B210, HackRF, SDR-IQ and AFEDRI gain changes
take effect without stopping and starting reception.

The **IQ-bandfilter** checkbox enables a digital filter before the DVB decoder,
for all sources including Linrad UDP. **[** narrows and **]** widens its total
bandwidth by 5% and enables it. You can also enter the total width in Hz directly;
uncheck the box for bypass. Width and enable state are saved per input with **W**.
The sample rate, IQ format and number of samples sent to the decoder stay the same.
The filter is centered at zero in complex baseband (the SDR tuning frequency).
Linrad recordings/measurements represent the filtered output when enabled.

The filter is an **8193-tap symmetric, real, Blackman-windowed FIR** applied
identically to I and Q, implemented using FFT overlap-save. Its fixed group delay
is **4096 samples**, regardless of filter width (36.864 ms at 111111 samples/s).
Changing width preserves input history and does not change group delay. Enabling
or bypassing changes the signal delay and may cause reacquisition; changing width
can also create a short transition. Bypass preserves the original bytes exactly.
Width denotes the full **-6 dB** bandwidth, not a brick-wall passband: leave margin
around the wanted signal. Allowed widths are max(10 Hz, sample-rate/256) through
95% of sample rate. There is no resampling, decimation, or frequency-dependent
phase correction. Fixed symmetric coefficients provide linear phase in the
passband. Hardware throughput and RF lock with the filter need live testing.

For **SDR-IQ with DVB-T 40 kHz**, select `sdriq`, DVB-T, bandwidth `40000`,
FEC `1/2` and GI `1/32` (matching the transmitter). The defaults are 111111
samples/s, 28000 kHz, ATT gain +10 dB and `s16`. Set frequency to the converter
output, not its UHF input. `[sdriq] args` defaults to
`driver=rfspace,sdr-iq=/dev/ttyUSB0`; change the device path if needed.
The SDR-IQ sample-rate selector offers 8138, 16276, 37793, 55556, 111111,
158730 and 196078 samples/s. For 40 kHz DVB-T, start at 111111 or 196078;
the lowest rates do not cover this signal. RF ATT gain is -20, -10, 0 or +10 dB.
For the existing DVB-S2 125 ksym/s use case, select 196078 samples/s.

SDR-IQ reception is implemented in C in `sdr_to_stdout`, using SoapyRFSpace CF32
with clipped, rounded CF32-to-s16 conversion. Optional `u8` uses the common
C helper's full-scale s16-to-u8 conversion.
Small 2048-sample reads avoid the latency of the high-rate SDR blocks.
The decoder receives the selected source sample rate; `--resample-to-dvbt-rate`
converts this to about 45714.286 samples/s for 40 kHz DVB-T at `--dvbt-ir 1`.
Integer rounding of SDR-IQ rates is tolerated up to 0.5 samples/s; other rate
changes are rejected. The compiled-in DVB-T receiver supports `--sr 40000`;
this mode still needs an RF reception test.

DVB-T bandwidth **35 kHz** is also selectable in the C GUI as `35000`
(or `--standard DVB-T --sr 35000`). Keep the SDR-IQ source sample rate at
111111 samples/s; the receiver resamples to 40000 samples/s internally for
35 kHz bandwidth at `--dvbt-ir 1`. The compiled-in receiver includes 35k support.

```bash
./build/linrad_dvb --source sdriq --standard DVB-T --sr 40000 --fec 1/2 --gi 1/32
```

For the first 5 MSym/s test, select DVB-S2, symbol rate `5000000`, FEC `3/4`, and
sample rate `10000000`. This selects **QPSK 3/4** (`--modcods 0x80`). Set frequency
and gain for the actual signal. The DVB-S2 arguments use `--framesizes 3` in
`[leandvb]` to accept both normal and short frames.

HackRF defaults to `u8`: native signed 8-bit samples are offset to unsigned IQ
without increasing their size. B210 defaults to `s16` to preserve its resolution;
`u8` is also selectable but discards precision. At 10 MS/s these formats carry
20 MB/s and 40 MB/s respectively. LeanDVB's normal decoder converts both formats
to float internally, so the smaller pipe traffic is not a guarantee of faster
DVB-S2/LDPC decoding. B210 USB transport remains `sc16` with either output format.

### LimeSDR Mini v1.2 ontvangst

Kies **SDR → Input / SDR → LimeSDR Mini v1.2**, of start met
`./build/linrad_dvb --source lime`. De native LimeSuite-aansturing volgt de
apparaatinitialisatie van `datv-transmitter`, met RX op kanaal 0 en TX uit.
Deze backend accepteert de originele Mini v1.x, inclusief v1.2.

- Samplerate: **100000–30720000 samples/s**; standaard 10 MS/s.
- Frequentie: **10 MHz–3,5 GHz**; standaard 437 MHz.
- Gain: **0–73**, stappen van 1 op de unsigned LimeSuite RX-gainschaal.
  De hardwarewaarde wordt teruggelezen en gelogd; de instelling is geen
  gekalibreerde absolute RF-versterking.
- **Device arguments**: leeg bij één apparaat, of `serial=SERIAL` met het
  volledige serienummer. Bij meerdere apparaten is selectie verplicht.
- **Antenna**: `AUTO` kiest LNAW onder 2 GHz en LNAH vanaf 2 GHz op dezelfde
  RX-SMA-aansluiting; `LNAW` en `LNAH` kunnen ook expliciet worden gekozen.
- IQ is standaard `s16`, met behoud van de 12-bit ADC-resolutie; `u8` is
  beschikbaar met verlies van precisie. Spectrum, audio-monitor, opname,
  digitale filtering en DVB receiver offset gebruiken de bestaande IQ-keten.

Frequentie en gain werken ook tijdens ontvangst. Bij afstemmen pauzeert de
RX-stream voor kalibratie en start daarna opnieuw; de decoder moet opnieuw
locken. Voor smalle DATV-instellingen kalibreert de driver op een hogere
samplerate en zet vervolgens de gekozen samplerate terug. Het analoge RX-filter
is minimaal 1,5 MHz; gebruik voor een smal kanaal het digitale IQ-filter.
Een afwijkende hardware-samplerate wordt geweigerd. **W** bewaart de
instellingen onder `[lime]`; bestaande configuraties krijgen automatisch defaults.

Installeer de klassieke **LimeSuite** ontwikkelbestanden (`LimeSuite.pc`,
`lime/LimeSuite.h`, `libLimeSuite`) en bouw opnieuw. SoapyLMS7 is niet nodig.
Zonder LimeSuite blijft de rest van het programma bouwen, maar geeft de
LimeSDR-ingang bij starten een duidelijke foutmelding. Voor de lokale
installatie die ook `datv-transmitter` gebruikt:

```bash
export PKG_CONFIG_PATH="/path/to/limesuite/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
python3 tools/build.py -j4
./build/linrad_dvb --source lime
```

De build neemt het librarypad op in de RX-helper. Het project vereist geen
naastgelegen checkout als LimeSuite via de normale pkg-config-paden beschikbaar
is. Bij wisselen van LimeSuite-installatie of bouwen zonder LimeSuite wordt de
helper automatisch opnieuw gebouwd.

`make test-lime` test de native API met een gesimuleerd apparaat tegen de echte
LimeSuite-headers, zonder USB te openen. De algemene tests controleren ook
GUI-bediening, pipelineopbouw, configuratie en invoergrenzen. RF-ontvangst en
duurzame doorvoer op 30,72 MS/s moeten op echte hardware worden geverifieerd.
Hardwarelimieten: [Mini v1.2 specificaties](https://myriadrf.org/projects/limesdr-mini/)
en [RF-paden](https://myriadrf.org/projects/limesdr-mini/reference/v1.2/rfcontrol.html);
API: [LimeSuite](https://github.com/myriadrf/LimeSuite).

### B210 input and shared IQ buffering

The B210 input follows `atv-receiver-analog`: UHD, channel 0, internal clock, and
Kintex-7 images from `[b210] images` (default
`/path/to/uhd-images`). This directory must contain the matching
`usrp_b210_fpga.bin` and `usrp_b200_fw.hex`. `[b210] antenna` selects `TX/RX` or `RX2`;
`args` supplies UHD device selection. `[hackrf] args` supplies Soapy device selection.
The helper rejects a coerced sample rate instead of silently feeding the decoder
at the wrong rate. An independent output thread and a bounded 500 ms IQ queue keep USB reception
running during short decoder stalls (about 20 MB at 10 MS/s with `s16`). Capacity
is counted in bytes, so short hardware reads do not waste buffer space. The log
reports RX rate, bytes delivered to the decoder expressed as MS/s, actual queued
milliseconds and peak queue occupancy once per second and keeps a cumulative count of dropped samples.
These measurements distinguish an output bottleneck from a hardware overflow.
Hardware
overflows are counted and logged, and reception continues so the decoder can
reacquire after lost samples. When the output queue fills, new complete IQ blocks are dropped and counted;
previously queued bytes remain intact and reception continues. The first drop is
logged immediately, followed by periodic throughput/drop counters. This keeps the
receiver running while searching in noise. Sample gaps can cause loss of lock and
TS packets; sustained drops with a real signal still require throughput diagnosis.
Other receive errors remain fatal. Sustained 5 MSym/s decoding still needs a
hardware/CPU throughput test.

Preview without opening hardware:

```bash
./build/linrad_dvb --source hackrf --standard DVB-S2 --sr 5000000 --fec 3/4 --print-pipeline
./build/linrad_dvb --source b210 --standard DVB-S2 --sr 5000000 --fec 3/4 --print-pipeline
```

`make test` checks pipeline construction, GUI source/parameter changes, and process
lifecycle using fake decoders; it does not claim an RF lock or hardware throughput.


The **Constellatie (DVB-S2)** selector supports QPSK and 8PSK. The FEC list adapts
to the selection: 8PSK supports 3/5, 2/3, 3/4, 5/6, 8/9 and 9/10; QPSK also
supports 1/4, 1/3, 2/5, 1/2 and 4/5. Switching keeps the selected FEC if valid;
otherwise it selects 2/3 for 8PSK or 1/2 for QPSK/DVB-S/DVB-T. This selector only
controls DVB-S2 and is disabled for the other standards.
For 5 MSym/s **8PSK 3/4**, select 8PSK and FEC 3/4 (MODCOD 14, mask `0x4000`):

```bash
./build/linrad_dvb --source b210 --standard DVB-S2 --sr 5000000 --constellation 8PSK --fec 3/4
```

The CLI rejects unsupported constellation/FEC combinations. Omitting
`--constellation` retains QPSK; 8PSK without `--fec` defaults to 2/3.


`make test-iq-output` checks the threaded IQ queue with a stalled pipe consumer,
byte-for-byte delivery after reads resume, queue saturation, Stop while blocked,
and propagation of a closed decoder pipe. The queue absorbs short stalls; it
cannot compensate for a decoder that is continuously slower than the SDR.


DVB-S2 automatic acquisition omits `--fastlock` above 333 ksym/s and enables
it at narrowband contest rates. Without a valid signal, aggressive
frame searches can be much slower than decoding an already acquired clean signal.
The messages `Unsupported or corrupted FEC` / `MODCOD` can arise from false
physical-layer-header detections in noise, including operation without an antenna.
They do not by themselves establish that the transmitter uses the wrong FEC.
Overflow recovery is not a guarantee of error-free reception: check that the
`dropped` counter stops increasing once a valid signal has been acquired.


`make test-iq-conversion` verifies the actual C conversion routines used by the
SDR helper against the sample-format conversion rules. It checks
all 256 signed-byte representations, all 65,536 signed 16-bit values, block
boundaries, zero/quantization edges and asymmetric I/Q pairs. HackRF CS8 becomes
u8 by XOR 0x80. UHD host `sc16` is passed unchanged to `--s16`; conversion to u8
maps -32768 to 0, 0 to 128 and 32767 to 255, retaining I/Q order. These are format
tests; they do not validate RF levels, physical I/Q orientation or the attached
B210 firmware with a known signal.
