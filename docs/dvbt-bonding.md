# Experimentele DVB-T channel bonding

Het samengevoegde [verslag van de drie ontvangstproeven op 15 november 2026](dvbt-bonding-drie-proeven-20261115.md)
bevat de resultaten, het oordeel en een tekstbasis voor een later websiteartikel.

De ontvanger gebruikt de v1-merger uit `dvb-channel-bonding`, vastgelegd in
`components/bonding/ORIGIN`. Twee zelfstandige DVB-T-decoders gebruiken dezelfde
IQ-invoer en dezelfde digitale afstemming/het kanaalfilter als de headless
multirx-repeater. Elke decoder levert zijn volledige kanaal-TS na RS-correctie
aan de merger. De gereconstrueerde of geremuxte TS gaat bij ingeschakelde bonding
naar de bestaande TSscatterFix-, programma-, opname- en spelerverwerking.
Het protocol is experimenteel en geen algemene DVB-bondingstandaard.

De component is bijgewerkt naar **1.6.0**, bronrevisie `89100be`.
De code is van Rob Hardenberg / PE1ITR en valt onder
[GPL-3.0-only](../components/bonding/LICENSE). De licentiecommit verandert de
mergerwerking niet; de [herkomst en bestandshashes](../components/bonding/ORIGIN)
blijven vastgelegd. De zender
kiest uit twee mechanismen die de ontvanger automatisch uit de PMT herkent:

- **Gedeeld (distribution 0)**: de bestaande private TS-overdracht met XOR8,
  XOR16 of hybrid-bescherming. De merger reconstrueert de bronpakketten.
- **Gescheiden kanalen (distribution 2)**: zelfstandig decodeerbare video op A
  en audio op B. Deze modus heeft geen XOR, interleaving of private media-PES.
  De merger maakt een gecombineerde TS met behoud van ES/PES/PTS, opnieuw
  opgebouwde PAT/PMT, nieuwe continuity counters en eigen null-padding.
  De oorspronkelijke pakketvolgorde en null-posities worden niet hersteld.

De eerdere gecodeerde video/audio-modus **distribution 1** is door de component
verwijderd en wordt geweigerd. Ook DBNC en de compacte PCR-records uit 1.3 zijn
verwijderd. Voor een bestaande uitzending in die modus moet de zender worden
bijgewerkt en met een nieuwe sessie in modus 0 of 2 starten. Er is geen nieuwe
ontvangerinstelling nodig.

In de gescheiden modus kan video doorgaan zonder audio; ontbrekende audio
wordt niet gerepareerd. Kanaal B is op zichzelf een audio-only programma.
Voor gecombineerde mergeruitvoer blijft het programma van A nodig: er is geen
gecombineerde audio-only fallback als A ontbreekt. Beide kanalen moeten voor
normale combinatie dezelfde programmatijdlijn gebruiken.

Versie 1.6.0 vereenvoudigt het generatorgerichte JSON-contract (schema v1).
Het beschrijft de bron-MPEG-TS met de modusnaam `DVB-T bonding shared` of
`DVB-T bonding lanes`, CBR-bitrate, klok- en timingvereisten en eventuele
media-TS-budgetten. Interne beschermingsinstellingen, discoverygegevens en
vrijgavevlaggen maken geen deel meer uit van dit externe contract.
De bronvereisten omvatten maximaal 100 ms tussen PCR's en maximaal 500 ms
tussen PAT- respectievelijk PMT-secties. Deze contractwijziging verandert de
ontvangerinstellingen of afspeelvertraging niet. Het interne mergerstatusobject
blijft de bestaande numerieke `distribution` en `profile` gebruiken.

De genoemde bitrates zijn TS-budgetten, geen encoderadvies. De oude
22k/16k-preset en het compacte PCR-bronprofiel zijn geen actuele presets.
De component claimt geen volledig ETSI/DVB-serviceprofiel of gegarandeerde
PCR-timing op alle uitgangen. Bij een nieuwe sessie/configuratie worden
mergerbuffers herstart; recent verlaten aankondigingen worden afgewezen.

## Bediening

- Kies **DVB-T**, **BW 40000**, **QPSK**, **FEC 1/2**, **GI 1/32**.
- Stem zoals gewoonlijk af op één kanaal in het midden. Bij een geldige
  bondingaankondiging licht de indicator naast **Teletext** op en schakelen de
  tweede decoder en merger **automatisch** in. Daarvoor hoeft geen optie aan.
- Dit geldt voor live ontvangst en IQ-bestanden: de eerste decoder begint op
  **0 Hz**. De aankondiging bepaalt de partnerafstand, normaal **+50 of −50 kHz**.
  De geadverteerde absolute zendfrequentie wordt niet als IQ-offset gebruikt.
- Een gewone enkelkanaals-TS loopt direct door zolang geen bonding is gevonden.
- **Mode → Disable automatic DVB-T bonding** houdt automatisch bijschakelen
  tegen. Detectie blijft werken; de indicator kan nog worden aangeklikt om
  handmatig te bonden. Tot die activering blijft de volledige kanaal-TS
  doorlopen, ook nadat bonding is gedetecteerd. De keuze geldt bij de volgende
  ontvangststart.
- De bestaande `DVB-T bonding`-JSON-metadata van datv-transmitter wordt herkend
  voor bandbreedte, samplefrequentie, FEC en GI. Metadata verandert de expliciete
  uitschakelkeuze niet en kiest geen verschoven eerste kanaal.
- Beide kanalen moeten binnen dezelfde SDR-band passen. De ontvanger verandert
  de hardwarefrequentie niet om de partner te zoeken. De tooltip toont de
  partneroffset binnen de aangeboden IQ-invoer. `Paired` betekent dat ook de
  wederkerige aankondiging van de partner is gecontroleerd.
- Stoppen schakelt beide decoders uit. De knop is geen aan/uit-schakelaar tijdens
  het mergen; start opnieuw om naar enkelkanaalsontvangst terug te gaan.

De IQ-samplefrequentie moet de twee kanalen bevatten: de opname met 250 kS/s
past. Het algemene IQ-filter moet beide kanalen doorlaten. Bij GUI-ontvangst
wordt een ingeschakeld smal IQ-filter via de bestaande bronbesturing verbreed
zodra de tweede decoder start. Bij headless gebruik moet het vooraf breed
genoeg staan of uitgeschakeld zijn. De twee decoderfilters selecteren ieder hun eigen 40-kHz-kanaal.
De bestaande instelling voor hardware receiver offset blijft vóór de gedeelde
IQ-invoer werken; de getoonde bondingoffsets zijn relatief aan die invoer.

De library gebruikt standaard **8 seconden afspeelbuffer**. In gedeelde modus
begint die na het eerste geldige bondingframe; in gescheiden modus blijft media
minstens de ingestelde vertraging na aankomst gebufferd. Acquisitie en wachten
op een videokeyframe komen daar nog bij. Gedeelde ontvangst kan op een geldige
herstelgroep instappen. Dit garandeert geen verliesvrije acquisitie of exact
herstel van een afgebroken opname.

## Configuratie en diagnose

De optionele INI-instelling is:

```ini
[dvbt]
disable_bonding=false
```

Deze instelling geldt alleen voor DVB-T met BW 40000. Zonder instelling staat
automatisch bijschakelen aan. Zet `disable_bonding=true` om alleen te detecteren.
De eerdere opt-in-sleutel `bonding` wordt niet meer gebruikt. Opslaan gebruikt de bestaande W-functie; de
implementatie verandert het actieve INI-bestand niet tijdens een build.
Voor headless gebruik kan dezelfde instelling worden gebruikt. De bestaande
headless repeaterconfiguratie en zijn losse UDP-uitgangen blijven beschikbaar.

Het HTTP-statusobject `bonding` bevat detectie, activering, partnerbevestiging,
de relatieve partneroffset en de laatste mergerstatus. De mergerstatus bevat
ook `distribution`: 0 voor gedeeld, 2 voor gescheiden video/audio. In modus 2
is `profile` altijd 0 (geen bescherming). Het log bevat
`[bond]`, `[bond-status]` en per decoder `iq_dropped` bij normale afronding.
IQ-queues zijn onafhankelijk en begrensd tot ongeveer 500 ms per decoder;
een volle queue houdt de andere decoder niet tegen. De gedeelde TS-queue is
ook begrensd. De eerste decoder blijft de bestaande constellatie/decoderstatus
leveren; die weergave is geen gezamenlijke kwaliteitsmeting van beide kanalen.

In gedeelde modus bepaalt een geldige EOS uit de zender de exacte eindlengte.
Een IQ-opname heeft vaak geen EOS. Dan stopt de adapter na het herstelbudget
plus twee seconden marge; vervangende nulls aan het einde bepalen geen
vastgestelde bronlengte. De gescheiden modus heeft geen private EOS: pas als
beide decoderuitgangen gesloten zijn, meldt de adapter `dvb_bond_rx_finish`
en speelt hij de resterende media uit tot END. Tijdelijk wegvallen van één
RF-kanaal is geen einde van de invoer.
Handmatig bijschakelen begint een nieuwe mergerbuffer zodat lang wachten op
de knop geen verouderde afspeelpositie behoudt.

## Validatie

`make test-dvbt-bonding` controleert de echte coordinator met twee gecontroleerde
decoderuitgangen: byte-identiek bronherstel, een gecentreerd eerste kanaal en partners op +50/−50 kHz,
partnerbevestiging, gewone TS-doorvoer en een ongeldige IQ-bandbreedte.
De gescheiden video/audio-modus wordt vanaf beide kanalen door de echte adapter
samengevoegd: alle ES/PES-payloadbytes worden per PID vergeleken en beide
gesloten decoderuitgangen moeten volledig worden afgehandeld.
`make test-bonding-component` draait de vijf meegeleverde C-tests voor
herstel, discovery, TS-transport, native video/audio en de bevroren v1-client-ABI;
deze tests maken ook deel uit van `make test-dvbt-bonding`.
De post-RS-test controleert dat private PES behouden blijft en TS met TEI of
ongeldige sync ook in de ruwe bondinguitvoer wordt weggegooid.
`build/gui_smoke --test-bonding` controleert metadata, knopdetectie en activering
onder een GTK-display.

Een eerste realtime replay van de bestaande 40-s-opname `9Eycvd.cf32` leverde
bevestigde ontvangst op beide offsets, nul adapter-IQ-queuedrops en 137
via FFmpeg gedecodeerde 160×120 H.264-beelden. De merger rapporteerde één
hersteld symbool en vier ontbrekende pakketten nabij het afgebroken einde.
De videodecoder meldde bij instappen ontbrekende PPS/keyframecontext. Dit is
een werkende integratieproef, geen claim van verliesvrij herstel van de hele
opname. Log, TS en decodecontrole staan in `build/bonding-validation/`.

Het standaard afspeelbudget blijft 8 seconden, zoals ingesteld bij de vorige
componentupdate van 16 naar 8 seconden. De knop naast Teletext
heeft een vaste breedte met bovenaan Bonding en daaronder de actuele status.

Oude IQ-opnamen met beide signalen op −25/+25 kHz passen niet bij de nieuwe
standaardafstemming. Maak voor de nieuwe proef een opname met het gekozen
hoofdkanaal op 0 Hz en het tweede kanaal erboven of eronder. Een eerder gemeten
uitkomst met de oude opname valideert deze nieuwe afstemming niet.

## Audiovergelijking KMvKDN — 15 november 2026

De complete opname
`dvbt-bonding-qpsk-bw40000-gi1-32-fec1-2-fs250000-60s-KMvKDN.cf32`
is 60 seconden lang bij 250 kS/s. Dit is native gescheiden video/audio
(distribution 2, geen XOR), met het hoofdkanaal op 0 Hz en audio op +50 kHz.
De bijbehorende `KMvKDN_rx.iq` is de ontvangen versie uit het propagatiemodel.
Dit zijn bestaande bestanden; er is geen nieuwe RF-meting gedaan.

| Controle | Schone `.cf32` | Ontvangen `_rx.iq` |
| --- | ---: | ---: |
| Decodeerbare audio na samenvoegen | 55,168 s | 14,763 s |
| Aaneengesloten stukken volgens audiotimestamps | 1 | 20 |
| Langste stuk | 55,168 s | 6,656 s |

Audio is AAC-LATM, mono op 24 kHz. Het losse audiokanaal vóór de merger
levert 55,680 seconden aaneengesloten audio. Bij het instappen in de merger
ontbreekt een beginstuk van 493 payloadbytes, overeenkomend met 0,512 seconde
gedecodeerde audio. De overige 90.152 gecodeerde audiopayloadbytes zijn exact
gelijk aan het overeenkomstige einde van het losse audiokanaal. De volledige
captures zijn dus niet byte-identiek. Bij uitgelijnde PCM verschillen alleen
samples binnen het eerste AAC-frame van 1.024 samples; daarna is de PCM gelijk.
In de schone proef melden de healthregels geen onherstelbare RS-blokken en
beide adapter-IQ-queues nul drops.

In de ontvangen opname is wel hoorbaar signaal aanwezig. De 14,763 seconden
audio liggen verspreid over 27,435 seconden aan audiotimestamps; slechts drie
stukken duren minstens één seconde. Er zijn veel onherstelbare RS-blokken op
beide kanalen. De video levert 71 decodeerbare frames over 40,25 seconden,
met decodeerfouten. Tijdens de geobserveerde afspeelbeurt koppelt het tweede
kanaal circa 19 seconden na de start. De eerdere constatering van volledig
stille audio betrof een andere opname, `FvMRrF`, en geldt niet voor `KMvKDN`.

Conclusie: de schone bron bevat lange, doorlopende audio en na acquisitie is
geen terugkerend audiopayloadverlies door de merger gevonden. Het verschil
met de ontvangen versie ondersteunt ontvangstverlies als verklaring voor de
korte audiostukken. Dit controleert niet iedere foutafhandeling bij beschadigde
invoer en is geen bewijs van verliesvrije acquisitie.

Het bewaarde meetrapport met hashes staat in
`benchmarks/dvbt-bonding-audio-2026-11-15.json`. Volledige logs, TS, PCM en
analysescripts staan in `build/bonding-clean-source-check/` en
`build/bonding-live-audio-check/`. Deze vergelijking veranderde geen actieve
INI, bronopname of afspeelinstelling.

### Vervolg met 3 dB meer zendvermogen

De gebruiker herhaalde de proef met opgegeven +3 dB zendvermogen; de nieuwe
metadata vermeldt 250 W. `KMvKDN_rx.iq` is onder dezelfde naam vervangen.
De vorige TS en analyses zijn behouden; de nieuwe metadata en IQ-hash staan
apart in `build/bonding-plus3db-check/`.

| Resultaat | Vorige proef | +3 dB-proef |
| --- | ---: | ---: |
| Decodeerbare audio | 14,763 s | 24,704 s |
| Audiostukken van minstens 1 s | 3 | 8 |
| Langste audiostuk | 6,656 s | 7,680 s |
| Decodeerbare videoframes, inclusief beschadigde frames | 71 | 141 |
| Start partnerdecoder na replaystart | circa 19 s | circa 5 s |

Een deel van de extra audio komt door eerder koppelen. Binnen hetzelfde
bron-timestampinterval als de vorige audiovergelijking groeit de decodeerbare
audio van 14,763 naar 17,835 seconden. Ook nu blijven 20 audiostukken over;
de opname is niet foutloos. Na een instapprefix van 284 gecodeerde bytes is
de resterende audiopayload vóór en na de merger exact gelijk: tijdens deze
proef verdwijnt daarna geen ontvangen audiopayload door het samenvoegen.
Beide adapter-IQ-queues melden nul drops.

Het is een opeenvolgende vergelijking; `noise_seed` is niet vastgezet in de
nieuwe metadata. Dit is dus geen gecontroleerde power-only vergelijking met
identieke ruis. Meetrapport: `benchmarks/dvbt-bonding-plus3db-2026-11-15.json`.

Deze +3 dB-proef is de vastgelegde **gescheiden-kanalenreferentie** voor de
volgende proef met volgens de gebruiker dezelfde inhoud in **shared mode**.
Het gemeten IQ-bestand is op hash gecontroleerd en apart bewaard als
`build/bonding-plus3db-check/reference-input-rx.iq`. De volgende meting krijgt
een eigen rapport; modus, bescherming, bitrates en zendvermogen worden opnieuw
uit metadata/status gecontroleerd. De vergelijking gebruikt dezelfde audio-,
video-, acquisitie- en verliesmaten en vermeldt bij shared ook herstelcounters.

### Eerste shared-proef: 125 W

De gebruiker bevestigt dat `n5QlHY_rx.iq` bewust op 125 W is gegenereerd.
Metadata en endpoint bevestigen distribution 0, XOR16, één interleavegroep,
50.100 bit/s bron en 30.160 bit/s per lane. De replay levert 11,477 seconden
decodeerbare AAC-LATM-audio in 12 stukken; het langste duurt 7,467 seconden
en alleen dat stuk duurt minstens één seconde. Er zijn 40 decodeerbare
videoframes, inclusief beschadigde/geconcealde frames. De merger eindigt op
970 ontbrekende/vervangende bronpakketten en nul herstelde symbolen; dit zijn
andere tellers dan RS-fouten. Beide adapter-IQ-queues melden nul drops.

Rapport: `benchmarks/dvbt-bonding-shared-125w-2026-11-15.json`. Volledige
artefacten en het op hash gecontroleerde IQ-bestand staan apart in
`build/bonding-shared-comparison/`. De gebruiker bereidt hierna een shared-proef
op 250 W voor. Deze 125 W-proef wordt niet overschreven of als 250 W behandeld.
De gegenereerde ruis is niet vastgezet en bron-PTS verschillen tussen modi;
de vergelijking is geen gecontroleerde modus-only A/B.

### Shared op 250 W, opnieuw gegenereerd en gecontroleerd

Na twee herhalingen van het oude 125 W-bestand is `n5QlHY_rx.iq` opnieuw
gegenereerd. Metadata bevestigt 250 W en de hash verschilt van de opgeslagen
125 W-versie. De afspeelbeurt van 21:00:40 is apart gemeten; beide bronbestanden
zijn op hash bewaard. Modus en bescherming blijven shared/XOR16 met één
interleavegroep en 50.100/30.160 bit/s bron/lane.

| Resultaat | Shared 125 W | Shared 250 W | Gescheiden 250 W |
| --- | ---: | ---: | ---: |
| Decodeerbare audio | 11,477 s | 20,011 s | 24,704 s |
| Aantal audiostukken | 12 | 23 | 20 |
| Stukken van minstens 1 s | 1 | 5 | 8 |
| Langste audiostuk | 7,467 s | 7,979 s | 7,680 s |
| Decodeerbare videoframes, inclusief beschadigde/geconcealde | 40 | 77 | 141 |

De shared-250 W-replay eindigt met 957 ontbrekende/vervangende bronpakketten,
nul herstelde symbolen en nul adapter-IQ-queuedrops op beide kanalen. Shared
verbetert hier met meer vermogen; de eerdere gescheiden-250 W-proef levert
meer totale audio en videoframes, terwijl shared een iets langer maximaal
audiostuk heeft. Nul herstelde symbolen bewijst geen defecte XOR-decoder.

Tussen de opgeslagen shared-propagatieparameters verandert alleen
`tx_power_w` van 125 naar 250. De ruisrealisatie is echter niet vastgezet
(`noise_seed=null`); over modi heen zijn ook de bron-PTS verschillend.
Dit blijft een vergelijking van opeenvolgende opnamen. Meetrapport:
`benchmarks/dvbt-bonding-shared-250w-2026-11-15.json`; volledige artefacten:
`build/bonding-shared-250w-new-check/`. Ontvangerinstellingen zijn niet gewijzigd.
