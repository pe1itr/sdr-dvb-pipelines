# Linrad DVB — bediening

[Back to README](../README.md)

Commands below run from the repository root. The detailed notes are in Dutch unless stated otherwise.

### Bouwen en starten

De toepassing ondersteunt Linrad, directe SDR-hardware en IQ-bestanden. Je stelt de parameters
rechtstreeks in de GTK-GUI in; er zijn geen profielen nodig. Zowel de GUI als de Linrad UDP-ontvanger zijn in C geschreven.

Alle opties en voorbeelden staan in de ingebouwde hulp. Typ vanuit de projectmap

```bash
bash tools/build.sh --help
```

Bouwen en starten (Linux, C- en C++20-compiler, Rust/Cargo 1.70+, `pkg-config` en development packages voor
GTK 3, GLib, json-c, FFTW3 single precision, SoapySDR, UHD, FFmpeg
(`libavcodec`, `libavformat`, `libavutil`, `libswscale`), libzvbi (`zvbi-0.2`), ALSA,
libsamplerate en libVLC 3
(inclusief de VLC-codecplugins)):

```bash
bash tools/build.sh
# Eenmalig, als linrad-dvb.ini nog niet bestaat:
cp -n linrad-dvb.example.ini linrad-dvb.ini
./build/linrad_dvb
```

De [buildhelper](../tools/build.sh) gebruikt Bash en controleert eerst de
buildtools, de C/C++-compiler en alle hierboven genoemde bibliotheken met
`pkg-config` en kleine compileer-/linkproeven. Alleen runtimebibliotheken
installeren is onvoldoende: ook de headers en development packages zijn nodig.
Een ontbrekende essentiële afhankelijkheid geeft **ERROR**, exitcode 1 en
blokkeert de build. Alle direct gekoppelde bibliotheken zijn essentieel voor
de huidige volledige build, ook SoapySDR en UHD als je alleen Linrad gebruikt.

Ontbrekende optionele Soapy-drivermodules voor HackRF, SDR-IQ, RTL-SDR, Airspy en SDRplay en
externe programma's VLC/CVLC, FFplay en Dire Wolf geven **WARNING**; de build
gaat door. De drivercontrole gebruikt `SoapySDRUtil --info`, zonder radio's
te openen. Zonder deze utility volgt een waarschuwing dat de drivers niet
gecontroleerd konden worden. De controle garandeert geen aangesloten hardware,
UHD-firmware, toegangsrechten of beschikbare VLC-codecplugins.

```bash
bash tools/build.sh --check     # alleen afhankelijkheden controleren
bash tools/build.sh -j 4        # controleren en met vier jobs bouwen
```

Op **Linux Mint, Debian en Ubuntu** kun je vanuit de uitgepakte bronmap de
benodigde bouwpakketten installeren en direct bouwen:

```bash
bash tools/build.sh --install-deps -j 4
```

Start dit als gewone gebruiker. Het script gebruikt `sudo` voor APT.
Wil je eerst zien wat er ontbreekt, gebruik dan `--check`. Bij ontbrekende
bibliotheken toont het script één kopieerbaar installatiecommando voor jouw
distributie. Dit werkt ook voor Fedora, openSUSE en Arch. Voer na installatie
het oorspronkelijke bouwcommando opnieuw uit. Een fout bij de C++20-proef
betekent dat je ook de getoonde compilerfout moet bekijken. Als Rust via
rustup is geïnstalleerd en nog geen werkende toolchain heeft, voer dan als
gewone gebruiker `rustup default stable` uit.

Op een 64-bits ODROID-N2/N2+ met Debian, Ubuntu of een daarop gebaseerde
Armbian gebruik je `bash tools/build_odroid_n2.sh --install-deps`. Daar bouwt
het script standaard met één taak. De opties voor SRT en een eigen FFmpeg
staan in de [ODROID-handleiding](headless.md).

Bij een SDR-waarschuwing hoef je alleen iets te doen als je die ontvanger wilt
gebruiken. Voor een RTL-SDR op Mint of Debian is dat bijvoorbeeld
`sudo apt install soapysdr-module-rtlsdr`. Controleer daarna of `rtlsdr` onder
**Available factories** staat bij `SoapySDRUtil --info`. Soapy laadt de plugin
wanneer de ontvanger opnieuw start. Voor native LimeSDR Mini installeer je de
LimeSuite-ontwikkelbestanden en bouw je opnieuw in een verse bouwmap.

De helper werkt ook vanuit een andere werkmap. Hij gebruikt `make all` en
respecteert `CC`, `CXX`, `CPPFLAGS`, `CFLAGS`, `CXXFLAGS`, `LDFLAGS`, `LDLIBS`
en `PKG_CONFIG_PATH` uit de omgeving. Waarschuwingen veranderen de exitcode
niet; een mislukte build geeft de exitcode van `make` terug. Rechtstreeks
`make` gebruiken blijft mogelijk.

Voor libzvbi zijn ook de ontwikkelbestanden nodig: `sudo dnf install zvbi-devel`
op Fedora, of `sudo apt install libzvbi-dev` op Debian/Ubuntu.
Voor USB-audio zijn `alsa-lib-devel libsamplerate-devel` (Fedora) of
`libasound2-dev libsamplerate0-dev` (Debian/Ubuntu) nodig.

De aparte [DVB-T2-component](../components/dvbt2/README.md) vereist bovendien een
C++20-compiler en Rust/Cargo 1.70 of nieuwer (`rust cargo` op Fedora,
`rustc cargo` op Debian/Ubuntu; controleer de versies op oudere distributies).
Alle Rust-bronafhankelijkheden zijn meegeleverd; de build werkt offline.
De bestaande DVB-T-mode blijft voor smalband/DX. DVB-T2 is voorlopig uitsluitend
voor 8 MHz omroep, met automatische FEC/modulatie/GI en PLP-keuze in **Mode**.

Voor de vrije NPO-multiplex in Eindhoven staat een aparte voorbeeldconfiguratie
klaar: **570 MHz, kanaal 33, 8 MHz BW, 10 Msps**. Deze overschrijft de gewone
configuratie niet. Stel bij een B210-clone eerst de juiste firmwaremap in bij
Input; kies ook de aansluiting waarop de antenne zit.

```sh
./build/linrad_dvb -c linrad-dvbt2-eindhoven.example.ini
# Of dezelfde instelling met HackRF:
./build/linrad_dvb -c linrad-dvbt2-eindhoven.example.ini --source hackrf
make test-dvbt2
```

De T2-status toont de gekozen PLP en verlies door overbelasting. Niet alle
T2-varianten zijn ondersteund: onder meer MISO, inter-frame interleaving en
type-2/sub-sliced PLP's ontbreken. De decoder meldt zulke parameters in het log.
Zie de componentdocumentatie voor tests, herkomst en GPL-3.0-licentie.

De GUI gebruikt één venster zonder tabs, standaard 83% van de beschikbare
schermbreedte en 80% van de beschikbare hoogte.
Bovenaan staan de menu's
**Input**, **Mode**, **Output**, **Presets**, **TCI**, **Log** en **About**. About toont uitleg over de
bediening, de builddatum, sneltoetsen, helpcommando’s en acknowledgements met
projectlinks voor LeanDVB, LDPC, de acquisitiepatch van Ondřej OK1CDJ en
OnAir (de basis van de DVB-T2-decoder). Het
venster is scrollbaar zodat ook op kleinere schermen alle informatie bereikbaar is.
Onder het constellatiescherm staan tien presetknoppen in twee kolommen en vijf
rijen. Open **Presets** om per knop een label in te stellen. **Save current
settings** bewaart de actuele ontvangerinstellingen alleen in die preset;
de gewone INI blijft ongewijzigd. Sluiten van het venster bewaart de labels.
De presets staan naast de INI, bijvoorbeeld in `linrad-dvb.ini.presets.ini`.
Bewaar dit bestand bij het verplaatsen of bijwerken van de installatie.
Logboek-/stationinstellingen en de HTTP-statusinstellingen gelden voor alle presets.

Een preset kiezen stopt eerst de lopende ontvangst en laadt daarna de opgeslagen
instellingen. Start de ontvangst zelf weer met **Start SDR**. De gekozen preset
is groen. Instellingen handmatig wijzigen heft die selectie op; opnieuw starten
of handmatig terugzetten maakt geen preset actief. Een lege presetknop is
uitgeschakeld totdat je er instellingen in opslaat. **W** blijft de gewone INI
opslaan en overschrijft geen presets.

De waterval en het spectrum staan bovenaan over de volle breedte. Daaronder staan
links de decoderinformatie en daarnaast een vast playerkader met dezelfde breedte
en hoogte als Service info. Rechts daarvan verschijnt het TSscatterFix-paneel
wanneer TSscatterFix is ingeschakeld.
Onder Service info staat Status; onder het playerkader staat de S-meter.
Het hoofdvenster is iets hoger zodat het playerkader zijn eerdere grootte behoudt.
Het log is bereikbaar via Log in de bovenbalk. De ontvangen
resolutie verandert de afmetingen van het kader niet. Zowel 4:3 als 16:9 wordt
met behoud van beeldverhouding weergegeven, zonder afsnijden of uitrekken;
overgebleven ruimte is zwart.
De bovenbalk toont achtereenvolgens mode, Symb/s (satelliet) of BW in Hz (DVB-T),
en FEC. Frequentie en gain staan op dezelfde regel als Start/Stop.
De bediening gaat met de pijltjestoetsen; er zijn geen plus/min-knoppen.
Onder het spectrum staan links een vierkante constellatie en rechts service-info.
Mode en renderer wijzigen doe je via hun menu. Naast **BW** staat de knop
**Scatterfix**, rood wanneer ingeschakeld. Je kunt Scatterfix ook tijdens
ontvangst aan- en uitzetten, zonder de decoder, player of UDP-uitvoer te herstarten.
Het werkt bij alle uitvoerkeuzes, ook MPEG-TS-bestandsopslag.
Rechtsboven staat één knop met een statuslampje: **Start SDR** bij uitgeschakelde
ontvangst, **Stop SDR · actief** met een groene knop en groen lampje tijdens
ontvangst. Tijdens stoppen is de knop geel en tijdelijk geblokkeerd; na een fout
wordt hij rood en kun je opnieuw starten. Onderaan staan de status en het
uitklapbare **Opstartcommando / Log**.

Links van **Start SDR** staat **Record IQ**. Start eerst de live ontvangst en
schakel deze knop in om de volledige ontvangen IQ-band op te nemen; de rode
**Stop IQ**-knop stopt de opname. Stoppen of sluiten van de ontvanger sluit ook
de opname af. Bij **Bestand (IQ)** is de opnameknop uitgeschakeld.
De bestanden staan naast het INI-bestand en heten
`linrad-dvb-[UTC timestamp]-[frequentie in Hz]-[samplerate in Hz].iq`, bijvoorbeeld
`linrad-dvb-20261101T001530.123456Z-144600000-250000.iq`.
Het formaat is interleaved I,Q, signed 16-bit little-endian, zonder header.
Er wordt opgenomen vóór het decoderfilter en vóór de conversie naar u8; native
8-bit SDR-samples worden naar s16 geschaald. Een bijbehorend `.iq.json`-bestand
bevat formaat, samplerate en de weergegeven ontvangstfrequentie bij het starten
(inclusief LO-offset bij directe SDR-invoer). Daardoor herkent de bestandsweergave
deze instellingen automatisch. Een nieuwe opname overschrijft geen bestaande
bestanden. Bij een schrijffout of een volle opnamebuffer stopt de opname met een
melding; de ontvangst blijft doorlopen.

- **Input**: input, samplerate, IQ-formaat, LO-offset, afstemstap,
  device-argumenten, antenne en UHD-images;
  bij Linrad het UDP-adres, de poort, de headerlengte en de frequentie voor de schaal.
  Bij **Afedri Net** stel je het SDR-IP, de TCP/UDP-poort en het UDP-bindadres in;
  zie [Native Afedri Net input](receivers.md#native-afedri-net-input).
  Kies **Bestand (IQ)** om een opname met bijbehorende JSON-instellingen af te spelen.
- **Mode**: DVB-mode, symbolrate of BW, FEC, guard interval en constellatie;
  voor DVB-S2 ook LDPC-methode, 1–8 workers en automatische acquisitie/verwerking.
- **Output → Renderer**: ingebouwde player, UDP naar een externe player, VLC (`cvlc`),
  VLC met bediening, FFplay, een eigen programma of een MPEG-TS-bestand.
- **Output → Status endpoint…** opent de instellingen voor het HTTP-statusendpoint.
- **Output → Program Map Table (PMT)** toont de streams per programma.
- **Alt+I / Alt+M / Alt+R / Alt+T / Alt+A**: open Input, Mode, Renderer, TCI en About.
  Kleine letters werken ook; houd Alt ingedrukt om een menu te openen.
- **Q**: sluit Linrad DVB af vanuit het hoofdvenster.
- **R**: reset de actieve TSscatterFix-reconstructie. Herstelbuffer, beeld en
  status worden gewist; reconstructie begint opnieuw met nieuwe TS-data.
  De ontvangst en gewone videospeler blijven doorlopen.
- **M**: demp/herstel de luidsprekers voor DVB en USB/FM/FM-DATA; TCI-audio loopt door.
- **U / F / D**: schakel respectievelijk USB (3 kHz), FM (12,5 kHz) of FM-DATA (20 kHz) in/uit.
- **Page Up / Page Down**: stem de actieve monitor lager/hoger, in stappen van
  100 Hz voor USB of 2,5 kHz voor FM/FM-DATA. Klikken in het spectrum stemt die monitor direct af.
- **←/→**: frequentie lager/hoger; **↑/↓**: gain hoger/lager.
  **[ / ]**: bandfilter smaller/breder; **W**: instellingen opslaan.
  **P**: het volledige hoofdvenster als PNG opslaan naast het INI-bestand,
  inclusief spectrum, waterval en ingebouwde video. Voorbeeld:
  `linrad-dvb-20261029T181530.123456Z-DVB-T-BW40000-144600kHz.png`.
  DVB-S/S2 gebruikt bijvoorbeeld `SR333000` (symbolen/s); DVB-T gebruikt BW in Hz.
  De tijd is UTC; de ontvangstfrequentie inclusief LO-offset wordt afgerond op hele kHz.
  Bij Linrad wordt de in Input ingevulde schaalfrequentie gebruikt.
  Deze toetsen gelden in het hoofdvenster, ook tijdens ontvangst.
  In de instellingendialogen werken de toetsen als normale invoer.

De GUI bevat onder andere:

- Direct onder Received video een compacte signaalmeter met de indeling van Linrad: een gele
  vermogenslijn op zwart, horizontale rasterlijnen en zes pijltjes. Links stel je
  de verticale resolutie in, rechts verplaats je de lijn omhoog/omlaag en met de
  twee horizontale pijlen rechtsonder verander je de snelheid en middeling
  (0,1–12,8 s per punt). De lijn loopt van links naar rechts; als het venster vol
  is schuift de historie een stukje terug. Onderaan staat de tijdschaal.
  Met **BW aan** wordt het vermogen binnen de ingestelde bandbreedte rond de
  receiver offset opgeteld; met **BW uit** over de volledige IQ-invoerband.
  De waarde is gemiddeld vermogen in **dBFS**, berekend uit de spectrumtelemetrie
  vóór het filter (inclusief ruis; bandranden op FFT-resolutie), geen gekalibreerde
  dBm-waarde. Langzamer tekenen middelt het lineaire vermogen over meer updates.
  Bij een andere meetband, afstemming, samplerate of snelheid begint een nieuwe
  grafiek. Bij Stop blijft de historie staan; ontbrekende IQ-data wordt gemeld.
  **W** bewaart niveau, resolutie en snelheid in de sectie `[display]`.
- Onder Service info staat een apart Status-vak voor meldingen zoals
  `Pipeline running` en `Settings saved`. Lange meldingen worden over twee
  regels verdeeld; de volledige tekst is ook zichtbaar als tooltip.

- DVB-S en DVB-S2: SR 35000, 66000, 125000, 250000 of 333000; FEC 1/2 of 2/3.
- DVB-T: BW 35000, 40000, 150000, 250000, 333000 of 500000 Hz; FEC 1/2, 2/3, 3/4, 5/6 of 7/8;
  guard interval 1/32, 1/16, 1/8 of auto.
- Scatterfix aan/uit, één Start/Stop-knop, een pipelineweergave en een logvenster.

DVB-S/S2 gebruiken de meegecompileerde LeanDVB-backend. Ook de DVB-T-broncode
wordt rechtstreeks gelinkt in `linrad_dvb`. Voor DVB-S2 bouwt `make` de
LDPC-helper mee; een andere checkout of losse installatie is niet nodig.
De GUI toont het beeld standaard in het rechterpaneel. DVB-S2 ondersteunt QPSK en 8PSK; bij QPSK: FEC 1/2 selecteert MODCOD-masker
`0x10`, FEC 2/3 selecteert `0x40`. De standaard samplerate is **1010526**
samples/s. Frequentie en de SDR zelf stel je in Linrad in.

Zonder Scatterfix:

```text
Linrad UDP -> C-ontvanger -> leandvb / ingebouwde DVB-T-decoder -> player (+ optioneel UDP)
```

Met Scatterfix:

```text
Linrad UDP -> C-ontvanger -> decoder -> oorspronkelijke TS -> player (+ optioneel UDP)
                                  \-> kopie -> embedded TSscatterFix -> RGB-afbeelding + status
```

TSscatterFix wordt meegecompileerd uit de hier onderhouden C-broncode in
[components/tsscatterfix](../components/tsscatterfix/README.md). Er is geen externe
TSscatterfix-checkout, Python-helper of extra UDP-poort nodig. De oorspronkelijke
MPEG-TS blijft ongewijzigd naar de gekozen player, UDP-uitvoer of het TS-bestand
gaan. De parallelle herstelthread levert direct RGB24-afbeeldingen aan GTK.

Het compacte paneel naast **Received video** toont de afbeelding met behoud van
beeldverhouding. Daaronder staan geschatte voortgang, herstelfase, provider,
servicenaam en aantallen headers, keyframes en afbeeldingen. Het percentage is
een schatting op basis van ontvangen informatie, geen resterende tijd.
H.264 en H.265 worden automatisch herkend. Het paneel is alleen zichtbaar als
**TSscatterFix** aan staat; uitschakelen, Stop en herstart wissen het beeld en de gegevens.
Bij opnieuw inschakelen begint reconstructie met nieuwe TS-data. Uitgeschakeld
blijft alleen de TS-doorgifte actief; er wordt geen hersteldata verzameld of gedecodeerd.

Herstel draait op een eigen thread met maximaal 4 MiB wachtende TS-data. Bij
achterstand wordt uitsluitend die herstelbuffer overgeslagen en de parser
opnieuw gesynchroniseerd; de player behoudt alle bytes. Een fout in reconstructie
verschijnt in het paneel en stopt de speler niet. Analyse gebeurt maximaal twee
keer per seconde, ook als een onvolledige PES blijft staan bij wegvallend signaal.
Stop onderbreekt de aanvoer direct; de interface blijft reageren terwijl een
lopende herstelanalyse wordt afgerond. De herstelbibliotheek bewaart standaard
maximaal 8 MiB aan PES/kandidaten naast de TS-wachtrij en beeldbuffers.
Oude `[paths] scatterfix`- en `[scatterfix]`-instellingen worden genegeerd.

Tijdens ontvangst zijn de parameterkeuzes geblokkeerd. Stop ruimt alle gestarte
procesgroepen op, inclusief subprocessen van de decoder, en stopt de herstelthreads. Na een
korte afsluitperiode (1,5 seconde) worden achtergebleven processen geforceerd
gestopt. Ook sluiten van het venster en uitval van een pipelineonderdeel stoppen
de volledige pipeline. Daarna kun je opnieuw parameters kiezen en starten.

### Tijdens ontvangst overschakelen naar de NVIDIA-speler

Bij **Output → Renderer → ingebouwde player** staat onder **Received video** de knop
**Extern op GPU**. Klik tijdens ontvangst om het beeld naar systeem-FFplay met
CUDA-decodering en NVIDIA Vulkan-weergave te verplaatsen. De ingebouwde
VLC-videospeler stopt daarbij, zodat hij dezelfde stream niet ook op de CPU
blijft decoderen. De SDR, DVB-decoder, programmaselectie en eventuele UDP-uitvoer
blijven draaien. TSscatterFix blijft onafhankelijk aan of uit staan.

Met **Terug in venster**, of door het FFplay-venster te sluiten, komt het beeld
terug in het ingebouwde paneel. Een spelerfout schakelt eveneens terug zolang
er nog ontvangst is. Schakelen vraagt enige tijd voor initialisatie, streamheaders
en een volgend keyframe. Een begrensde buffer van 4 MiB vangt opstarten en
kortstondige pauzes in het lezen op. Een vastgelopen externe speler blokkeert de
ontvangst niet: bij een volle buffer worden alleen achterlopende videodata voor
die speler overgeslagen.
**Stop SDR** en sluiten van de GUI sluiten ook de externe speler af.

Schakelen gebeurt uitsluitend met de knop, niet automatisch bij HD. Iedere nieuwe
ontvangstsessie begint weer ingebouwd. De gekozen service en dempstand gaan mee;
een servicewissel of **M** in het hoofdvenster heropent uitsluitend FFplay wanneer
de externe speler actief is. FFplay heeft in zijn eigen venster ook **M** voor
lokale demping en **F** voor volledig scherm.

De ingebouwde speler en GPU-speler starten beide op 100% spelervolume. De
ingebouwde speler stelt dit ook in zodra audio actief wordt, zodat een eerder
opgeslagen laag toepassingsvolume niet het startniveau bepaalt. Daarna kun je
het volume gewoon met de systeemmixer aanpassen; **M** blijft dempen.

De knop gebruikt standaard `/usr/bin/ffplay` en selecteert een Vulkan-device
waarvan de naam `NVIDIA` bevat. Dit staat los van de bestaande FFplay-renderer
onder **Output → Renderer**. Zo wordt de oudere eigen FFplay-build in de home-directory
niet per ongeluk gebruikt. Eventuele aanpassingen in het INI-bestand:

```ini
[ffplay]
gpu_program=/usr/bin/ffplay
gpu_device=NVIDIA
```

Deze FFplay-build moet CUDA én Vulkan/libplacebo ondersteunen. De ingebouwde
weergave blijft softwaredecodering gebruiken; de knop activeert de hieronder
geteste externe route.

De integratie is op 1 november 2026 gecontroleerd met een opgenomen H.264-stream
van 1920×1080, stille MP2-audio en een TS-muxrate van 8 Mbit/s, gevoed via een
lopende pipe. De proef schakelde ingebouwd → FFplay → ingebouwd met behoud van
dezelfde bron en relay. Beide ingebouwde weergaven zijn visueel bevestigd. Het
laatste FFplay-log bevestigt NVIDIA CUDA/Vulkan-weergave en bevat geen
TS-continuïteits- of corrupte-pakketmeldingen. Er draaide hierbij geen SDR-decoder.
De korte proef bewijst geen langdurige stabiliteit; lokale log en testhulp staan
onder `build/diagnostics/gpu-video/live-switch-final.log` en `live-switch.c`.
Geautomatiseerde tests controleren ook behoud van bytes tijdens spelerinitialisatie,
service/dempstand, terugkeer bij sluiten/fouten en doorlopende aanvoer bij een
vastgelopen speler.

### Programma kiezen

Met **TSscatterFix uit** staat op dezelfde plek het paneel **Programs**. De
ontvangen programma's staan onder elkaar, met hun uitgezonden namen. Klik op een
programma om beeld en geluid in de **ingebouwde player** om te schakelen.
De gekozen rij blijft gemarkeerd; EIT en Teletext volgen deze keuze. Zolang de
naam nog niet ontvangen is, staat er `Program` met het service-ID. De tooltip
vermeldt het service-ID en, indien ontvangen, de provider. Een lange lijst is
verticaal scrollbaar.

Dit werkt ook bij hoge symbolrates, zonder TSscatterFix-verwerking. De ontvangst
blijft doorlopen tijdens het omschakelen; de player wacht kort op de tabellen en
een bruikbaar videoframe van het gekozen programma. De eerste service wordt
automatisch gekozen als er nog geen keuze is of de gekozen service verdwijnt.
UDP-uitvoer en TS-opnames behouden alle programma's. Bij een externe renderer
schakel je in die player; de lijst in de app is dan niet aanklikbaar.
Met **TSscatterFix aan** komt het reconstructiepaneel op deze plek terug.

### EIT en klassieke teletekst

Naast **TSscatterFix** staan twee kleine knoppen: **EIT** en **Teletext**.
**EIT** wordt groen zodra programmainformatie is ontvangen. **Teletext** wordt groen
zodra de gekozen service teletekst aankondigt en blijft groen tijdens het bladeren.
De tooltip vermeldt de laatste ontvangsttijd. Beide knoppen blijven aanklikbaar.
Elke knop opent een eigen niet-modaal venster zonder tabbladen. Beide vensters kunnen
tegelijk openstaan; sluiten van een venster onderbreekt de ontvangst niet.

**EIT** toont het huidige en volgende programma uit de actual-transportstream
present/following-tabel, met titel, korte en uitgebreide beschrijving en tijden in
de lokale tijdzone. Het venster **Teletext** toont de uitgezonden pagina op een
zwart 4:3-scherm: 40 kolommen en 25 rijen, inclusief de originele kop (rij 0)
en onderste regel (rij 24). Het beeld bevat geen eigen zenderlogo, lokale klok,
statusbalk of gegenereerde navigatietekst. Servicekeuze, ontvangststatus en de
knop **Reveal** staan buiten het scherm. De pagina schaalt als geheel mee met
het venster, met behoud van de beeldverhouding en scherpe bitmaptekens.
Met **Pagina** kies je 100–899: typ drie cijfers en druk op Enter, of gebruik
**Vorige** en **Volgende** onder het beeld. Bij uitgezonden navigatielinks
open je de bestemming met de bijbehorende kleurknop of door op de gekleurde
verwijzing in de onderste teletekstregel te klikken. **Index (100)** gaat terug naar de beginpagina.
De navigatie loopt van 899 terug naar 100 en omgekeerd. De ontvanger bewaart ontvangen pagina’s in het werkgeheugen,
ook als het venster gesloten is. Een bewaarde pagina verschijnt direct. Bij een
nieuwe uitzending wordt de pagina bijgewerkt. Alleen bij een ontbrekende pagina
verschijnt een wachtmelding. Bij afsluiten vervalt de cache. Ontvangen subpagina's
volgen automatisch de uitzending. De paginakeuze blijft bij een servicewissel
behouden en wordt bij stoppen/herstarten 100. Alleen de informatieviewer wijzigt:
SDR, videospeler (ook extern op GPU) en TS-uitvoer blijven lopen.

De verwerking volgt [ETSI EN 300 706 V1.2.1](https://www.etsi.org/deliver/etsi_en/300700_300799/300706/01.02.01_60/en_300706v010201p.pdf)
voor teletekst en [ETSI EN 300 472 V1.4.1](https://www.etsi.org/deliver/etsi_en/300400_300499/300472/01.04.01_60/en_300472v010401p.pdf)
voor het DVB-transport. De bitmaptekens komen uit libzvbi: kleur- en
achtergrondcodes, nationale tekensets, aaneengesloten/gescheiden mozaïeken,
hold/release-mosaics en dubbele hoogte/breedte worden door de teletekstdecoder
verwerkt. Verborgen tekst blijft verborgen tot **Reveal** wordt ingeschakeld.
De normale knipperfunctie werkt ook zonder nieuwe TS-pakketten.
Het exacte letterontwerp is volgens EN 300 706, bijlage G.2, een decoderkeuze;
verschillende televisies kunnen daarom verschillende glyphs tonen.

Dit is een zelfstandige paginaweergave, geen op video gesynchroniseerde
ondertiteloverlay. De basispagina (Level 1/1.5) is het geteste weergaveprofiel.
libzvbi verwerkt daarnaast beschikbare enhancements tot Level 3.5; volledige
Level 2.5/3.5-conformiteit, zijpanelen en de aanvullende kleur-/faseknippermodi
worden niet geclaimd.

De informatie wordt ook verzameld als het venster gesloten is, bij DVB-S/S2/T,
alle renderers en met Scatterfix uitgeschakeld. Bij meerdere services kiest de
ontvanger aanvankelijk de eerste service uit de PAT; in de popup kun je een andere
service kiezen. Die keuze geldt voor beide informatievensters en verandert de player niet.
De servicekeuze vermeldt naam en service-ID om verwarring met een externe player te
voorkomen. Er worden maximaal 32 services bijgehouden.

Tijdens ontvangstonderbrekingen blijft de laatst ontvangen inhoud staan. Na twee
minuten zonder nieuwe informatie meldt de popup dat er geen recente update is.
Stoppen, opnieuw starten, afstemmen met de app of een andere informatieservice kiezen
wist de oude inhoud. Verandering van transportstream-ID wist de inhoud eveneens.
Afstemmen buiten de app (bijvoorbeeld in Linrad) is alleen via veranderingen in de
ontvangen transportstream herkenbaar. Ontbrekende teletekst in de PMT en wachten op
de gekozen pagina hebben afzonderlijke statusmeldingen; ontbreken van EIT wordt niet uit
een korte wachttijd afgeleid.

De TS-informatieverwerking draait op een eigen thread met maximaal 1 MiB wachtende
TS-data. Pakketverlies of overloop reset de opbouw van secties en teletekst; de
laatst weergegeven inhoud blijft staan. CRC-foute SI-secties worden genegeerd.
De oorspronkelijke TS-uitvoer blijft byte voor byte behouden. Teletekst gebruikt
libzvbi rechtstreeks, omdat FFmpeg's bitmapinterface conceal altijd onthult en
flash permanent inschakelt. De parser controleert de PES-header, EBU-dataidentifier,
data-unitlengtes en framingcode voordat de gegevens naar de decoder gaan.
Zowel gewone teletekstunits (0x02) als ondertitelunits (0x03) worden verwerkt;
stuffing en onbekende units worden overgeslagen. Bij een decoderinitialisatiefout
blijft EIT beschikbaar.

`make test-ts-info` test de SI-parser en teletekstdecoder met gegenereerde
transportstreams, inclusief tekstcoderingen, uitgebreide beschrijvingen,
servicewissels, pakketverlies en pagina-updates. Pixeltests controleren conceal,
flash, attribuuttiming, mozaïeken, dubbele tekengrootte, nationale tekens,
koponderdrukking en rij 24. De GTK-smoketest controleert de afzonderlijke vensters,
de 4:3-pagina zonder extra widgets in het beeld, servicekeuze en reset tijdens
achtergrondverwerking.

### Eigen configuratie

Maak bij de eerste start een lokale kopie van `linrad-dvb.example.ini` als
`linrad-dvb.ini` (zie de bouwinstructies hierboven). Dit lokale bestand wordt
door Git genegeerd; **W** in de GUI bewaart alleen jouw instellingen. De
voorbeeldconfiguratie gebruikt relatieve buildpaden, geen lokale opname en
geen persoonlijke UHD-firmwaremap. Laat `[b210] images` leeg voor de normale
UHD-zoekpaden, of stel daar je eigen map in.

Pas `linrad-dvb.ini` aan voor de programmapaden en opstartopties:

- `[paths]`: C-ontvanger, `leandvb` en externe renderers.
  Een oude `rbdvbt_rx`-padinstelling wordt genegeerd.
- `[linrad]`: IPv4-adres, UDP-poort (50000), headerlengte (24), samplerate,
  IQ-formaat (`s16` of `u8`) en extra ontvangeropties.
- `[leandvb]`, `[rbdvbt_rx]`, `[cvlc]`: extra argumenten.
  `[rbdvbt_rx]` configureert nu de ingebouwde decoder; `--live` en
  `--resample-to-dvbt-rate` worden automatisch toegevoegd. Een oude `--gui`
  opent geen extra DVB-T-vensters meer.
- `[session] scatterfix`: ingebouwde parallelle reconstructie aan/uit.

Paden met een slash zijn relatief aan de map van het configuratiebestand;
losse programmanamen worden in `PATH` gezocht. Processen starten met die map
als werkmap, zodat relatieve paden hetzelfde werken vanaf elke startmap. De standaardconfiguratie wordt gevonden naast de map `build`,
onafhankelijk van de werkmap van de terminal. Een eigen bestand selecteer je met:

```bash
./build/linrad_dvb --config /pad/naar/linrad-dvb.ini
```

De configuratie wordt bij het starten van de toepassing ingelezen. Herstart de
GUI na wijzigingen. Argumenten ondersteunen aanhalingstekens voor spaties,
maar geen shell-expansie, variabelen, redirections of pipelines. Zet de parameters
die de GUI beheert niet nogmaals in de extra argumenten. Programmapaden in
`[paths]` hebben geen aanhalingstekens nodig, ook bij spaties.

### IQ-spectrum en waterfall

De GTK-GUI toont bovenaan eerst de waterval en daaronder het spectrum,
beide over de volledige breedte en voor alle invoerbronnen. De monitor meet het IQ vóór het bandfilter (in het gekozen uitgangsformaat). De blauwe
markeringen geven de ingestelde totale filterbreedte aan, met grenzen op
−BW/2 en +BW/2. Bij uitgeschakeld filter zijn de markeringen gedimd. De toetsen
`[` en `]` bedienen de bandbreedte vanuit het hoofdvenster. De BW-aanduiding staat
in het spectrum direct links van de rechter filtergrens en beweegt mee met die
lijn. De kleine **BW**-knop links in het spectrum, net onder de bovenste as,
schakelt het IQ-bandfilter aan en uit. De knop is rood wanneer het filter aan
staat. Er is geen apart IQ-bandfilterpaneel of BW-invoerveld meer.
Het spectrum in de DVB-ontvanger meet juist
het doorgegeven signaal ná het filter.

Spectrum en waterfall gebruiken dezelfde vaste, instelbare ondergrens en bereik
in dBFS/Hz; er is geen automatische niveauregeling. De kleine pijltjes rechts
in het spectrum wijzigen de schaal met 5 dB per klik: onderaan, net boven de
onderste as, stel je de ondergrens in; bovenaan, net onder de bovenste as,
het bereik. Omhoog verhoogt de waarde, omlaag verlaagt deze. De actuele waarde
staat ook in de tooltip. **W** bewaart de ondergrens en het bereik voor spectrum
en waterval in `[display] floor_dbfs_hz` en `range_db`; bij de volgende start
worden die waarden hersteld. Deze weergave-instellingen gelden voor alle bronnen.
Zonder opgeslagen waarden blijft de standaard −160 dBFS/Hz met 100 dB bereik.
Meer ingangsruis blijft dus
zichtbaar zolang de ingang niet clipt. De frequentieas toont de ontvangstfrequentie
in MHz (SDR/IF + LO-offset). Bij Linrad stel je de middenfrequentie voor de
weergave handmatig in via **SDR**; dit stemt Linrad zelf niet af.
De nieuwste waterfallregel staat bovenaan. Bij een gewijzigde frequentie of
samplerate wordt de historie gewist om oude regels niet onder een nieuwe
frequentieschaal te tonen. De filtermarkeringen tonen de huidige instelling.

De monitor gebruikt 2048-punts Hann-FFT's, met acht spectra gemiddeld in lineair
vermogen. Resolutie en updatefrequentie hangen af van de samplerate (maximaal
10 updates/s). De waterfall toont opeenvolgende ontvangen updates, geen exacte
tijdas. Telemetrie gaat via een lokale niet-blokkerende UDP-socket; bij drukte
mogen beeldupdates vervallen zonder op de GUI te wachten. De monitor wijzigt
geen IQ-samples, versterking, filtercoëfficiënten of samplerate. Er is voor deze
weergave geen aanpassing aan de DVB-ontvanger nodig.

### IQ-bestanden realtime afspelen

Stop de ontvangst en kies **SDR → Input / SDR → Bestand (IQ)**. Met **Bladeren…**
kies je de opname. De toepassing leest automatisch het JSON-bestand ernaast
(`opname.iq.json` of `opname.json`) en vult samplerate, bestandsformaat,
ontvangstfrequentie en beschikbare DVB-instellingen in. Controleer de samenvatting
onder **Mode**, sluit de SDR-instellingen en druk rechtsboven op **Start bestand**.
Dezelfde knop wordt groen en toont **Stop bestand · actief**. De opname speelt
eenmaal af; aan het einde wordt de decoderuitvoer afgewerkt en stopt de pipeline.
Een volgende Start begint opnieuw bij het begin. **W** bewaart de bestandskeuze
en instellingen voor de volgende start van de toepassing.

Ondersteunde opnamen zijn interleaved I,Q: signed 16-bit little-endian (`s16` /
`cs16_le`), unsigned 8-bit (`u8` / `cu8`) en 32-bit float little-endian
(`cf32` / `fc32`). Float wordt begrensd tot −1…+1 en naar s16 omgezet voor de
decoder. Een eventuele header wordt overgeslagen. Samplerate, bestandsformaat,
headerlengte en middenfrequentie zijn ook handmatig instelbaar onder SDR.
Zonder voldoende metadata staat er een melding om de instellingen te controleren.

De metadata van datv-transmitter en de afgeleide Airplanescatter-opnamen worden
ondersteund. Als `parameters.iq_in` naar het oorspronkelijke zendbestand verwijst,
worden de DVB-instellingen ook uit diens JSON gelezen wanneer dat aanwezig is.
De metadata van de gekozen opname heeft voorrang. Namen met bijvoorbeeld
`dvbt-qpsk-bw40000-gi1-32-fec1-2-fs250000` leveren ontbrekende DVB- en
samplerate-instellingen. De gekozen opname bepaalt altijd het sampleformaat en
de afspeelsamplerate; de samplegegevens uit het oorspronkelijke zendbestand
worden daarvoor niet gebruikt.

Waterval, spectrum, **BW** en **[ / ]** werken ook tijdens bestandsweergave.
De ontvangstfrequentie bepaalt alleen de frequentieschaal. Hardware-afstemming
en gain zijn uitgeschakeld. De ingebouwde `iq_file_to_stdout` speelt op basis van
het aantal samples en een monotone klok af; als de decoder achterloopt, vertraagt
het afspelen zonder samples over te slaan. De helper wordt met `make all` gebouwd.
**Output → Renderer** kiest zoals gebruikelijk een speler of rechtstreekse MPEG-TS-uitvoer.

Vanaf de opdrachtregel kan dit ook:

```bash
./build/linrad_dvb --iq-file /pad/naar/opname.iq
./build/linrad_dvb --iq-file /pad/naar/opname.iq --print-pipeline
./build/linrad_dvb --iq-file /pad/naar/opname.iq --run
```

De onderstaande meetnotities verwijzen naar lokale opnamen; de opnamen en
bijbehorende JSON-bestanden worden niet meegeleverd.

De oude AFEDRI-opnamen `recordings/fbn2.raw`, `recordings/org.raw` en
`recordings/h.raw` zijn Linrad RAW-bestanden met een header van **41 bytes** en
daarachter ongecomprimeerde `cs16_le` IQ-data. Hun headers vermelden
**1010526 samples/s** en een ontvangermiddenfrequentie van **28 MHz** (de eventuele
RF-frequentie vóór een converter is onbekend). Conversie is niet nodig: de
bijbehorende `.raw.json`-bestanden stellen dit automatisch in. De opnamen duren
respectievelijk ongeveer 3 min 36 s, 10 min 18 s en 1,11 s.

Decodertests op fragmenten bevestigen **DVB-S2 QPSK, 125 ksym/s, FEC 1/2**
voor `fbn2.raw` (DL4FBN, H.264) en **DVB-S QPSK, 125 ksym/s, FEC 1/2** voor
`org.raw` (PE1ORG, HEVC en AAC). `fbn2.raw` gebruikt korte DVB-S2-frames;
`[leandvb] dvbs2_args` bevat daarom `--framesizes 3` om zowel normale als korte
frames te accepteren. De ontvangst is in de geteste fragmenten onderbroken;
continu beeld is niet bevestigd. Voor `h.raw` is geen lock gevonden; daar blijft
DVB-S, 125 ksym/s, FEC 1/2 een voorlopige instelling.

```bash
./build/linrad_dvb --iq-file recordings/fbn2.raw
./build/linrad_dvb --iq-file recordings/org.raw
./build/linrad_dvb --iq-file recordings/h.raw
```

Druk in de GUI op **Play file**. Via **Input → File (IQ) → Browse** kun je een
andere opname kiezen. Bewaar de `.raw.json` naast de bijbehorende `.raw`.

### USB/FM/FM-DATA-monitor voor bakens, spraak en packet

Naast **Teletekst** staan **USB**, **FM** en **FM-DATA**. Met **U/u**, **F/f**
en **D/d** schakel je de bijbehorende stand aan of uit. Er is één parallelle
audio-ontvanger: er kan maar één stand tegelijk aanstaan. Omschakelen bewaart de afstemfrequentie,
de speakerverbinding en de TCI-clients. De actieve knop licht oranje op.

- **USB:** Upper Side Band, **3 kHz** breed vanaf de beatfrequentie.
  De spectrumlijnen staan op de beatfrequentie en **+3 kHz**.
- **FM:** smalband communicatie-FM, **12,5 kHz** breed rond de draaggolf,
  met nominale piekdeviatie **±2,5 kHz**, een spraakfilter van circa 300–3000 Hz
  en 75 µs de-emphasis. Het spectrum toont de middenfrequentie en de grenzen
  op **−6,25 en +6,25 kHz**. Dit is een open monitor zonder squelch.
- **FM-DATA:** **20 kHz** breed rond de draaggolf, met spectrumgrenzen op
  **−10 en +10 kHz**. Vlakke discriminatoraudio zonder spraakfilters,
  de-emphasis, audio-AGC of squelch, inclusief behoud van DC. Vaste versterking
  geeft ±0,25 bij ±5 kHz deviatie; de uitgang heeft begrenzing op ±0,95.
  RF-filtering en anti-aliasfiltering bij resampling blijven actief.
  Dit is een uitgang voor externe modemsoftware, geen ingebouwde packetdecoder.

**FM-N/FM-W** verwijzen naar narrow/wide FM; de audioverwerking verschilt per
programma. [SDR Console](https://www.sdr-radio.com/modes) gebruikt bijvoorbeeld
Wide FM zonder de-emphasis. Hier geeft **FM-DATA** expliciet het vlakke audiopad
aan. Een bredere spraak-FM-stand alleen is onvoldoende voor 9600-baud
packet. De vaste 20 kHz is een uitgangspunt voor G3RUH-signalen; de benodigde
RF-bandbreedte hangt af van de deviatie en pulsfiltering van de zender.
De externe modem moet TCI-audio ondersteunen, of via een afzonderlijke audiobrug
worden aangesloten. Dire Wolf kan bijvoorbeeld de discriminatoraudio decoderen,
maar heeft hiermee niet automatisch een rechtstreekse TCI-ingang.

De afstemfrequentie staat links van de midden-/beatfrequentielijn. Klik met de
linkermuisknop in het spectrum om de ingeschakelde monitor daar af te stemmen.
**Page Up** stemt lager af (naar links), **Page Down** hoger (naar rechts):
**100 Hz per stap bij USB**, **2,5 kHz per stap bij FM en FM-DATA**. **↑ / ↓** blijven de
SDR-gain bedienen. Klikken en de afstemtoetsen veranderen alleen de monitor;
SDR-frequentie, DVB-filterbreedte en DVB-processen blijven behouden.
Bij het veranderen van de SDR-middenfrequentie volgt de monitor met dezelfde
afstand tot het midden. Bij Linrad moet de middenfrequentie in **Input** kloppen;
de LO-offset wordt meegenomen voor directe SDR-bronnen.

Alle drie standen geven audio naar de standaard speakeruitgang (ALSA, eventueel
via PipeWire/PulseAudio) en naar TCI. **M/m dempt uitsluitend de speakers**:
zowel de ingebouwde DVB-player als de USB/FM/FM-DATA-monitor. De audio naar **TCI en PCM UDP wordt niet gedempt**. De mute-instelling blijft behouden
bij omschakelen tussen de drie standen; de TCI-verbinding blijft open.
UDP-uitvoer en TS-opnames behouden eveneens hun audio.

De monitor gebruikt de IQ-bron die met **Start SDR** of **Play file** loopt.
Zonder actieve bron blijft TCI bereikbaar en staat in het TCI-venster
**Waiting for IQ**. De volledige monitorband moet in het beschikbare IQ-spectrum
passen. FM vereist minstens **12.500 IQ-samples/s**, FM-DATA **24.000**;
bij een te smalle bron of afstelling blijft een mislukte omschakeling in de vorige stand.

TCI is standaard bereikbaar op **`ws://127.0.0.1:40031`**. De poort is aanpasbaar
via **TCI** (ook **Alt+T**), wanneer de monitor uitstaat.
**W** in het hoofdvenster bewaart de instelling. Voor bijvoorbeeld MSHV kies je
**TCI Client RX1**, host `127.0.0.1`, poort `40031`, en **TCI Client Input** als
audio-input. De RX-audio is mono, ook als MSHV op twee TCI-kanalen staat.
Met **TCI audio output (dB)** stel je het niveau voor alle verbonden
TCI-clients tijdens ontvangst in, van −60 tot 0 dB. De standaard is 0 dB.
Als MSHV te veel niveau aangeeft, probeer dan −18 dB met de MSHV-ingangsregelaar
in het midden. Druk op **W** in het hoofdvenster om de instelling te bewaren.
TCI geeft de frequentie, modus (**USB** of **NFM**, ook voor FM-DATA) en filtergrenzen
door. Clients kunnen binnen het IQ-spectrum afstemmen; de modulatie kies je
met de USB/FM/FM-DATA-knoppen. De server is uitsluitend voor ontvangst.

De optionele INI-sectie is:

```ini
[usb]
tci_bind=127.0.0.1
tci_port=40031
tci_level_db=0
audio_device=default
pcm_udp_enabled=false
pcm_udp_host=127.0.0.1
pcm_udp_port=7355
```

Voor een TCI-client op een andere computer stel je `tci_bind` in op het lokale
LAN-adres van deze computer. TCI gebruikt WebSocket zonder authenticatie of TLS;
gebruik dit alleen binnen een vertrouwd netwerk. `audio_device` kan een andere
ALSA-uitgang zijn. Als de speakeruitgang ontbreekt, meldt de monitorstatus dat en
blijft TCI-audio beschikbaar.

De monitortak kopieert IQ **vóór** het DVB-bandfilter naar een begrensde buffer.
Demodulatie, speaker en TCI draaien in een aparte thread. De bron wacht nooit op
die buffer: bij achterstand vallen alleen monitorblokken weg, zichtbaar in de
status in het TCI-venster en de tooltip van de actieve USB/FM/FM-DATA-knop.
Trage TCI-clients worden afzonderlijk verbroken. De monitor uitschakelen sluit de speaker en de TCI-poort; de DVB-processen blijven draaien. Deze extra
verwerking kost wel CPU-tijd en geheugenbandbreedte, vooral bij hoge IQ-rates.

De TCI-implementatie volgt de RX-audiowerking van `tci-radio-bridge` en het
[TCI-protocol](https://github.com/ExpertSDR3/TCI). Standaard wordt mono float32
audio op 48 kHz verstuurd na `audio_start:0;`, met een 64-byte little-endian
TCI-header. Clients kunnen ook int16 kiezen.
USB en spraak-FM ondersteunen daarnaast 24/12 kHz. **FM-DATA gebruikt altijd
48 kHz** om aliasing door ongefilterde decimatie te voorkomen. Bij omschakelen
meldt de server de nieuwe samplerate aan bestaande clients; verzoeken om
24/12 kHz krijgen in FM-DATA de werkelijke waarde 48 kHz terug.
`make test-usb` controleert USB-zijband- en aliasonderdrukking, FM-demodulatie,
AM-onderdrukking, het spraakfilter, de vlakke FM-DATA-respons en DC,
live omschakelen, TCI-audio tijdens speaker-mute,
resampling, frequentie via WebSocket, trage clients en ongewijzigde DVB-IQ bij een volle monitorbuffer.
Als Dire Wolf-tools `gen_packets` en `atest` aanwezig zijn, controleert de test ook
het decoderen van een gegenereerd 9600-baud G3RUH-packet na FM-modulatie en
FM-DATA-demodulatie, zonder foutcorrectie van ongeldige CRC's.
Met `direwolf` en `gen_packets` test hij bovendien de volledige route van
synthetische IQ via FM/1200 AFSK en FM-DATA/9600 G3RUH naar PCM UDP en de echte
Dire Wolf-decoder, terwijl de speaker gedempt is.

### PCM UDP naar Dire Wolf

Open **TCI** (**Alt+T**) en vink **Send PCM audio via UDP** aan. Standaard is
de bestemming **127.0.0.1:7355**; de optie staat aanvankelijk uit. Het doeladres
is een numeriek IPv4-adres. Schakel PCM tijdelijk uit om adres of poort te
wijzigen; de monitor en TCI hoeven daarvoor niet uit. **W** in het hoofdvenster
bewaart ook deze instellingen.

De uitgang volgt de geselecteerde monitorstand, ook bij live omschakelen:

| Ontvangst | Stand in Linrad DVB | Dire Wolf |
| --- | --- | --- |
| 1200-baud Bell 202 AFSK-packet | **FM (F)** | `-B 1200` / `MODEM 1200` |
| 9600-baud G3RUH-packet | **FM-DATA (D)** | `-B 9600` / `MODEM 9600` |

Ook **USB** kan audio naar dezelfde uitgang sturen. Er is één monitor en dus
één audiostroom; de baudrate stel je in de externe modem in. Voor Dire Wolf
zijn geen TCI-bridge en geen virtuele geluidskaart nodig. Start de SDR of een
IQ-bestand, schakel de gewenste monitorstand in en stem de zender af.

Vanuit deze projectmap kun je het meegeleverde ontvangstvoorbeeld gebruiken:

```bash
# Gebruik FM in Linrad DVB:
direwolf -c direwolf-pcm.conf -B 1200

# Of gebruik FM-DATA in Linrad DVB:
direwolf -c direwolf-pcm.conf -B 9600
```

Start één van beide opdrachten. Het bestand `direwolf-pcm.conf` bevat alleen
ontvangstinstellingen voor 48 kHz mono-audio; het zet geen beacon, IGate of
KISS/AGW-server aan. Pas bij een andere UDP-poort ook `ADEVICE UDP:7355 null`
aan. TCI blijft op zijn eigen poort **40031** werken.

PCM UDP levert **48.000 samples/s, mono, signed 16-bit little-endian**, zonder
WAV-, TCI- of RTP-header: **480 samples / 960 bytes per datagram** (10 ms).
De uitgang volgt het audiopad van de gekozen stand en heeft geen eigen
spraakfilters of versterkingsregeling. **M** beïnvloedt alleen de speakers.
Bij uitgeschakelde monitor of ontbrekende IQ stopt de stroom; de aangevinkte
optie blijft klaarstaan voor de volgende ontvangst. UDP verzendt zonder op een
ontvanger te wachten en zonder hertransmissie. Verzendfouten staan in de
monitorstatus; verlies verderop in het netwerk is hiermee niet meetbaar.

### Renderer en rechtstreeks MPEG-TS opslaan

Via **Output → Renderer** kies je **Embedded player (right panel)**, **UDP stream (external player)**,
VLC, VLC met bediening, **FFplay**, een eigen programma of **MPEG-TS-bestand**.
De rendererkeuze staat in dit menu; Scatterfix bedien je met
de knop naast **BW** in het spectrum.
Stop de ontvangst voordat je de uitvoer wijzigt. **W** bewaart de keuze in de INI.

De ingebouwde player toont beeld in het vaste rechterkader en speelt geluid af.
Met **M** (of **m**) in het hoofdvenster schakel je speaker-mute aan of uit
voor de ingebouwde DVB-player en de USB/FM/FM-DATA-monitor; TCI-audio blijft doorlopen. Het videokader
toont **Muted (M)** zolang het geluid gedempt is. De keuze blijft tijdens deze
app-sessie behouden, ook na Stop/Start. UDP-uitvoer en TS-opnames behouden hun audio;
externe players bedienen hun eigen geluid.
Vink **Also send video/audio via UDP** aan om tegelijk een externe player te voeden.
Met **UDP stream (external player)** wordt alleen UDP verstuurd, zonder lokaal beeld
of geluid. Het rechterkader behoudt ook dan zijn afmetingen. Stel het IPv4-adres
van de externe player en de doelpoort in; standaard is dit `127.0.0.1:1234`.
Voor een afzonderlijke data-uitgang vink je **Send DSM-CC from all programs via UDP** aan.
Vul een unicast- of multicastadres en poort in (standaard `127.0.0.1:1235`).
Deze uitgang volgt de PAT en de PMT’s van alle services op de ontvangen multiplex,
onafhankelijk van het gekozen programma en de volledige UDP-uitgang. De datagrammen
bevatten uitsluitend originele TS-pakketten van de aangekondigde DSM-CC-PID’s.
PAT en PMT worden lokaal gebruikt voor detectie. Een app kan de PID uit de
TS-header lezen. Bij multicast naar `239.255.42.2:1235` kun je de secties bekijken
met `tsp -I ip 239.255.42.2:1235 -P tables --all-sections -O drop`.

Met het statusendpoint ingeschakeld kan een app ATV Mosaic-berichten ophalen via
`GET /dsmcc/messages`, ook als DSM-CC via UDP uitstaat. De ontvanger leest
`station.json` uit de single-file BIOP-carrousel van Mosaic (één DDB-blok).
Chat gebruikt `id` (UUID), `from`, `to`, `type: "chat"` en `text`. Voor een
antwoord gebruikt de ontvanger de uitbreiding `type: "response"` met een eigen UUID
en `reply_to` met de UUID van de opdracht. Extra JSON-velden blijven beschikbaar.

De eerste GET geeft de berichten uit het afgelopen uur in ontvangstvolgorde. Elk item bevat
`message`, `pid`, `received_unix_ms` en `cursor`. Bewaar na verwerking de
`latest_cursor` en vraag vervolgens `GET /dsmcc/messages?after=<cursor>` op.
`/status` bevat onder `dsmcc` de route en de nieuwste cursor. Een bericht vervalt 60 minuten na de eerste ontvangst. Herhaling van dezelfde UUID
binnen dat uur verlengt die termijn niet. De berichten staan alleen in het werkgeheugen.
Stoppen wist ze ook als de applicatie open blijft. Bij een verlopen cursor geeft
`gap: true` aan dat de app berichten kan hebben gemist; de respons bevat dan
alle nog bewaarde berichten.

Open in de ontvangende VLC `udp://@:1234`, of in FFplay `udp://0.0.0.0:1234`.
De externe player bepaalt zelf zijn venstergrootte. De ontvangen MPEG-TS wordt
ongewijzigd doorgestuurd, zonder hercoderen. TSscatterFix krijgt een parallelle
kopie en toont de herstelde afbeelding in zijn eigen paneel. De player en
UDP-uitvoer blijven de oorspronkelijke stream ontvangen.

De ingebouwde weergave gebruikt libVLC-videocallbacks en GTK; ook onder Wayland
en op een GTK-testdisplay is geen apart VLC-venster nodig. Dit gebruikt
softwaredecodering en schaalt het beeld in de applicatie.

Een eigen programma krijgt de transportstream op stdin. Geef bij **Programma / pad**
alleen het uitvoerbare bestand op en vul de opties bij **Argumenten** in.
`{input}` wordt vervangen door `-` voor stdin.
Gebruik de invoersyntaxis die het gekozen programma ondersteunt; een programma
dat stdin zonder argument leest, kan lege argumenten gebruiken. Argumenten
worden zonder shell uitgevoerd.

**MPEG-TS-bestand** schrijft de decoderuitvoer rechtstreeks naar de opgegeven
bestandsnaam. TSscatterFix kan ook hierbij parallel werken. Relatieve paden zijn relatief aan
de INI-map. De doelmap moet bestaan en het bestand moet nieuw zijn; een bestaand
bestand wordt niet overschreven. Stop sluit de opname. De bytes worden ongewijzigd
opgeslagen: de ontvanger voegt geen ontbrekende TS-pakketten toe.

Een opgeslagen rendererkeuze blijft behouden. Zonder rendererkeuze gebruikt de
GUI `embedded`; de opdrachtregel zonder GUI behoudt `cvlc` als standaard.
`[session] renderer` is `embedded`, `udp`, `srt`, `cvlc`, `vlc`, `ffplay`, `custom` of `mpegts`.
`srt` gebruikt de instellingen onder `[srt]` en maakt Full HD-uitvoer voor een
externe SRT-listener; zie de headless repeaterbeschrijving hierboven.
`[renderer] udp_enabled`, `udp_host` en `udp_port` regelen de extra UDP-uitvoer
(standaard `false`, `127.0.0.1`, `1234`). Bij renderer `udp` is UDP altijd actief.
Met `--renderer` kun je de opgeslagen keuze overschrijven. `embedded` en `udp`
vereisen de GUI; gebruik bijvoorbeeld `--run --renderer cvlc` voor de bestaande
opdrachtregelworkflow.
Voor een eigen programma gebruik je `[paths] custom_renderer` en `[renderer] args`;
voor bestandsuitvoer `[renderer] output_file`. VLC gebruikt de bestaande
`[cvlc] args`; `[paths] vlc` is standaard `vlc`.
FFplay gebruikt `[paths] ffplay` (standaard `ffplay`) en eigen opties in
`[ffplay] args` (standaard `-hide_banner -loglevel warning -autoexit -f mpegts`).
Het leest de oorspronkelijke MPEG-TS via `pipe:0`, ook met TSscatterFix aan.
Installeer ffplay als het nog niet op het systeem staat; bestaande INI-bestanden
krijgen de nieuwe standaardwaarden automatisch.
