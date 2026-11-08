# Native Windows x64

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
zijn. Linux `.so`-drivers werken niet op Windows. Hardwareontvangst moet nog op
een echte Windows-pc worden getest. Windows ondersteunt geen Linux
`SO_RXQ_OVFL`-teller; AFEDRI telt wel de ontvangen sequentiegaten en queuedrops.

FUNcube koppelt HID en audio via hun Windows-apparaatcontainer en gebruikt
uitsluitend onbewerkte stereo-IQ op 192 kHz. Sluit bij automatische selectie
één Pro+ aan; bij meerdere apparaten kan `hid=WINDOWS_HID_PATH` worden gekozen.
`lna=0|1` en `mixer=0|1` werken zoals op Linux. Linux-apparaatnamen zoals
`/dev/hidrawN` en ALSA `hw:...` zijn niet overdraagbaar naar Windows.
LimeSuite gebruikt de native Mini-backend, inclusief RX-kalibratie; er wordt
geen firmware bijgewerkt. B210 mixer/LO-instellingen zijn eveneens beschikbaar.

Een Kintex-7-clone gebruikt andere FPGA-images dan een originele Ettus B210.
Gebruik de bestanden die al bij die ontvanger werken. Een persoonlijk pakket
kan die met `--b210-images /pad/naar/uhd-images` opnemen; de Windows-INI wijst
dan naar `./uhd-images/kintex7`, met SHA-256-controles naast de bestanden, en
vult bij Device arguments `type=b200` in. Bewaar eigen wijzigingen met **W**.
De generieke build vervangt die bestanden niet door standaard Ettus-images.

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

Live DVB-T2 op Windows is nog niet verliesvrij gevalideerd. De native
profielmeting liet interne IQ-verliezen zien terwijl de SDR-uitvoerwachtrij
verliesvrij bleef. De parallelle verwerking is bedoeld om meer ruimte voor
demodulatie te maken, maar het effect op live Windows-ontvangst moet nog
worden gecontroleerd. Vertraagde IQ-replay en FEC/L1-tests controleren de
decodeeruitkomst, niet de realtime doorvoer.

Het pakket bevat ook de librsvg-pixbuf-loader voor de ingebouwde SVG-iconen
van GTK's Adwaita-thema. Een melding `Could not load a pixbuf` met een pad
onder `/org/gtk/libgtk/theme/Adwaita/assets/` kan ontstaan als deze loader of
zijn DLL's ontbreken. Pak bij een update het volledige pakket uit: alleen de
EXE vervangen herstelt ontbrekende loaders en hun cache niet.

Via **Log → Status endpoint…** schakel je het endpoint voor alle modi direct
aan of uit. Kies `0.0.0.0` als luisteradres voor toegang vanaf andere computers.
Sluit het venster en druk **W** om de instelling voor volgende starts op te slaan.

DAB-status gaat via een pipe naar de applicatie en is met
`--status-port 8877 --status-bind 0.0.0.0` op `http://ONTVANGER-IP:8877/status` uit te lezen.
De applicatie schrijft daarvoor geen `state.json`. De JSON bevat ook recente
logmeldingen en fouten. Sta de TCP-poort toe in de Windows-firewall voor het
privénetwerk; dit uitleesendpoint heeft geen authenticatie.
DAB probeert tijdelijk vergrendelde slideshowbestanden opnieuw;
de IQ-ontvangst blijft doorlopen. Paden met Unicode-tekens worden ondersteund.

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
