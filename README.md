# LeanDVB receive helpers for multiple SDR sources

For the new Linrad-only C receiver and GUI, run `make` followed by
`./build/linrad_dvb`. Set parameters directly in the GUI and edit
`linrad-dvb.ini` for paths and startup options. See
[Linrad DVB ontvanger in C](#linrad-dvb-ontvanger-in-c) below.
The profile-driven scripts described elsewhere in this README are the older
experimental workflow.

This repository contains a small collection of experimental command-line helpers for receiving narrowband DVB-S and DVB-S2 signals with **LeanDVB**.

The setup is built around the main launcher script:

- `rx_dvb_linrad_leandvb.sh`

That script selects a predefined receive profile and connects one of several SDR input paths to `leandvb`, usually followed by `cvlc` for MPEG-TS playback. The profiles are intentionally practical and experimental. This older workflow uses shell commands, pipes, and profile numbers; the new Linrad GUI operates independently of it.

Useful upstream references:

- LeanDVB / LeanSDR: <http://www.pabr.org/radio/leandvb/leandvb.en.html>
- Linrad: <https://www.sm5bsz.com/linuxdsp/linrad.htm>

## Project goal

The goal of this repository is to make it easy to try DVB-S and DVB-S2 reception with different SDR devices while keeping a largely uniform downstream pipeline:

```text
SDR or Linrad output -> helper/converter -> leandvb -> VLC / TS tools
```

The helpers mainly solve practical integration issues such as:

- getting IQ into `stdin`
- converting between `s16`, `s8`, and `u8`
- controlling specific radios
- measuring IQ levels or SNR
- inspecting MPEG transport streams

## Main entry point

### `rx_dvb_linrad_leandvb.sh`

This is the central launcher for the whole setup. It defines a set of numbered profiles and hides most of the pipe plumbing.

Current profile groups include:

- Linrad UDP input to LeanDVB
- AFEDRI direct UDP input to LeanDVB
- RTL-SDR direct input to LeanDVB
- HackRF direct input to LeanDVB
- RFSpace SDR-IQ direct input to LeanDVB
- measurement-only profiles for input levels and SNR
- optional transport stream inspection via TSDuck

Show the available profiles with:

```bash
./rx_dvb_linrad_leandvb.sh --info
```

Run a profile with:

```bash
./rx_dvb_linrad_leandvb.sh --profile 4
```

The launcher currently includes profiles 1 through 18, covering DVB-S, DVB-S2, measurement, and multiple source devices. fileciteturn1file0

## Linrad DVB ontvanger in C

De toepassing ondersteunt Linrad, directe SDR-hardware en IQ-bestanden. Je stelt de parameters
rechtstreeks in de GTK-GUI in; er zijn geen profielen nodig. Zowel de GUI als de
Linrad UDP-ontvanger zijn in C geschreven. `rx_dvb_linrad_leandvb.sh` wordt niet
gelezen, gestart of gewijzigd door de toepassing. De overige scripts hieronder
blijven beschikbaar voor de oudere experimenten.

Bouwen en starten (Linux, C-compiler, `pkg-config` en development packages voor
GTK 3, GLib, json-c, FFTW3 single precision, SoapySDR, UHD, FFmpeg
(`libavcodec`, `libavformat`, `libavutil`, `libswscale`), libzvbi (`zvbi-0.2`) en libVLC 3
(inclusief de VLC-codecplugins)):

```bash
make
./build/linrad_dvb
```

Voor libzvbi zijn ook de ontwikkelbestanden nodig: `sudo dnf install zvbi-devel`
op Fedora, of `sudo apt install libzvbi-dev` op Debian/Ubuntu.

De GUI gebruikt één venster zonder tabs, standaard ongeveer 80% van de
schermbreedte en 90% van de beschikbare hoogte. Bovenaan staan de menu's
**Input**, **Mode**, **Renderer** en **About**. About toont een korte uitleg,
Rob Hardenberg / PE1ITR als maker, de builddatum en de sneltoetsen in een raster.
De waterval en het spectrum staan bovenaan over de volle breedte. Daaronder staan
links de decoderinformatie en daarnaast een vast playerkader met dezelfde breedte
en hoogte als Service info. Rechts daarvan verschijnt het TSscatterFix-paneel
wanneer TSscatterFix is ingeschakeld.
Status en log staan onder deze rij. De ontvangen
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

- **Input**: input, samplerate, IQ-formaat, LO-offset, afstemstap,
  device-argumenten, antenne en UHD-images;
  bij Linrad het UDP-adres, de poort, de headerlengte en de frequentie voor de schaal.
  Kies **Bestand (IQ)** om een opname met bijbehorende JSON-instellingen af te spelen.
- **Mode**: DVB-mode, symbolrate of BW, FEC, guard interval en constellatie.
- **Renderer**: ingebouwde player, UDP naar een externe player, VLC (`cvlc`),
  VLC met bediening, FFplay, een eigen programma of een MPEG-TS-bestand.
- **Alt+I / Alt+M / Alt+R / Alt+A**: open respectievelijk Input, Mode, Renderer en About.
  Kleine letters werken ook; houd Alt ingedrukt om een menu te openen.
- **R**: reset de actieve TSscatterFix-reconstructie. Herstelbuffer, beeld en
  status worden gewist; reconstructie begint opnieuw met nieuwe TS-data.
  De ontvangst en gewone videospeler blijven doorlopen.
- **U**: geluid van de ingebouwde player dempen; nogmaals **U** schakelt het weer in.
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

- DVB-S en DVB-S2: SR 35000, 66000, 125000, 250000 of 333000; FEC 1/2 of 2/3.
- DVB-T: BW 35000, 40000, 150000, 250000, 333000 of 500000 Hz; FEC 1/2, 2/3, 3/4, 5/6 of 7/8;
  guard interval 1/32, 1/16, 1/8 of auto.
- Scatterfix aan/uit, één Start/Stop-knop, een pipelineweergave en een logvenster.

DVB-S/S2 gebruiken `leandvb`. De DVB-T-broncode wordt rechtstreeks meegecompileerd
en gelinkt in `linrad_dvb`; een losse `rbdvbt_rx`-executable is niet nodig.
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

TSscatterFix wordt meegecompileerd uit de vastgelegde C-broncode in
[third_party/tsscatterfix](third_party/tsscatterfix/README.md). Er is geen externe
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

### EIT en klassieke teletekst

Naast **TSscatterFix** staan twee kleine knoppen: **EIT** en **Teletext**.
Elke knop is grijs zolang er nog geen inhoud beschikbaar is en wordt groen zodra
voor de gekozen service EIT-programmainformatie of teletekstpagina 100 is ontvangen.
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
Ontvangen subpagina's van pagina 100 volgen de uitzending; er is geen paginabrowser.

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
pagina 100 hebben afzonderlijke statusmeldingen; ontbreken van EIT wordt niet uit
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

### Ingebouwde DVB-T-decoder en verdeling van het werk

`make` bouwt de vastgelegde broncode in `third_party/rbdvbt` mee in
`build/linrad_dvb`. De oorspronkelijke auteur is **Rob Hardenberg / PE1ITR**;
herkomst, lokale aanpassingen en licentie staan in
[third_party/rbdvbt/README.md](third_party/rbdvbt/README.md).
Er is tijdens bouwen of ontvangen geen andere checkout of decoderbinary nodig.
`make package` legt ook de decoderlicentie in `build` voor distributie.

De GUI start voor DVB-T dezelfde executable met `--decoder-worker=dvbt`.
Dit werkproces gebruikt de bestaande procesbewaking en Start/Stop-afhandeling,
maar voert de meegecompileerde C-functies uit. Dit houdt de GTK-eventloop vrij
van DSP-werk en maakt een volgende start onafhankelijk van oude decoderstatus.

| Onderdeel | Werk |
|---|---|
| GTK-hoofdproces | Bediening en spectrum/waterval op 10 updates/s; decoderpaneel op maximaal 20 updates/s |
| Bronhelper | SDR/Linrad/bestandsinvoer, bandfilter en bestaande IQ-monitor; hardware-uitvoer heeft een eigen begrensde wachtrij |
| `dvbt-iq` | IQ uit de pipe lezen en in de begrensde invoerring plaatsen |
| `dvbt-ofdm` | Resampling, timing/AFC, FFT, pilots en equalizing |
| `dvbt-fec` | Deinterleaving, doorlopende Viterbi, Reed–Solomon, TS-validatie en uitvoer |
| Renderer | Oorspronkelijke video/audio afspelen |
| TSscatterFix | Parallelle TS-kopie verwerken; RGB-beeld en status aan GTK doorgeven |

De OS-scheduler kan deze threads/processen over de beschikbare cores verdelen;
er is geen vaste core-affiniteit of realtimescheduling. De FEC-keten behoudt
zijn volgorde en decoderstatus binnen één worker. Meer FEC-workers zonder een
ander algoritme zouden die continuïteit verbreken. De GUI deelt geen DSP-locks:
kleine lokale UDP-berichten zijn niet-blokkerend en mogen worden overgeslagen.
Een langzaam renderprogramma kan de TS-uitvoer wel ophouden; de bestaande
begrensde buffers en overflowmeldingen blijven daarom nodig.

Het vierkante constellatiepaneel staat direct onder het spectrum, links van
**Service-info**. Dat toont OFDM/FEC/TS/service-lampjes, SNR, pilot-lock, CFO,
IQ-niveaus, service- en providernaam en pakketstatistieken in een compact raster
met twee kolommen, dunne scheidingslijnen, rechts uitgelijnde meetwaarden en
aparte eenheden. De constellatie gebruikt
dunne limegroene punten, een assenkruis en QPSK-referentiekruisjes zoals het
oorspronkelijke decodervenster. Nieuwe punten worden ook tijdens demodulatie
doorgegeven, met maximaal 20 updates per seconde. Het constellatievenster houdt
uitsluitend in het GUI-proces een rollende historie van een halve seconde bij
(maximaal 12 updates), met vervagende oudere punten. Bij een korte onderbreking
blijft het laatste beeld staan; Stop/herstart wist ook de historie. Dit verandert
de decoder, blokgrootte en signaalverwerking niet. De aanvoer van nieuwe punten
blijft afhankelijk van de decoder: 64 OFDM-symbolen vertegenwoordigen bij
40 kHz BW en GI 1/32 ongeveer 2,96 seconden signaal. **Buffer** is het
percentage bezette plaatsen in de OFDM→FEC-wachtrij van 1024 symbolen, exclusief
het blok dat wordt verwerkt. Deze vulling wordt bij iedere wachtrijwijziging
doorgegeven, ook als nog geen TS-lock bestaat. Onbekende gegevens tonen `—`.
Bij Stop/herstart worden alle gegevens gewist; na tien seconden zonder actuele
telemetrie worden lampjes grijs en oude meetwaarden verborgen. Er zijn geen
extra DVB-T-spectrum- of buffervensters. De monitorinterface en GTK-panelen zijn
afzonderlijke modules voor later gebruik door andere modes; DVB-S/S2 gebruiken
voorlopig nog hun bestaande decoderweergave.

Alle vijf DVB-T-FEC-verhoudingen (**1/2, 2/3, 3/4, 5/6 en 7/8**) gebruiken de
doorlopende Viterbi-route. De oude code beperkte die route tot 1/2 en 2/3 en
viel voor hogere verhoudingen terug op opnieuw decoderen per blok.
Dit zijn de verhoudingen uit [DVB BlueBook A012, §4.3.3](https://dvb.org/wp-content/uploads/2019/12/a012_dvb-t_june_2015.pdf).
Hogere verhoudingen geven bij gelijke modulatie/BW/GI meer nuttige bitrate en
minder foutbescherming. De FEC moet overeenkomen met de zender.
Voor 333 en 500 kHz BW moet de IQ-bron voldoende bandbreedte en samplerate
leveren. De nominale DVB-T-samplerates bij interpolatiefactor 1 zijn ongeveer
381 en 571 ksample/s; een hogere bronrate wordt intern geresampled. De SDR-IQ
met maximaal 196078 samples/s is voor die bandbreedtes onvoldoende. Kies een
geschikte Linrad-uitvoer, HackRF/B210 of IQ-opname; controleer ook het bandfilter.
`make test-dvbt` controleert de werkelijke decoder voor alle vijf FEC's met
bekende gecodeerde bytes, puncturing, bitfouten, blokovergangen en resets.
In een synthetische realtime QPSK/2K-test op 250 kHz (10,5 s IQ, deze machine)
daalde de FEC-thread-CPU-tijd voor 3/4 van circa 1,72 naar 0,81 s. De maximale
wachtrijvulling was 6,25%, zonder overflow. Een aparte H.264-test leverde
service/provider en 1510 TS-pakketten op; alle uitgevoerde pakketten kwamen
bytegetrouw uit de originele TS. Dit is geen garantie voor zwakke RF-signalen,
andere computers of hogere bandbreedtes.

Aanvullende synthetische realtime-tests op dezelfde machine controleerden
5/6 en 7/8 op 250 kHz, 5/6 op 333 kHz (7,88 s IQ) en 7/8 op 500 kHz
(5,25 s IQ). Alle runs leverden service-info en bytegetrouwe TS-pakketten
zonder wachtrij-overflow. Bij 500 kHz/7/8 was de maximale wachtrijvulling 12,5%;
de FEC-thread gebruikte circa 1,10 CPU-seconde. Dit zijn korte functionele
doorvoertests, geen langdurige RF- of belastingstest.

### Eigen configuratie

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

### C-ontvanger zelfstandig gebruiken

`build/linrad_udp_to_stdout` vervangt `linrad_udp_to_stdout_v3.py` in nieuwe
pipelines. Het bestaande Python-bestand blijft beschikbaar als testreferentie
en voor de ongewijzigde oudere scripts.

```bash
./build/linrad_udp_to_stdout --help
./build/linrad_udp_to_stdout --s16-gain 2 --measure 3200000 > /dev/null
./build/linrad_udp_to_stdout --record-iq recordings
./build/linrad_udp_to_stdout --record-iq recordings --record-stdout | vervolgprogramma
```

Ondersteund: header verwijderen, s16-doorvoer/gain, u8-conversie met scale en
offset, meetstatistieken, IQ-opnamen en optionele gelijktijdige stdout-uitvoer.
Afronding en clipping komen overeen met de Python-versie. Meten rondt een
byte-aantal naar beneden af op complete IQ-paren; daardoor kan ook een niet
uitgelijnd meetaantal afronden. Te grote UDP-pakketten worden verworpen in plaats
van gedeeltelijk doorgestuurd; stel `--bufsize` zo nodig hoger in (standaard 8192).
Een oneven laatste payloadbyte wordt, net als in Python, weggelaten.
Opnamen overschrijven geen bestaand bestand met dezelfde naam.

Voor opname vanuit de GUI voeg je bijvoorbeeld `--record-iq recordings` toe
bij `[linrad] args`. De GUI voegt altijd `--record-stdout` toe zodat de decoder
IQ blijft ontvangen. De ontvanger wijzigt de samplerate niet; de ingestelde waarde
moet overeenkomen met de uitvoer van Linrad.

### Controle zonder GUI

```bash
./build/linrad_dvb --print-pipeline --standard DVB-S2 --sr 66000 --fec 2/3
./build/linrad_dvb --print-pipeline --standard DVB-T --sr 250000 --gi 1/16 --scatterfix
./build/linrad_dvb --run --standard DVB-S --sr 125000 --fec 1/2
make test
```

`--run` start dezelfde pipeline zonder GUI en stopt met Ctrl+C. Zonder deze
vlag worden eventuele commandoregelparameters de beginwaarden van de GUI.
`--print-pipeline` toont de argumenten zonder ontvangers of VLC te starten; de
parallelle TSscatterFix-tak in de weergave is een beschrijving, geen shellcommando.

De regressietests gebruiken Python 3 als testhulpmiddel. Ze vergelijken alle
s16-waarden met de oorspronkelijke Python-conversie, controleren de
parametercombinaties en testen IQ-opnamen, UDP/pipe-doorvoer en procesopruiming
met vervangende decoders. Als `broadwayd` beschikbaar is, testen ze ook de GTK-
keuzes, Stop, herstarten en sluiten op een eigen onzichtbaar testscherm.
Deze tests vervangen geen ontvangsttest met een echt DVB-signaal uit Linrad.

## Repository contents

### `rx_dvb_linrad_leandvb.sh`
The main profile-driven launcher script. It contains all predefined receive chains and is the preferred starting point. fileciteturn1file0

### `linrad_udp_to_stdout_v2.py`
Receives raw Linrad UDP IQ and writes the payload to stdout. It supports plain `s16` pass-through, optional `s16` gain, and conversion from `s16 IQ` to `u8 IQ` using:

```text
u8 = clip(offset + s16*scale/256)
```

It also supports one-shot level measurement for both input and output IQ. fileciteturn1file8

### `afedri-udp.py`
Receives AFEDRI UDP IQ packets and writes raw `s16` IQ payload to stdout. It supports optional source IP filtering and packet loss statistics based on sequence numbers. fileciteturn1file10

### `afedri-control.py`
Original AFEDRI control helper for setting frequency, rate, and gain and starting/stopping the stream. fileciteturn1file9

### `afedri-control-fixed.py`
Improved AFEDRI/AFE822x control helper with more complete command handling, AGC support, corrected RF gain coding, single-channel mode handling, and detailed reply logging. fileciteturn1file6

### `soapy_sdriq_to_stdout_v2.py`
Reads RFSpace SDR-IQ samples via SoapySDR and writes them to stdout as either `s16` IQ or converted `u8` IQ. It supports the same `u8` scaling philosophy as the Linrad helper. Supported sample rates are constrained to a defined set, and ATT gain is quantized to the device step values. fileciteturn1file1

### `hackrf_s8_to_u8.py`
Converts the HackRF signed 8-bit interleaved IQ stream to unsigned 8-bit IQ for use with `leandvb --u8`. Optional I/Q swap and throughput statistics are supported. fileciteturn1file3

### `iq_band_snr_meter.py`
Reads complex `s16` IQ from stdin and estimates signal and noise power in the frequency domain. It reports C+N/N, SNR, and equivalent SNR normalized to a reference bandwidth. fileciteturn1file7

### `tsp_monitor.py`
Consumes output from `tsp -P analyze`, stores the latest block, and logs newly observed service/provider combinations with one-hour suppression of duplicates. fileciteturn1file5

### `dvbs2_modcod.py`
Helper tool to look up DVB-S2 MODCOD values and combined `--modcods` masks for LeanDVB based on constellation and FEC or FEC ranges. fileciteturn1file2

## General receive architecture

The common downstream idea is always the same:

```text
Source IQ -> format helper -> leandvb -> VLC
```

or for measurement:

```text
Source IQ -> helper -> iq_band_snr_meter.py
```

or for MPEG transport stream inspection:

```text
Source IQ -> helper -> leandvb -> tee -> VLC
                                 -> tsp analyze -> tsp_monitor.py
```

## Device-specific sections

## 1. Linrad network output

Linrad is used here as an external frontend that already receives and conditions the signal. This repository then consumes the **UDP IQ output from Linrad**.

Related website:

- <https://www.sm5bsz.com/linuxdsp/linrad.htm>

### Driver / helper used

- `linrad_udp_to_stdout_v2.py`

### What it does

- listens to Linrad UDP IQ
- strips the Linrad UDP header
- outputs interleaved IQ on stdout
- can output `s16` directly
- can convert `s16` to `u8`
- can measure input and output IQ statistics fileciteturn1file8

### Relevant options

- `--ip` — bind/listen IP, default `127.0.0.1`
- `--port` — UDP port, default `50000`
- `--skip` — bytes skipped at the start of each Linrad UDP packet, default `24`
- `--u8` — convert Linrad `s16` IQ to LeanDVB `u8`
- `--scale` — u8 conversion scale, formula `u8 = clip(offset + s16*scale/256)`
- `--offset` — u8 DC offset, default `128`
- `--s16-gain` — linear gain for `s16` output mode
- `--measure N` — measure first N bytes of input and output IQ and print stats fileciteturn1file8

### Notes

This path is the basis of the “general input” idea in the project. Even though multiple devices are supported directly, the overall workflow still treats the Linrad-fed path as a primary reference path.

The launcher includes several Linrad-based profiles, including DVB-S, DVB-S2, level measurement, and SNR measurement. fileciteturn1file0

## 2. AFEDRI / AFE822x

AFEDRI is supported in two ways:

- direct control/programming of the device
- direct UDP IQ reception into the local pipeline

### Drivers / helpers used

- `afedri-control.py`
- `afedri-control-fixed.py`
- `afedri-udp.py`

### What they do

`afedri-control.py` is the basic original control utility for setting frequency, sample rate, gain, and starting/stopping the stream. fileciteturn1file9

`afedri-control-fixed.py` is the more complete and recommended control utility. It adds:

- main clock query
- real-rate calculation
- single-channel mode selection
- AGC read/write control
- corrected AFEDRI RF gain coding
- detailed TX/RX logging of control responses fileciteturn1file6

`afedri-udp.py` receives the AFEDRI UDP IQ stream and writes raw `s16` IQ to stdout for use with LeanDVB or the SNR meter. fileciteturn1file10

### Relevant options

#### `afedri-control-fixed.py`

- `--ip` — AFEDRI control IP
- `--port` — AFEDRI control TCP port, default `50005`
- `--freq` — center frequency in Hz
- `--rate` — requested sample rate in Hz
- `--gain` — requested RF gain in dB
- `--channel` — DDC to use in single-channel mode (`0` or `2`)
- `--freq-channel-field` — channel field used in the frequency command
- `--gain-channel-field` — channel field used in the RF gain command
- `--agc keep|on|off` — AGC handling for the selected DDC
- `--stop-first` — stop stream before programming
- `--verbose` — detailed logging of control traffic fileciteturn1file6

#### `afedri-udp.py`

- `--bind` — bind address, default `0.0.0.0`
- `--port` — UDP port, default `50005`
- `--source-ip` — optional source IP filter
- `--stats-every` — periodic packet statistics to stderr fileciteturn1file10

### Notes

The launcher contains AFEDRI-related measurement and decoding profiles, including direct SNR measurement and DVB-S2 decoding at different sample rates. fileciteturn1file0

## 3. RTL-SDR

RTL-SDR is supported directly from the command line via the standard `rtl_sdr` utility.

### Driver / helper used

- `rtl_sdr`
- no separate Python converter is needed here

### What it does

RTL-SDR already produces unsigned 8-bit interleaved IQ, which matches LeanDVB’s `--u8` input mode directly. In the current launcher, the RTL-SDR profile sends the raw stream straight into `leandvb --u8`. fileciteturn1file0

### Relevant options seen in the launcher

- `-f` — center frequency
- `-s` — sample rate
- `-g` — tuner gain
- `-` — write raw IQ to stdout fileciteturn1file0

### Notes

Current launcher profile:

- profile 13 — RTL-SDR, DVB-S2, sample rate 2.4 MS/s, symbol rate 125 kS/s fileciteturn1file0

## 4. HackRF

HackRF is supported directly via `hackrf_transfer`, followed by a small converter because HackRF outputs **signed 8-bit IQ**, while LeanDVB expects **unsigned 8-bit IQ** when `--u8` is used.

### Driver / helper used

- `hackrf_transfer`
- `hackrf_s8_to_u8.py`

### What they do

`hackrf_transfer` captures the raw signed 8-bit IQ stream.

`hackrf_s8_to_u8.py` converts the HackRF stream from signed int8 IQ to unsigned uint8 IQ using the byte-wise equivalent of adding 128. Optional I/Q swap and throughput statistics are supported. fileciteturn1file3

### Relevant options

#### `hackrf_transfer`

- `-r -` — write raw IQ to stdout
- `-f` — tuning frequency
- `-s` — sample rate
- `-l` — LNA gain
- `-g` — VGA gain fileciteturn1file0

#### `hackrf_s8_to_u8.py`

- `--chunk-size` — bytes processed per read
- `--iq-swap` — swap I and Q in each IQ pair
- `--stats` — print throughput statistics to stderr fileciteturn1file3

### Notes

The launcher includes HackRF-based DVB-S2 profiles on 436 MHz, 437 MHz, and a high-symbol-rate test on 1291 MHz. One profile is explicitly marked as not working yet, which underlines the experimental status of the setup. fileciteturn1file0

## 5. RFSpace SDR-IQ

RFSpace SDR-IQ is supported via SoapySDR using a dedicated helper script.

### Driver / helper used

- SoapySDR with the `rfspace` driver
- `soapy_sdriq_to_stdout_v2.py`

### What it does

The helper opens the SDR-IQ via SoapySDR, tunes it, sets the sample rate, optionally applies ATT gain, and writes the stream to stdout as either:

- `s16` IQ
- `u8` IQ for LeanDVB `--u8` mode fileciteturn1file1

Supported sample rates are restricted to a fixed list and requested rates are rounded to the nearest supported value. ATT gain is quantized to the supported step values `-20`, `-10`, `0`, and `10` dB. fileciteturn1file1

### Relevant options

- `--device` — SDR-IQ device path or full Soapy device string
- `-f`, `--freq` — center frequency in Hz
- `-r`, `--rate` — requested sample rate
- `-g`, `--gain` — ATT gain in dB
- `--seconds` — run for a limited duration
- `--buf_len` — complex samples per read
- `--u8` — output unsigned 8-bit IQ
- `--s16` — output signed 16-bit IQ
- `--scale` — u8 conversion scale
- `--offset` — u8 conversion offset
- `--progress` — status output to stderr fileciteturn1file1

### Notes

The launcher currently contains SDR-IQ profiles for DVB-S2 on 28 MHz at 125 kS/s and 66 kS/s. fileciteturn1file0

## Measurement and analysis helpers

### `iq_band_snr_meter.py`

Use this when you want a quick estimate of whether the input signal is likely decodable. It reads `s16` IQ, performs FFT averaging, and estimates signal and noise power based on a central signal band and outer-band noise estimate. It also calculates an equivalent SNR normalized to a reference bandwidth. fileciteturn1file7

Example:

```bash
python3 afedri-udp.py \
  | python3 iq_band_snr_meter.py --fs 1010526 --signal-bw 170000 --exclude-bw 300000 --raw-power --avg-blocks 256 --ref-bw 2500
```

### `tsp_monitor.py`

Use this together with `tsp -P analyze` when you want service/provider visibility from the resulting MPEG transport stream. The script writes the latest analysis block to `tsp_latest.txt` and logs newly seen service/provider combinations to `service_provider.log`. fileciteturn1file5

### `dvbs2_modcod.py`

Use this helper when you want to determine LeanDVB `--modcods` masks from human-readable DVB-S2 parameters. It is especially useful for QPSK/8PSK/16APSK/32APSK experiments and for building combined masks for FEC ranges. fileciteturn1file2

## Current profile map

The launcher currently defines these profiles: fileciteturn1file0

1. DVB-S 125k FEC 1/2 from Linrad, u8
2. DVB-S2 125k QPSK 1/2 from Linrad, u8
3. DVB-S 125k FEC 1/2 from Linrad, s16
4. DVB-S2 125k QPSK 1/2 from Linrad, s16
5. DVB-S2 125k multiple modcods from Linrad, s16
6. DVB-S2 66k QPSK 2/3 from Linrad, s16
7. Measure Linrad s16 levels
8. Measure Linrad SNR
9. Measure AFEDRI SNR
10. AFEDRI direct DVB-S2 decode
11. AFEDRI direct DVB-S2 decode at lower sample rate
12. AFEDRI direct DVB-S2 decode plus TS analysis
13. RTL-SDR DVB-S2 125k
14. HackRF DVB-S2 125k on 436 MHz
15. HackRF DVB-S2 333k on 437 MHz
16. HackRF DVB-S2 8PSK SR 5M test on 1291 MHz, marked as not working
17. SDR-IQ DVB-S2 125k FEC 1/2
18. SDR-IQ DVB-S2 66k FEC 2/3

## Typical examples

### Show available profiles

```bash
./rx_dvb_linrad_leandvb.sh --info
```

### Run a Linrad DVB-S2 profile

```bash
./rx_dvb_linrad_leandvb.sh --profile 4
```

### Measure Linrad input levels

```bash
./rx_dvb_linrad_leandvb.sh --profile 7
```

### Measure AFEDRI SNR

```bash
./rx_dvb_linrad_leandvb.sh --profile 9
```

### Run direct RTL-SDR receive

```bash
./rx_dvb_linrad_leandvb.sh --profile 13
```

### Run direct HackRF receive

```bash
./rx_dvb_linrad_leandvb.sh --profile 14
```

### Run direct SDR-IQ receive

```bash
./rx_dvb_linrad_leandvb.sh --profile 17
```

## Requirements

Typical dependencies are:

- Python 3
- `numpy`
- LeanDVB / LeanSDR build with `leandvb`
- VLC / `cvlc`
- TSDuck (`tsp`) for TS analysis profiles
- Linrad for the Linrad-based path
- device-specific command-line tools where applicable:
  - `rtl_sdr`
  - `hackrf_transfer`
  - SoapySDR with RFSpace support for SDR-IQ

Install NumPy if needed:

```bash
python3 -m pip install numpy
```

## Notes and caveats

- This repository is experimental and intentionally shell-oriented.
- The profiles are tuned for a specific local setup and should not be seen as universal defaults.
- Sample rates, symbol rates, `--modcods`, and `--ldpc-bf` settings are practical experiment values.
- The path to `leandvb` in the launcher is currently hardcoded and may need local adjustment. fileciteturn1file0
- VLC caching settings are intentionally tuned for live receive experimentation.
- Device setup may depend on prior configuration outside this repository.

## License

This repository is licensed under the MIT License. See the `LICENSE` file for the full text.

Copyright (c) 2026 Rob Hardenberg / PE1ITR

This applies to the helper scripts and repository content in this project.

External / upstream software used by this project, including LeanDVB / LeanSDR, VLC, Linrad, TSDuck, SoapySDR, rtl-sdr, and HackRF tools, remains subject to its own license terms.

Many thanks to the authors and maintainers of these excellent tools, and especially to the LeanDVB / LeanSDR and Linrad authors, for making this kind of experimentation possible.


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

The frequency readout in the top bar shows **RF frequency = SDR frequency + LO offset**.
Set **LO-offset (kHz)** in the **SDR** menu for the converter: 28 MHz IF plus 408000 kHz offset
shows 436.000000 MHz. The **SDR / IF (kHz)** field is also in the **SDR** menu.
Below these settings, a live calculation shows **SDR/IF + LO-offset = RF** and
the difference **RF − IF** in MHz and kHz to help check the offset. The top bar
shows the received RF frequency and gain alongside Start/Stop.
The offset changes the readout and spectrum/waterfall axes; the hardware always receives the IF frequency.
Signed offsets are supported; this is an additive offset, not a high-side mixer
inversion mode.

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
and the same clipped, rounded CF32-to-s16 conversion as the Python example.
No Python process is used. Optional `u8` uses the common C helper's full-scale
s16-to-u8 conversion; the old Python `--scale` boost is not applied.
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
and gain for the actual signal. The default DVB-S2 arguments accept normal frames;
change `--framesizes 1` in `[leandvb]` if the transmitter uses short frames.

HackRF defaults to `u8`: native signed 8-bit samples are offset to unsigned IQ
without increasing their size. B210 defaults to `s16` to preserve its resolution;
`u8` is also selectable but discards precision. At 10 MS/s these formats carry
20 MB/s and 40 MB/s respectively. LeanDVB's normal decoder converts both formats
to float internally, so the smaller pipe traffic is not a guarantee of faster
DVB-S2/LDPC decoding. B210 USB transport remains `sc16` with either output format.

The B210 input follows `atv-receiver-analog`: UHD, channel 0, internal clock, and
Kintex-7 images from `[b210] images` (default
`/home/rhardenb/repos/ettus/uhd-images`). This directory must contain the matching
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


DVB-S2 startup defaults omit `--fastlock`. Without a valid signal, aggressive
frame searches can be much slower than decoding an already acquired clean signal.
The messages `Unsupported or corrupted FEC` / `MODCOD` can arise from false
physical-layer-header detections in noise, including operation without an antenna.
They do not by themselves establish that the transmitter uses the wrong FEC.
Overflow recovery is not a guarantee of error-free reception: check that the
`dropped` counter stops increasing once a valid signal has been acquired.


`make test-iq-conversion` compares the actual C conversion routines used by the
SDR helper with `hackrf_s8_to_u8.py` from the original HackRF pipeline. It checks
all 256 signed-byte representations, all 65,536 signed 16-bit values, block
boundaries, zero/quantization edges and asymmetric I/Q pairs. HackRF CS8 becomes
u8 by XOR 0x80. UHD host `sc16` is passed unchanged to `--s16`; conversion to u8
maps -32768 to 0, 0 to 128 and 32767 to 255, retaining I/Q order. These are format
tests; they do not validate RF levels, physical I/Q orientation or the attached
B210 firmware with a known signal.

The legacy `rx_dvb_linrad_leandvb.sh` profiles are not identical receiver setups:
14/15 use a +500 kHz hardware LO offset and decoder `--tune -500000`, HackRF
LNA 16 dB / VGA 40 dB, and 50 LDPC bitflips. Profile 16 uses 5 MSym/s, no offset,
8PSK 3/4 and zero LDPC bitflips, and is labelled `WERKT NIET` in the script.
The GUI currently uses center tuning and the Soapy aggregate gain control; its
single gain value does not reproduce that LNA/VGA pair automatically.

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
**Renderer** kiest zoals gebruikelijk een speler of rechtstreekse MPEG-TS-uitvoer.

Vanaf de opdrachtregel kan dit ook:

```bash
./build/linrad_dvb --iq-file /pad/naar/opname.iq
./build/linrad_dvb --iq-file /pad/naar/opname.iq --print-pipeline
./build/linrad_dvb --iq-file /pad/naar/opname.iq --run
```

### Renderer en rechtstreeks MPEG-TS opslaan

Via **Renderer** kies je **Embedded player (right panel)**, **UDP stream (external player)**,
VLC, VLC met bediening, **FFplay**, een eigen programma of **MPEG-TS-bestand**.
De rendererkeuze staat in dit menu; Scatterfix bedien je met
de knop naast **BW** in het spectrum.
Stop de ontvangst voordat je de uitvoer wijzigt. **W** bewaart de keuze in de INI.

De ingebouwde player toont beeld in het vaste rechterkader en speelt geluid af.
Met **U** (of **u**) in het hoofdvenster schakel je mute aan of uit. Het videokader
toont **Muted (U)** zolang het geluid gedempt is. De keuze blijft tijdens deze
app-sessie behouden, ook na Stop/Start. UDP-uitvoer en TS-opnames behouden hun audio;
externe players bedienen hun eigen geluid.
Vink **Also send video/audio via UDP** aan om tegelijk een externe player te voeden.
Met **UDP stream (external player)** wordt alleen UDP verstuurd, zonder lokaal beeld
of geluid. Het rechterkader behoudt ook dan zijn afmetingen. Stel het IPv4-adres
van de externe player en de doelpoort in; standaard is dit `127.0.0.1:1234`.
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
`[session] renderer` is `embedded`, `udp`, `cvlc`, `vlc`, `ffplay`, `custom` of `mpegts`.
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
