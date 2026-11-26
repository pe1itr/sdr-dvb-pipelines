# Native Windows x64

## Optionele GPU-LDPC voor DVB-T2

In het modevenster staat **Use GPU for DVB-T2 LDPC (experimental)**. Uit blijft
de bestaande CPU-decoder actief, zonder GPU-initialisatie. Aan probeert de
ontvanger Direct3D 11-LDPC; bij een ontbrekende GPU, mislukte zelftest of
GPU-fout wordt de CPU gebruikt. De keuze wordt opgeslagen en geldt bij de
volgende ontvangststart. DVB-S2 en de keuze van videorenderer staan hier los van.

Service Info toont `LDPC: GPU`, `CPU` of `CPU (fallback)` na een verwerkt frame.
Het statusendpoint bevat `ldpc_gpu_requested`, `ldpc_backend`,
`ldpc_gpu_frames` en `ldpc_cpu_frames`. GPU betreft de eerste LDPC-poging;
BCH en eventuele herkansingen blijven op CPU. Het is een proefversie zonder
aangetoonde versnelling op de Windows-laptop.

Met `check-dvbt2-gpu.cmd` kan de GPU-zelftest afzonderlijk worden uitgevoerd.
Sluit de ontvanger daarvoor af; het resultaat staat in `dvbt2-gpu-check.log`.
De zelftest controleert bekende korte en normale codewoorden. Een geslaagde
zelftest is nog geen volledige vergelijking van ontvangstkwaliteit of snelheid.

Dit is een experimentele native Windows-port van Linrad DVB. Het ZIP-package
bevat echte Windows `.exe`-bestanden, DLL's en plugins. WSL en MSYS2 zijn niet
nodig om het programma te gebruiken. Gebruik Windows 10/11, 64-bit.

## Starten

1. Pak **de volledige ZIP** uit in een schrijfbare map, bijvoorbeeld
   `C:\Users\Rob\Apps\LinradDVB`. Start niet vanuit de ZIP-weergave.
2. Start `linrad_dvb.exe`. Bij de eerste start wordt `linrad-dvb.ini` uit
   `linrad-dvb.example.ini` aangemaakt. Het consolevenster bevat foutmeldingen
   en decoderlogs.
3. Kies **Input**, **Mode** en **Start SDR**. Met **W** bewaar je de instellingen
   in `linrad-dvb.ini` naast de executable.
4. Houd de DLL's, `plugins/`, `lib/` en `share/` bij het programma. Je kunt de
   volledige map verplaatsen. Een nieuwe ZIP bevat alleen de voorbeeldconfiguratie;
   uitpakken over een bestaande installatie overschrijft je eigen INI niet.

Onder het constellatiescherm staan tien presetknoppen (twee kolommen, vijf rijen).
Via **Presets** stel je per knop een label in en sla je met **Save current settings**
de actuele ontvangerinstellingen op. Dit overschrijft de gewone INI niet.
Sluiten van het venster bewaart de labels. De presets staan in
`linrad-dvb.ini.presets.ini`; bewaar dit bestand ook bij updates of verplaatsingen.
Een preset kiezen stopt eerst de ontvangst. Start daarna zelf met **Start SDR**.
De gekozen knop is groen; handmatig gewijzigde instellingen heffen die selectie op.
Opnieuw starten maakt dan geen preset actief. **W** blijft de gewone INI opslaan.
Logboek-/stationinstellingen en de HTTP-statusinstellingen gelden voor alle presets.

De decoders zijn beschikbaar voor DVB-S, DVB-S2, DVB-T, DVB-T2 en DAB/DAB+.
Het DVB-T2-bereik blijft gelijk aan de Linux-versie: de ondersteunde 8 MHz
broadcastmodi. Ingebouwde video gebruikt de meegeleverde libVLC-plugins.
Audio monitoring gebruikt het standaard Windows-audioapparaat via PortAudio;
laat `[usb] device=default` staan.

DAB gebruikt dezelfde decoder als Linux: DAB+ EEP-A/B, stationselectie,
DLS-tekst, MOT-slideshow (PNG/JPEG), constellatie, FIC/TII, DX-profielen,
vrije frequentie, kanaalwissels en TII-logboek. DAB-audio gebruikt FAAD2 en
PortAudio; `[dab] audio_device=default` kiest de standaarduitgang. Een exacte
Windows-afspeelapparaatnaam kan ook worden ingevuld. Klassieke DAB MP2 en UEP
worden, net als op Linux, niet ondersteund.

## Invoer en drivers

| Invoer | Windows-package |
| --- | --- |
| Linrad UDP | Ingebouwd; stem IP, UDP-poort, sample rate en IQ-formaat af op Linrad |
| IQ-bestand | Ingebouwd; s16, u8 en cf32, inclusief JSON-metadata |
| AFEDRI Ethernet | Ingebouwd; dezelfde TCP/UDP-instellingen als op Linux |
| HackRF | SoapyHackRF en libusb meegeleverd; passende Windows USB-driver nodig |
| Pluto / PlutoSky RX1 | Native libiio 0.26 meegeleverd, dezelfde aansturing als atv-receiver-analog; USB-auto of netwerk-URI |
| RTL-SDR | SoapyRTLSDR en libusb meegeleverd; passende Windows USB-driver nodig |
| B210 / B210-compatibel Kintex-7 | UHD meegeleverd; passende USB-driver en de juiste board-specifieke FPGA/firmware-images nodig |
| FUNcube Pro+ | Native HID-besturing en exclusieve WASAPI-IQ-invoer op 192 kS/s; Windows HID/USB-audiodriver |
| LimeSDR Mini v1.x | Native LimeSuite 23.11 en FTD3XX meegeleverd; passende FTDI D3XX USB-driver nodig |
| SDRplay, Airspy, RFSpace | Vereisen aanvullende compatibele SoapySDR 0.8 Windows-modules en leveranciersdrivers; niet meegeleverd/gevalideerd |

Voeg alleen passende 64-bit SoapySDR 0.8-modules toe aan
`lib/SoapySDR/modules0.8`; eventuele afhankelijkheden moeten ook beschikbaar
zijn. Linux `.so`-drivers werken niet op Windows. B210-compatibel Kintex-7,
HackRF en RTL-SDR zijn inmiddels met live ontvangst op een echte Windows-pc
getest; dit valideert niet alle bovenstaande radio's en drivers. Zie de
[geteste computers](../README.md#tested-computers-and-practical-limits) en de
[resterende DVB-T2-beperkingen](../README.md#dvb-t2-on-windows).
Windows ondersteunt geen Linux
`SO_RXQ_OVFL`-teller; AFEDRI telt wel de ontvangen sequentiegaten en queuedrops.

FUNcube koppelt HID en audio via hun Windows-apparaatcontainer en gebruikt
uitsluitend onbewerkte stereo-IQ op 192 kHz. Sluit bij automatische selectie
één Pro+ aan; bij meerdere apparaten kan `hid=WINDOWS_HID_PATH` worden gekozen.
`lna=0|1` en `mixer=0|1` werken zoals op Linux. Linux-apparaatnamen zoals
`/dev/hidrawN` en ALSA `hw:...` zijn niet overdraagbaar naar Windows.
De SDR-helper laadt `libLimeSuite.dll` alleen bij de keuze voor LimeSDR Mini.
De native Mini-backend verzorgt ook de RX-kalibratie; er wordt
geen firmware bijgewerkt. B210 mixer/LO-instellingen zijn eveneens beschikbaar.

Een Kintex-7-clone gebruikt andere FPGA-images dan een originele Ettus B210.
Gebruik de bestanden die al bij die ontvanger werken. Een persoonlijk pakket
kan die met `--b210-images /pad/naar/uhd-images` opnemen; de Windows-INI wijst
dan naar `./uhd-images/kintex7`, met SHA-256-controles naast de bestanden, en
vult bij Device arguments `type=b200` in. Bewaar eigen wijzigingen met **W**.
De generieke build vervangt die bestanden niet door standaard Ettus-images.

De SDR-uitvoerschrijver wacht bij een volle Windows-pipe kort en onderbreekbaar
(gevraagde wachttijd 1 ms, werkelijke hervatting afhankelijk van de scheduler).
Dit voorkomt voortdurend opnieuw proberen wanneer Windows schrijfbereidheid
meldt maar de pipe geen bytes aanneemt. Alleen de afzonderlijke schrijver
wacht; de SDR-ontvangstthread blijft de bestaande begrensde uitvoerqueue vullen.
De schrijver vraagt bij het openen eenmalig de Windows-pipecapaciteit op en
begrenst grote schrijfverzoeken daarop. Dit voorkomt dat een opgehoopte queue
als één verzoek groter dan de volledige pipe wordt aangeboden; onder Wine
kan zo'n verzoek zelfs bij een lege pipe worden geweigerd.
Een gesloten lezer wordt als pipefout afgehandeld, ook wanneer Windows
`ERROR_NO_DATA` bij schrijven meldt; daarop wordt niet eindeloos opnieuw geprobeerd.
Een volle uitvoerqueue blijft nieuwe blokken verwerpen en als drops tellen.
Er worden geen headers of onderbrekingsmetadata aan de ruwe IQ-stream toegevoegd.
De startmelding is `IQ writer: interruptible backpressure wait`; alleen bij
dergelijke wachttijden verschijnt een intervalteller in het bestaande SDR-log.
Deze optimalisatie bewijst niet dat de UHD/USB-overflows zijn opgelost:
hardware-overflows, uitvoerqueue-drops en decoder-inputdrops blijven aparte
verliespunten. De uitvoerqueue bevindt zich na UHD en kan hardwareverlies
dus niet opvangen.

Video, EPG/teletekst, TS- en IQ-opname, SRT-uitvoer via FFmpeg, USB/FM-monitoring,
PCM-UDP, TCI en het ontvangstlogboek gebruiken dezelfde applicatiecode als Linux.
Externe spelers en optionele leveranciersdrivers moeten, net als bij een
Linux-installatie, aanwezig zijn wanneer die uitvoer of hardware wordt gekozen.

Voorbeeld zonder GUI:

```powershell
.\linrad_dvb.exe --print-pipeline --standard DVB-S2 --sr 125000 --fec 1/2
.\linrad_dvb.exe --help
```

De externe speler `vlc.exe` wordt niet meegeleverd: de ingebouwde libVLC-speler
wel. FFmpeg en FFplay zijn meegeleverd. De bestaande externe GPU-knop gebruikt
CUDA/NVIDIA-instellingen en is geen algemene Windows-GPU-autodetectie.

## Pluto / PlutoSky en foutdiagnose

Kies **Pluto / PlutoSky (RX1)** in Input. Laat de Pluto-URI leeg voor automatische
USB-selectie, of vul bijvoorbeeld `ip:192.168.2.1` in voor de netwerkverbinding.
De aangesloten PlutoSky (Z7020/AD9361) is hiermee getest via USB op Linux
en via IP met de Windows-executable onder Wine. Dit gebruikt dezelfde RX1/A_BALANCED-ingang als `atv-receiver-analog`.
De sample rate is 2.083334..61.44 MS/s; begin met 4.096 MS/s voor DAB.
De werkelijk ondersteunde frequenties hangen af van de firmware van je Pluto;
de toepassing wijzigt die firmware niet. USB vereist de Windows-driver van
Analog Devices; netwerkgebruik vereist een bereikbaar Pluto-IP-adres.

Als een proces crasht, vermeldt de foutmelding zowel het lange decimale nummer
als de Windows-code in hexadecimale vorm en, indien bekend, een omschrijving.
Gebruik `start-met-log.cmd` om de console-uitvoer naast het programma in
`windows-diagnose.log` op te slaan. De B210-log noemt het gebruikte imagepad
en of het openen van het apparaat is gelukt. De Kintex-7-images blijven behouden.

Voor DVB-T2-tijdmetingen start je `start-dvbt2-profile.cmd` en laat je de B210
minstens tien seconden ontvangen. Elke vijf seconden verschijnen cumulatieve
`DVB-T2 profile:`-regels in het log en via het statusendpoint. Windows gebruikt
hiervoor CPU-tijd per thread (`GetThreadTimes`), zonder wachttijd. De profiler
staat bij normaal starten uit. Sluit daarna de applicatie en bewaar het unieke
`dvbt2-profile-*.log` naast de executable.

Je kunt ook via **Input → File (IQ)** een korte opname helemaal afspelen
(bij 10 MS/s en s16 is 15 seconden ongeveer 600 MB). Laat voor een zuivere
vergelijking geen tweede decoder tegelijk draaien. Controleer de afspeelduur in
de `[iq-file] end of file`-regel: de bestandsbron kan bij vertraging langer doen
over de opname en bootst dan geen ononderbroken live-SDR-aanvoer na.

Het profiel bevat ook `wall=`: verstreken tijd binnen elke meetstap, inclusief
wachten en tijd waarin de thread niet wordt ingepland. Geneste meetstappen
worden afgetrokken van hun bovenliggende stap. Tijd van parallelle threads kan
overlap hebben; tel `wall` daarom niet op als totale looptijd. `wall - cpu` is
een aanwijzing voor wachten of verdringing, geen afzonderlijk bewijs van de
oorzaak. De extra meetpunten zijn:

- `host.input-wait`: wachten op invoer en de invoerwachtrij;
- `host.convert`, `host.feed`, `host.poll`, `host.report`: werk in de adapter,
  exclusief onderliggende gemeten decoderstappen;
- `host.ts-delivery` en `host.ts-flush`: BBFRAME-uitpakken en schrijven naar
  TS-uitvoer, respectievelijk flushen; blokkering door de uitvoer telt mee;
- `t2.resample-wait` en `t2.pool-wait`: wachten op ruimte in de resamplerpijplijn
  en op afronding door parallelle workers.

Het opstartlog meldt `logical_processors` en `configured_workers` (`-1` is
automatisch). Het endpoint geeft nu ook `decoder.buffer_valid` en, indien
beschikbaar, `decoder.buffer_percent`: dezelfde momentopname als Service Info.
De bestaande decoderalgoritmen, prioriteiten en wachtrijlimieten veranderen
niet door deze meetuitbreiding. Profileren zelf heeft enige overhead.

De Windows-build compileert de DVB-T2-component en zijn workeradapter met
`-O3`, overeenkomstig onze Linux-build en OnAir's Release-configuratie.
AVX2/FMA blijven runtime geselecteerd; er is geen globale AVX2-eis en geen
`fast-math`. Controleer bij live ontvangst zowel de `[SDR]`-verliescounters als
`input_dropped` van de decoder: een lege SDR-uitvoerwachtrij sluit verlies in
de interne decoderwachtrij niet uit.

De DVB-T2-worker laat resampling nu op een eigen thread lopen, tegelijk met
OFDM-demodulatie. De extra wachtrij is begrensd tot 32 blokken (circa 210 ms
bij 10 MS/s); samplegaten blijven op hun plek en bij bestandseinde worden
ook de laatste resamplerblokken verwerkt. De opstartregel meldt
`DVB-T2 frontend: pipelined`. Voor een vergelijking met de oorspronkelijke
verwerking kan in dezelfde opdrachtprompt `set LINRAD_DVBT2_FRONTEND=sync`
worden ingesteld; `set LINRAD_DVBT2_FRONTEND=` herstelt de standaard.

Bij bekende IQ-gaten telt de voorspelde P1-positie niet meer mee als meting
van de sampleklok. Herhaalde gaten konden daardoor een onterechte klokcorrectie
opbouwen, waarna P2/L1 al vóór LDPC beschadigd raakte. Een synthetische test
zonder LDPC reproduceert dit; dezelfde test blijft met de wijziging stabiel.
Dit herstelt een fout ná sampleverlies. De eerste oorzaak van het vollopen van
de Windows-buffer is hiermee nog niet vastgesteld.

De Windows FEC-decoderthreads schakelen EcoQoS (execution-speed throttling)
expliciet uit. Hun bestaande threadprioriteit blijft gelijk. Bij het opstarten
meldt elke betrokken thread `EcoQoS disable accepted`, `EcoQoS disable failed`
(met Windows-foutcode), of `EcoQoS control unavailable`. Deze meldingen staan
ook in het statusendpoint. Acceptatie van de API-aanroep bewijst niet dat het
IQ-verlies is opgelost; vergelijk daarvoor twee verse metingen bij dezelfde
ontvangstinstellingen en spelerinstelling. Start via `start-dvbt2-profile.cmd`.

De klokcorrectie kiest op geschikte x86-processors automatisch een AVX2-pad.
Het opstartlog meldt `DVB-T2 clock correction: avx2`. De filtercoëfficiënten,
sampleposities en klokregeling blijven gelijk. Voor een vergelijking met het
oude rekenpad start je vanuit dezelfde opdrachtprompt:

```bat
set DECT2_CLOCK_GENERIC=1
start-dvbt2-profile.cmd
```

Met `set DECT2_CLOCK_GENERIC=` en opnieuw starten wordt AVX2 weer automatisch
gekozen. Dit versnelt één onderdeel; een live meting moet uitwijzen hoeveel
ruimte dit op de Windows-ontvanger oplevert en of het IQ-verlies afneemt.

De AVX2-LDPC-kernels gebruiken de twee kleinste waarden rechtstreeks, zonder
hun posities bij te houden. Het opstartlog meldt
`DVB-T2 LDPC: avx2 value-minima`. De iteratielimieten en herkansingen blijven
gelijk. Regressietests vergelijken alle uitvoerbits, convergentie en aantallen
iteraties met de onafhankelijke referentiedecoder, ook bij gelijke minima.
Korte componenttests onder Wine geven 6-11% minder LDPC-tijd; het effect op
live ontvangst op de Windows-pc moet afzonderlijk worden gemeten.

Het frontend hergebruikt de databuffers en stelt de geselecteerde PLP direct
samen, zonder eerst de volledige tussenstroom te kopiëren. Het log meldt
`DVB-T2 channel processing: direct PLP assembly, reusable buffers`.
De profielregels `t2.ce.*` en `t2.eq.*` splitsen kanaalverwerking verder op.
Hun tijden worden niet meer in de bovenliggende `t2.chanest+eq` en
`t2.eqloop` meegerekend: tel de onderdelen erbij om met oude logs te vergelijken.

De native meting na de kanaaloptimalisatie, met GPU-videorenderer aan,
leverde over 102,373 seconden nul nieuwe decoder-IQ- en FEC-frameverliezen.
De buffer stond op de meetmomenten op 19%, 9% en 20%. Er waren nog
UHD-overflows en 1.642 onherstelbare BCH-blokken; verliesvrije ontvangst
is dus nog niet aangetoond. Deze live intervallen zijn geen gecontroleerde
vergelijking met exact dezelfde IQ-invoer.

Het pakket bevat ook de librsvg-pixbuf-loader voor de ingebouwde SVG-iconen
van GTK's Adwaita-thema. Een melding `Could not load a pixbuf` met een pad
onder `/org/gtk/libgtk/theme/Adwaita/assets/` kan ontstaan als deze loader of
zijn DLL's ontbreken. Pak bij een update het volledige pakket uit: alleen de
EXE vervangen herstelt ontbrekende loaders en hun cache niet.

Via **Output → Status endpoint…** schakel je het endpoint voor alle modi direct
aan of uit. Kies `0.0.0.0` als luisteradres voor toegang vanaf andere computers.
Sluit het venster en druk **W** om de instelling voor volgende starts op te slaan.

DAB-status gaat via een pipe naar de applicatie en is met
`--status-port 8877 --status-bind 0.0.0.0` op `http://ONTVANGER-IP:8877/status` uit te lezen.
De applicatie schrijft daarvoor geen `state.json`. De JSON bevat ook recente
logmeldingen en fouten. Sta de TCP-poort toe in de Windows-firewall voor het
privénetwerk; dit uitleesendpoint heeft geen authenticatie.
DAB probeert tijdelijk vergrendelde slideshowbestanden opnieuw;
de IQ-ontvangst blijft doorlopen. Paden met Unicode-tekens worden ondersteund.

## SDR-ontvangstdiagnostiek

Voor de B210/UHD-bron publiceert `/status` nu een apart `sdr`-object.
`hardware_overflows`, `recv_timeouts`, `received_samples`, `recv_calls`,
`output_dropped_samples`, `service_gaps_over_5ms` en
`spectrum_snapshots_dropped` zijn cumulatief sinds de start van de bron.
Een hardware-overflow is een gebeurtenis, geen aantal verloren samples.
`received_samples` telt wat UHD retourneert, inclusief eventuele samples
bij een foutmelding; het is geen schatting van de ontbrekende samples.
De bestaande decodervelden `input_dropped` en `fec_frames_dropped` blijven
aparte tellers. `sdr.sample_rate` is de werkelijk ingestelde UHD-samplerate;
het bovenste `rate` blijft de geselecteerde kanaalbandbreedte.

Elke circa één seconde verschijnt een `[SDR] RX timing:`-regel. Het
`sdr`-object bevat de laatste regel, met eigen `telemetry_age_ms` en
`telemetry_fresh` (minder dan drie seconden oud en ontvanger actief).
Zonder UHD-meting is `sdr` null. Vergelijk twee verse snapshots voor
verliesdelta's; timingmaxima gelden alleen voor `interval_ms`, niet voor
de hele sessie. Voor pieken tussen snapshots blijven de logregels beschikbaar.

- `recv_mean_ms` / `recv_max_ms`: verstreken tijd binnen UHD `recv`, inclusief
  wachten op samples en eventuele onderbreking door de Windows-scheduler.
- `service_gap_max_ms`: langste tijd van terugkeer uit `recv` tot de volgende
  aanroep; omvat eigen verwerking, loggen en schedulervertraging.
- `call_interval_max_ms`: langste tijd tussen het begin van twee leesacties.
- `overflow_service_gap_max_ms`: grootste voorafgaande servicepauze bij een
  aanroep die een overflow rapporteert. UHD kan een overflow vertraagd melden;
  deze correlatie bewijst geen oorzaak. Nul kan ook betekenen: geen overflow
  in dit interval; controleer de cumulatieve overflowdelta.
- `monitor_max_ms` / `output_push_max_ms`: langste monitor/tap-aanroep en
  overdracht naar de bestaande IQ-uitvoerwachtrij in het rapportinterval.

Het SDR-proces verplaatst spectrum-FFT's naar een aparte thread. Alleen
acht aaneengesloten FFT-vensters per spectrumupdate worden gekopieerd,
met één wachtende en één verwerkte snapshot. Een trage spectrumthread mag
een spectrumupdate laten vallen; dit verandert de decoder-IQ en de audio-tap
niet. `spectrum_async=true` en het opstartlog
`[SDR] Spectrum processing: asynchronous snapshots` bevestigen dit pad.
Voor een vergelijking met dezelfde meetcode en de oude synchrone
spectrumverwerking kan vóór het starten `LINRAD_IQ_MONITOR_SYNC=1` worden gezet.
Threadprioriteiten, UHD-transportbuffers en decoderinstellingen zijn hierbij
niet veranderd. Het effect op native Windows-overflows moet nog live worden
gemeten; de unit-/Wine-tests bewijzen geen USB- of schedulerverbetering.


## Bouwen en packagen op Linux

Vereist Python 3 met `pefile`, CMake, pkg-config, GNU tar/zstd, MinGW-w64 GCC/G++ met
`-mcrtdll=ucrt`, en een Rust-toolchain met target `x86_64-pc-windows-gnu`.
Rust-compiler en targetbibliotheek moeten uit dezelfde toolchain komen.

De C/C++-code en de SDK gebruiken de Windows UCRT. Bij GCC 15 met een GCC 16
SDK koppelt de builder de TLS-destructorhelper uit de eigen `libsupc++.a`
statisch mee; die static library moet dus ook geïnstalleerd zijn.

```sh
python3 tools/windows-deps.py build/windows-sdk
python3 tools/build-windows-lime.py
python3 tools/build-windows-pluto.py
rustup target add x86_64-pc-windows-gnu
python3 tools/build-windows.py --rebuild -j 4
python3 dev/package_windows.py
# Persoonlijk pakket voor de Kintex-7-ontvanger:
# python3 dev/package_windows.py --b210-images /pad/naar/werkende/uhd-images
```

De dependency-helper downloadt naar een lokale SDK en controleert de SHA-256
uit de MSYS2-index. Hij installeert niets in het hostsysteem. Reken op enkele
gigabytes vrije schijfruimte; gebruik hiervoor geen kleine tmpfs.
`dependency-packages.json` registreert de precieze pakketversies/hashes.
De package-helper volgt recursief de PE-importtabellen en weigert ontbrekende
niet-systeem-DLL's én ontbrekende geïmporteerde functies.
`dll-imports.json` bewaart de gecontroleerde importboom.
De ZIP staat in `dist/linrad_dvb-windows-x64.zip`, naast de SHA-256-checksum.
LimeSuite wordt uit een vastgelegde, op SHA-256 gecontroleerde bronversie
gebouwd. `source-packages.json` beschrijft deze aanvullende dependency;
het bronarchief, de MinGW-aanpassingen in de buildhelper en de LimeSuite/FTDI-
licentieteksten worden meegeleverd.

Projectbroncode en componentlicenties worden meegeleverd. De dependencylicenties
staan onder `share/licenses/`. MSYS2-bronpakketten bij de vastgelegde versies
staan op <https://repo.msys2.org/mingw/sources/>; bewaar ook die bij verdere
verspreiding waarvoor de betreffende licentie bijbehorende broncode vereist.

## Teststatus

De port wordt gecrosscompileerd en onder Wine gecontroleerd. Dit vervangt geen
ontvangst- en drivertest op Windows. De Windows-specifieke I/O-laag gebruikt
Winsock, binaire CRT-pijpen, Windows file mappings en jobs voor decoderkindprocessen.
Linux blijft zijn bestaande implementaties gebruiken.

Gecontroleerd onder Wine: binaire IQ-replay, Linrad UDP, DVB-S2 met twee
LDPC-processen, de volledige IQ-naar-TS-pijplijn, DVB-T2 FEC/L1 en het opruimen
van thread-local werkbuffers. De GTK-test controleert gedecodeerde MPEG-TS
videopixels en starten/stoppen van de ontvangstketen; tekst en grafieklabels
zijn ook visueel gecontroleerd. Linux-build en decoderregressietests blijven
slagen. Live Windows-ontvangst, USB-drivers en audio zijn nog niet gevalideerd.

De DAB-uitbreiding is onder Wine gecontroleerd met de negen DAB-kernprogramma's,
een opgenomen 11C-uitzending met AAC-decodering, de volledige IQ-bestandpijplijn,
stationselectie, FIC/TII-presentatie, logboek en herhaalde kanaalwissels zonder
procesherstart. De B210-, RTL-SDR- en HackRF-pipelineprofielen zijn gecontroleerd
zonder hardware te openen. PNG/JPEG-slideshows en stille s16/float-audiouitvoer
via PortAudio zijn getest. De Lime-backend is met gesimuleerde API-antwoorden
getest. Dit blijft een softwaretest op de cross-buildhost, geen live test van
de aangesloten ontvangers en leveranciersdrivers op Windows.

Tests opnieuw uitvoeren op de cross-buildhost (Wine en `build/leandvbtx` nodig):

```sh
python3 tools/build-windows.py --tests -j 4
python3 dev/package_windows.py --stage-only
cp build/windows/windows_tls.exe build/windows/test_dvbt2_fec.exe build/windows/test_dvbt2_l1.exe build/windows/test_lime_rx.exe build/windows/dab_test_*.exe build/linrad_dvb-windows-x64/
python3 tests/test_windows.py
# Optioneel, met grafische sessie en audio-uitgang:
cp build/windows/dab_gui_smoke.exe build/windows/dab_fake_source.exe build/windows/windows_media.exe build/linrad_dvb-windows-x64/
LINRAD_WINDOWS_GUI_TEST=1 python3 tests/test_windows.py
# DAB_FIXTURE_DIR wijst optioneel naar bestaande DAB-IQ-regressieopnames.
```

Maak daarna opnieuw het definitieve package; de testprogramma's worden niet
meegeleverd. `tests/windows_gui_smoke.c` is de aanvullende GUI/video-test.


### HackRF: eerst LNA, daarna begrensde VGA

De gainpijlen regelen de HackRF rechtstreeks via de afzonderlijke versterkertrappen.
VGA blijft op 12 dB terwijl LNA in stappen van 8 dB van 0 naar 40 dB gaat.
Daarna stijgt VGA in stappen van 2 dB, tot maximaal 24 dB. Omlaag gaat in
omgekeerde volgorde. Het hoofdscherm toont LNA en VGA afzonderlijk.
RF amplifier (AMP) blijft afzonderlijk op Auto / Off / On instelbaar;
Auto schakelt AMP in zodra LNA 40 dB bereikt. AMP telt niet mee in `gain`.

Nieuwe configuraties gebruiken `[hackrf] gain_policy=lna_first` en `gain=44`
(LNA 32 + VGA 12). Een bestaande INI zonder `gain_policy` krijgt bij het laden
deze beginstand; de oude waarde wordt in het geheugen bewaard als
`legacy_soapy_gain`. De actieve INI wordt daarmee niet automatisch herschreven:
W slaat de nieuwe instellingen op. Een bestaande AMP-keuze blijft behouden.
De helper accepteert voor HackRF `--gain 12..64` (LNA + VGA), afgerond naar
bovenstaande hardwarestappen; de RF-versterker is `--hackrf-amp auto|off|on`.
