# Headless reception and ODROID-N2

[Back to README](../README.md)

Commands below run from the repository root. The detailed notes are in Dutch unless stated otherwise.

### Headless repeater: RTL-SDR naar SRT (PI6EHV)

`linrad_dvb` kan zonder displayserver als repeaterontvanger draaien. De ingebouwde
DVB-T-decoder blijft zoeken naar ontvangst. Met `--srt-output` start het programma
automatisch een beheerde FFmpeg-encoder zodra de TS geldige videopayload bevat.
De uitvoer is **SRT caller**: deze machine maakt de uitgaande verbinding; de
server op **44.137.26.85:4001** moet **SRT listener** zijn.

Maak een aparte configuratie naast `linrad-dvb.ini`, zodat de relatieve
helperpaden blijven kloppen:

```bash
cp linrad-dvb.example.ini pi6ehv.ini
# Pas [rtlsdr] frequency, gain en eventueel sample_rate/args aan.
# Onderstaande DVB-T BW/FEC/GI zijn voorbeelden: gebruik de zenderinstellingen.
./build/linrad_dvb --config pi6ehv.ini --headless \
  --source rtlsdr --standard DVB-T --sr 250000 --fec 1/2 --gi auto \
  --no-scatterfix --srt-output 'srt://44.137.26.85:4001'
```

De RTL-SDR gebruikt dezelfde SoapyRTLSDR-invoer als de GUI. Stel de echte
ontvangstfrequentie in Hz in bij `[rtlsdr] frequency`; de voorbeeldconfiguratie
is geen bevestiging van de frequentie of bandbreedte van PI6EHV. De bestaande
bandbreedte- en sampleratecontroles blijven gelden. Voor een controle zonder
hardware of netwerk vervang je `--headless` door `--print-pipeline`.

De SRT-uitvoer wordt omgezet naar **1920 × 1080 progressief**, standaard **25 fps**,
**H.264/yuv420p** (4 Mbit/s doelbitrate) en, indien aanwezig, **AAC stereo,
48 kHz, 128 kbit/s**. De eerste video- en eerste audiostream worden gebruikt.
De omzetting houdt rekening met de pixelverhouding van SD-bronnen, deinterlacet
als het bronbeeld als interlaced gemarkeerd is en voegt zwarte balken toe waar
nodig. Beeld wordt niet uitgerekt. De SRT-container is MPEG-TS.

Hiervoor moet het programma `ffmpeg` beschikbaar zijn, met `libx264`, `aac` en
SRT-ondersteuning. `[paths] ffmpeg` kan een ander pad aanwijzen. Dit gebruikt
software-encoding; controleer de CPU-belasting op de uiteindelijke repeaterserver.
De gewone build blijft dezelfde GTK/VLC-bibliotheken gebruiken, maar headless
ontvangst opent geen GUI, speler, geluidsapparaat of displayverbinding.

```ini
[srt]
url=srt://44.137.26.85:4001
loss_timeout_ms=2000
retry_interval_ms=2000
fps=25
video_bitrate_kbps=4000
```

Je kunt ook `[session] source=rtlsdr`, `standard=DVB-T`, `sr`, `fec`, `gi`,
`scatterfix=false` en `renderer=srt` opslaan. Dan is starten met alleen
`./build/linrad_dvb --config pi6ehv.ini --headless` voldoende. `--srt-output URL`
overschrijft de opgeslagen bestemming en kiest automatisch renderer `srt`.
URL-opties `mode=caller`, `latency`, `streamid`, `passphrase` en `pbkeylen` zijn
toegestaan; listener/rendezvous worden geweigerd. `latency` gebruikt microseconden,
zoals beschreven in de [FFmpeg SRT-documentatie](https://ffmpeg.org/ffmpeg-protocols.html#srt).

Zonder video ontstaat geen verbinding. Bij twee seconden zonder nieuwe geldige
videopayload wordt FFmpeg gestopt en vervalt de SRT-verbinding; die vertraging is
instelbaar met `loss_timeout_ms` en vangt korte ontvangstdips op. Alleen PSI,
null-pakketten, herhaalde continuity counters, TEI of versleutelde payload houden
de verbinding niet actief. Dit detecteert videotransport, niet of ieder beeld
foutloos kan worden gedecodeerd. Bij nieuwe ontvangst start een nieuwe encoder
en verbinding, met enige opstarttijd voor streamherkenning en het eerstvolgende
bruikbare videoframe. Een niet-bereikbare server of gestopte encoder leidt tot
een nieuwe poging na `retry_interval_ms`, zolang video binnenkomt. Buffering is
begrensd tot circa 1 MiB / één seconde; bij achterstand vervalt oudere TS, zodat
netwerk- of encoderproblemen de SDR-keten niet blokkeren.

Ctrl+C en SIGTERM stoppen ook de encoder en de SDR-/decoderprocessen. Voor
automatisch starten kan bijvoorbeeld een systemd-service worden gebruikt
(pas gebruiker, paden en configuratie aan; deze service wordt niet automatisch
geïnstalleerd):

```ini
[Unit]
Description=PI6EHV DVB-T to Full HD SRT
Wants=network-online.target
After=network-online.target

[Service]
Type=simple
User=pi6ehv
WorkingDirectory=/opt/sdr-dvb-pipelines
ExecStart=/opt/sdr-dvb-pipelines/build/linrad_dvb --config /opt/sdr-dvb-pipelines/pi6ehv.ini --headless
Restart=on-failure
RestartSec=5
KillMode=control-group
TimeoutStopSec=5

[Install]
WantedBy=multi-user.target
```

De servicegebruiker moet toegang hebben tot de RTL-SDR via de gebruikelijke
udev-regels. Logging gaat naar stderr (bij systemd naar het journal).
`make test-srt` controleert het schakelen, herstel na uitvoerfouten, begrensde
buffering, stoppen zonder display en Full HD/H.264/AAC via een lokale SRT-listener.
De tests benaderen het adres van PI6EHV niet.

### ODROID-N2: Ubuntu 20.04.6 aarch64 en meerdere RTL-SDR's

Voor deze machine is er een aparte native buildhelper:

```bash
# Vanuit de uitgepakte bronmap / checkout op de Odroid, als gewone gebruiker:
CC=gcc-10 CXX=g++-10 bash tools/build_odroid_n2.sh --install-deps
# Voor de SRT-repeater met lokaal gebouwde FFmpeg:
# CC=gcc-10 CXX=g++-10 bash tools/build_odroid_n2.sh --install-deps --build-ffmpeg
```

Het script gebruikt `sudo apt-get` voor de Ubuntu/Debian-pakketten en bouwt
vervolgens in **build-odroid-n2/**. Standaard compileert één taak tegelijk om
ook op een N2 met weinig RAM te kunnen bouwen; met voldoende vrij geheugen
kan `-j 2` worden toegevoegd. De compileropties zijn `-march=armv8-a` en
`-mtune=cortex-a73.cortex-a53`, geschikt voor beide CPU-clusters. De GCC-opties
staan in de [AArch64-documentatie](https://gcc.gnu.org/onlinedocs/gcc/AArch64-Options.html).
De bestaande `build/` en ontvangerconfiguratie worden niet overschreven.

De DVB-T-Viterbi gebruikt in deze AArch64-build automatisch **NEON**, op zowel
de Cortex-A73- als de Cortex-A53-kernen. De logregel `[viterbi] kernel=neon-float`
bevestigt de gekozen route. De NEON-kern is hier op bitgelijke scores en
beslissingen gecontroleerd onder ARM-emulatie; de volledige native build en
live doorvoer moeten nog op de fysieke N2 worden gevalideerd. De AVX2-winst
van de Ryzen-metingen is geen snelheidsclaim voor de N2.

Na bouwen op de N2 voert dit de decodercontroles uit, inclusief alle vijf FEC's,
NEON/scalarvergelijking en een build met alleen scalarcode:

```sh
make BUILD_DIR=build-odroid-n2 test-dvbt
```

Vergelijk daarna met dezelfde bestaande configuratie en SDR/zenderinstellingen
(stop de eerste run voordat je de tweede start):

```sh
# NEON automatisch:
./build-odroid-n2/linrad_dvb --config pi6ehv-odroid.ini --headless --rtlsdr-serial 1002
# Scalarreferentie, uitsluitend voor deze start:
LINRAD_DVBT_VITERBI_SCALAR=1 ./build-odroid-n2/linrad_dvb --config pi6ehv-odroid.ini --headless --rtlsdr-serial 1002
```

Gebruik de juiste dongle/configuratie voor jouw opstelling. Controleer vooral
3/4, 5/6 en 7/8 bij passende zenderinstellingen: CPU, groeiende Buffer-wachtrij,
IQ-verlies, FEC-drops en TS-uitvoer. Laat ook de gebruikelijke video/SRT-keten
meedraaien; die concurreert op de N2 om CPU-tijd. Herhaal beide routes met een
stabiel signaal. Gelijke TS-bytes worden met identieke IQ-bestanden gecontroleerd;
twee opeenvolgende RF-ontvangsten bevatten niet noodzakelijk dezelfde data.

Ubuntu 20.04 heeft oudere GLib-, json-c- en SoapySDR-bibliotheken. De broncode
houdt daar rekening mee; de volledige toepassing blijft GTK/VLC-devpakketten
nodig hebben, ook bij headless gebruik. Het script installeert ook de
Boost-system-developmentbibliotheek die de UHD-linkstap op Focal nodig heeft.
Ubuntu's `universe`-component moet beschikbaar zijn voor de SDR-pakketten.

De standaard-FFmpeg van Ubuntu 20.04 heeft geen SRT-uitvoer in de gecontroleerde
Focal-packagebuild. `--build-ffmpeg` haalt daarom de officiële FFmpeg-broncode
op, vastgezet op tag **n6.1.6** en een gecontroleerde commit, en bouwt een
beperkte versie met MPEG-TS, H.264/HEVC/MPEG-2/MPEG-4-decodering,
H.264/AAC-encoding, de gebruikte beeldfilters en SRT. Bron en uitvoer blijven
onder `build-odroid-n2/`; de executable wordt
`build-odroid-n2/ffmpeg/bin/ffmpeg`. Deze optie vervangt geen systeem-FFmpeg,
headers of bibliotheken. Zie ook de [officiële FFmpeg-releases](https://ffmpeg.org/releases/).
Een reeds aanwezige SRT-geschikte FFmpeg kan met `--srt --ffmpeg /pad/naar/ffmpeg`
worden gekozen in plaats van `--build-ffmpeg`.

Na de build wordt **pi6ehv-odroid.ini** aangemaakt, alleen als die nog niet
bestaat. Deze configuratie verwijst naar de juiste helpers en FFmpeg en kiest
RTL-SDR, DVB-T en de ingebouwde speler. Met `--srt` of `--build-ffmpeg` kiest een nieuwe configuratie de SRT-caller naar `44.137.26.85:4001`. Stel voor het starten in:

- `[rtlsdr] frequency`: de afstemfrequentie van de SDR in Hz (na de omzetter). De template
  gebruikt bewust `0`, zodat hij niet op een aangenomen frequentie begint.
- `[session] sr`, `fec`, `gi`: de zenderinstellingen. `sr=250000` en `fec=1/2`
  zijn voorbeeldwaarden, geen vastgestelde PI6EHV-instellingen.
- `[rtlsdr] gain` en zo nodig `sample_rate`.

Bij RTL-SDR toont HTTP `/status` onder `tuning` de ingestelde frequentie
(`sdr_set_hz`) en de door de driver teruggemelde frequentie (`sdr_readback_hz`).
De kanaalfrequentie `rf_frequency_hz` is de teruggemelde frequentie plus
`lo_offset_hz` en `receiver_offset_hz`. Bij 144600000 Hz, een externe LO van
10224000000 Hz en nul kanaaloffset is dat 10368600000 Hz. De externe LO komt
uit de configuratie. Tot de driver een waarde meldt, zijn de teruglezing en
de daarvan afgeleide RF-frequentie `null`. Dit werkt ook zonder DVB-lock.

Kies één van je drie dongles op het **USB-serienummer**, bijvoorbeeld **1002**:

```bash
./build-odroid-n2/linrad_dvb --config pi6ehv-odroid.ini \
  --headless --rtlsdr-serial 1002
```

`--rtlsdr-serial 1001` en `--rtlsdr-serial 1003` kiezen de andere apparaten.
Deze optie selecteert automatisch RTL-SDR-invoer en overschrijft alleen voor
deze start een opgeslagen selectie. Eventuele oude `rtl=`/`index=`-waarden
worden verwijderd zodat ze het serienummer niet kunnen overrulen. Andere
RTL-SDR-opties blijven behouden. Dit gebruikt `serial=1002` bij SoapyRTLSDR;
een niet gevonden of bezet geselecteerd apparaat geeft een fout.
De index 0/1/2 van `rtl_test` kan na opnieuw aansluiten anders zijn.

Permanent kan de selectie in de INI worden gezet:

```ini
[rtlsdr]
args=driver=rtlsdr,serial=1002
```

Vervang alleen de `args`-regel in de bestaande sectie; behoud de andere
ontvangerinstellingen. Voeg bij systemd `--rtlsdr-serial 1002` toe aan
`ExecStart`, of gebruik deze opgeslagen selectie. Gebruik daarbij het pad
`build-odroid-n2/linrad_dvb` en `pi6ehv-odroid.ini` uit dit voorbeeld.

De Odroid-template houdt **1920×1080p25** aan met `[srt] x264_preset=ultrafast`,
`video_threads=3` en `filter_threads=1`. `video_threads` geldt afzonderlijk
voor de videodecoder en encoder, niet als totale CPU-limiet. Deze snellere
preset kost minder rekentijd, met minder compressie-efficiëntie dan `veryfast`.
Hardware-encoding wordt niet verondersteld: bruikbaarheid hangt af van de
aanwezige kernel, drivers en FFmpeg. Meet stabiele realtime ontvangst op de
uiteindelijke N2, inclusief DVB-T-decodering; een geslaagde compilatie bewijst
niet dat de totale 1080p25-keten snel genoeg is.

Gecontroleerd met een volledige build in een Ubuntu 20.04-container (x86_64),
een GCC 9 AArch64-compileerproef voor de CPU-opties, de SoapySDR 0.7-tests en
de lokale SRT-test met de afzonderlijk gebouwde FFmpeg. Die SRT-test controleert
1080p25, audio, anamorf SD zonder audio en opnieuw verbinden. Dit is nog geen
ontvangst- of prestatietest op een fysieke Odroid N2.

```bash
# Later opnieuw bouwen; lokale FFmpeg wordt automatisch hergebruikt:
bash tools/build_odroid_n2.sh --srt
# Alleen controleren, zonder installatie, build of configuratiewijzigingen:
bash tools/build_odroid_n2.sh --check --srt
# Opdracht controleren zonder de dongle of SRT-verbinding te openen:
./build-odroid-n2/linrad_dvb --config pi6ehv-odroid.ini \
  --print-pipeline --rtlsdr-serial 1002
```

### Experimenteel: meerdere DVB-T-kanalen uit één SDR naar ATV-mozaic

Voor Linux is er een aparte headless launcher, zonder GUI of FFmpeg:

```bash
make -j2 build/linrad_dvb build/sdr_to_stdout
python3 tools/dvbt_multirx.py --config examples/dvbt-multirx.ini --print-pipeline
python3 tools/dvbt_multirx.py --config examples/dvbt-multirx.ini
```

Python 3.9 of nieuwer is nodig; er zijn geen extra Python-pakketten vereist.
`--print-pipeline` controleert de configuratie en toont de opdrachten zonder
SDR of netwerk te openen. De launcher schrijft niets naar je actieve
`linrad-dvb.ini`. Bewaar een eigen kopie van het voorbeeld in `examples/`, of
pas bij verplaatsen de relatieve paden in `[paths]` aan. Alle frequenties en
bandbreedtes in dit bestand zijn in Hz.

Het voorbeeld opent **één RTL-SDR op 144,550 MHz, 2,4 MS/s** en start twee
onafhankelijke ingebouwde DVB-T-decoders:

| Kanaal | Offset ten opzichte van SDR | BW | FEC | MPEG-TS via UDP |
| --- | --- | --- | --- | --- |
| 144,525 MHz | −25 kHz | 40 kHz | 1/2 | 127.0.0.1:10000 |
| 144,575 MHz | +25 kHz | 40 kHz | 1/2 | 127.0.0.1:10001 |

De decoder verschuift elk kanaal vóór resampling en gebruikt in deze modus
een langere anti-alias FIR. De test met gelijktijdige tonen in beide kanalen
controleert de richting, doorlaatband en minimaal 40 dB onderdrukking van de
geteste tonen uit het andere kanaal, op beide voorbeeldsamplerates. Dat is
geen garantie voor ontvangst bij willekeurige vermogensverschillen tussen
zenders. De bestaande korte filter en de instellingen van de gewone GUI-modus
blijven behouden. De uitvoer bevat de ontvangen MPEG-TS zonder video/audio
opnieuw te encoderen; er wordt geen lokaal TS-bestand geschreven.

Voor een **FUNcube Dongle Pro+** vervang je uitsluitend de betreffende
instellingen onder `[sdr]`:

```ini
[sdr]
source=fcdpp
frequency=144550000
sample_rate=192000
gain=30
args=
```

Een specifieke FUNcube kun je selecteren met bijvoorbeeld
`args=hid=/dev/hidraw3`; de helper ondersteunt ook `lna=0|1,mixer=0|1`.
Een RTL-SDR selecteer je via bijvoorbeeld `args=driver=rtlsdr,serial=1002`.
SDR-frequentie, samplerate en gain zijn gedeeld door alle kanalen. Elk kanaal
heeft een eigen FEC/GI, decoderstatus in de log en UDP-bestemming. Standaard
is GI `auto`; gebruik `gi=1/32` als dat de bekende zenderinstelling is.

Maak in ATV-mozaic twee MPEG-TS/UDP-ingangen die luisteren op **10000** en
**10001**. Bij een FFmpeg/VLC-achtige invoersyntaxis zijn dat bijvoorbeeld
`udp://@:10000` en `udp://@:10001`; gebruik de luistersyntaxis die jouw
ATV-mozaic-versie ondersteunt. Staat ATV-mozaic op een andere computer, zet
dan bij beide `[channel:...]`-secties `udp_host` op het IPv4-adres van die
computer en laat die computer de twee UDP-poorten ontvangen. Dit is ruwe
MPEG-TS over UDP, geen RTP of SRT.

Je kunt 1 tot 8 `[channel:NAAM]`-secties definiëren. De eerste experimentele
versie ondersteunt alleen DVB-T BW 40 kHz en bronnen `rtlsdr` en `fcdpp`.
Alle kanalen moeten binnen de gedeelde IQ-doorlaatband vallen; CPU-capaciteit
en ontvangstcondities bepalen het praktisch haalbare aantal. De voorbeeldkanalen
beslaan samen 90 kHz, inclusief 10 kHz tussenruimte. Ook met de FUNcube op
192 kS/s passen beide binnen de ingestelde digitale bandgrenzen.

Voor de tweekanaalsproef bevat het voorbeeld ook **experimentele CPU-affiniteit**:

```ini
[channel:144525]
cpu_affinity=2
# ... overige kanaalinstellingen behouden

[channel:144575]
cpu_affinity=3
# ... overige kanaalinstellingen behouden
```

Hiermee draait de eerste decoder op logische CPU 2 en de tweede op CPU 3.
CPU-nummers beginnen bij 0; bekijk de nummers op de doelmachine met `lscpu -e`.
Dit kiest geen fysieke kern op basis van snelheid. Controleer bij verschillende
CPU-clusters dus zelf welke CPU's je wilt gebruiken. De binding geldt voor
**alle threads van de betreffende decoder**, inclusief IQ, OFDM en FEC.
De gedeelde SDR-helper en Python-launcher krijgen geen extra binding. Andere
processen mogen nog op de gekozen CPU's draaien; dit reserveert geen cores.

De launcher gebruikt `taskset` uit `util-linux`, vóór het starten van de
decoderthreads. Een ontbrekend programma of een CPU buiten de voor de launcher
toegestane set geeft een fout voordat de SDR wordt geopend. Dit houdt ook
rekening met CPU-beperkingen van een service of container. `--print-pipeline`
toont de `taskset`-opdrachten maar controleert de CPU-beschikbaarheid pas bij
echt starten, zodat je een bestand voor een andere machine kunt voorbereiden.
Verwijder `cpu_affinity`, laat de waarde leeg of gebruik `cpu_affinity=auto`
om Linux weer over de beschikbare CPU's te laten verdelen. De binding is een
proef en bewijst geen snelheidswinst; de native ODROID-vergelijking staat nog open.

Stoppen met Ctrl+C of SIGTERM sluit alle subprocessen en de SDR af. De Python
IQ-verdeling heeft per decoder een begrensde buffer (`queue_ms`, standaard
500 ms). Bij een volle verdelingsbuffer of een gestopt subprocess stopt de
hele opstelling met foutstatus; de launcher laat daar geen stille IQ-gaten
ontstaan. Bestaande hardware-overflows, decoder-ringverliezen en UDP-sendfouten
worden door de native processen gelogd en blijven afzonderlijk relevant.
Er is nog geen gezamenlijk HTTP-statusendpoint of automatische herstart per
kanaal. Voor een repeater kun je de launcher onder systemd draaien met
`Restart=on-failure`, `RestartSec=3`, een absolute `--config` en het juiste
`WorkingDirectory`. Gebruik een gebruiker met SDR-/audio-/HID-rechten.

Validatie: `make test-dvbt-multirx` controleert de echte native mixer en
kanaalfilter, configuratiefouten, byte-identieke IQ-verdeling, overbelasting
en procesopruiming. De tests controleren ook afzonderlijke CPU-maskers voor
twee decoderprocessen en hun nieuwe threads, met behoud van de CPU-maskers
van launcher en SDR-helper. Twee echte DVB-T-kanalen tegelijk, langdurig gebruik en
de koppeling met ATV-mozaic moeten nog live worden gevalideerd.
