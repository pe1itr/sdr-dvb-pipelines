# DVB-T bonding: vergelijking van drie proeven op 15 november 2026

## Oordeel

In deze drie opnamen levert **gescheiden video/audio op 250 W de meeste
decodeerbare audio en videoframes**. Shared met XOR16 levert op 250 W meer
audio en beeld dan shared op 125 W. Het langste afzonderlijke audiostuk is
bij shared op 250 W iets langer dan bij de gescheiden kanalen, maar de totale
audio-opbrengst is lager. Geen van de proeven levert ononderbroken ontvangst.

Dit is bruikbare praktijkinformatie voor een artikel, geen algemene rangorde
van bondingmethoden. **De gegenereerde ruis was niet identiek.** Het gaat om
opeenvolgende simulatieopnamen, met één opname per hier vergeleken instelling.
Er kan daarom geen exact causaal voordeel aan uitsluitend vermogen of modus
worden toegeschreven.

## Opzet

De gebruiker gaf aan dezelfde inhoud aan te bieden. De opnamen zijn elk
ongeveer 60 seconden lang. De ontvangen IQ-bestanden zijn gegenereerd met
een propagatiemodel; dit zijn geen drie nieuwe metingen via een echte RF-link.

Gemeenschappelijke instellingen:

- Twee DVB-T-kanalen: QPSK, 2K, FEC 1/2, GI 1/32, nominale bandbreedte
  40 kHz per kanaal en 50 kHz kanaalafstand.
- IQ-samplefrequentie 250 kS/s; eerste kanaal op 0 Hz, partner op +50 kHz.
- Bron-TS 50.100 bit/s; kanaal-TS 30.160 bit/s per kanaal.
- Automatische bondingdetectie en de bestaande afspeelbuffer van 8 seconden.
- Audio: AAC-LATM, mono op 24 kHz.

Bij **gescheiden kanalen** staat native video op A en native audio op B,
zonder XOR-bescherming. De ontvanger maakt daarvan een gecombineerde TS.
Bij **shared** wordt de bron via het private transport over beide kanalen
verdeeld; deze proeven gebruiken XOR16 met één interleavegroep.

De actuele bestandsnaam, sessie en modus zijn gecontroleerd via de lokale
ontvangerstatus. Vermogen en modelinstellingen zijn uit de metadata gelezen.
De drie gemeten IQ-versies zijn met SHA256 en een afzonderlijke kopie bewaard;
dezelfde bestandsnaam is tussentijds opnieuw gebruikt. Herhalingen waarbij
nog het oude 125 W-bestand afspeelde zijn niet als 250 W-proef meegeteld.

Na de geobserveerde afspeelbeurt is ieder bestand afzonderlijk in realtime
naar een TS-bestand gedecodeerd voor de media-analyse. Dit is een aanvullende
replay, geen opname van de luidsprekeruitvoer van de gebruiker. Ontvanger- en
afspeelinstellingen zijn daarbij niet veranderd.

## Resultaten

| Meetwaarde | Gescheiden 250 W | Shared XOR16 125 W | Shared XOR16 250 W |
| --- | ---: | ---: | ---: |
| Totaal decodeerbare audio | **24,704 s** | 11,477 s | 20,011 s |
| Aantal audiostukken | 20 | 12 | 23 |
| Audiostukken van minstens 1 s | **8** | 1 | 5 |
| Langste audiostuk | 7,680 s | 7,467 s | **7,979 s** |
| Decodeerbare videoframes, inclusief beschadigde/geconcealde | **141** | 40 | 77 |
| Door shared gerapporteerde herstelde symbolen | niet van toepassing | 0 | 0 |
| Door shared gerapporteerde ontbrekende/vervangende bronpakketten | niet van toepassing | 970 | 957 |
| Adapter-IQ-queuedrops, kanaal A/B | 0/0 | 0/0 | 0/0 |

Decodeerbare audiotijd is de som van de door FFmpeg gedecodeerde audiosamples
gedeeld door de samplefrequentie. Audiostukken zijn bepaald met de bron-PTS
en de duur van ieder gedecodeerd audioframe; een tijdgat groter dan 1 ms
begint een nieuw stuk. Dit meet beschikbaarheid van decodeerbare audio,
niet spraakverstaanbaarheid, luisterkwaliteit of het precieze gedrag van VLC.

Het aantal stukken is op zichzelf geen kwaliteitsscore: meer ontvangen audio
kan ook meer losse stukken opleveren. Totale duur, langste stuk en het aantal
stukken van minstens één seconde moeten samen worden gelezen.

Videoframes zijn geteld tijdens decoderen. Er zijn ook beschadigde en door
de codec aangevulde frames. Het frameaantal is dus geen aantal foutloze
beelden en geen maat voor vloeiend afspelen. Een zichtbaar beeld kan bovendien
blijven staan terwijl er geen nieuwe bruikbare frames worden ontvangen.

## Betekenis van de uitkomst

Binnen shared groeit de decodeerbare audio van 11,477 naar 20,011 seconden
bij de vermogensverhoging van 125 naar 250 W, ongeveer +3 dB. Het aantal
stukken van minstens een seconde groeit van één naar vijf. Dat is een
duidelijke verbetering van de gemeten bruikbaarheid in deze opnamen.

Op 250 W levert gescheiden video/audio ongeveer 4,7 seconden meer audio dan
shared, en meer decodeerbare videoframes. Shared heeft wel een iets langer
maximaal audiostuk: 7,979 tegenover 7,680 seconden. De aanwezige
XOR16-bescherming vertaalt zich in deze shared-proeven niet naar gemelde
herstelde symbolen. Dat betekent dat hier geen extra herstel is aangetoond;
het bewijst niet dat XOR defect is of onder andere verliespatronen geen nut
heeft.

De ontvangen opnamen bevatten veel onherstelbare RS-blokken. De nulwaarden
voor adapter-IQ-queuedrops wijzen erop dat de geobserveerde uitval niet
samenviel met verlies in die queues. Dit sluit niet iedere andere foutbron
in modulator, model, demodulatie of ontvanger uit.

Voor de gescheiden-250 W-proef is de audio ook vóór en na de merger
vergeleken. Na een instapprefix van 284 gecodeerde bytes is de resterende
audiopayload exact gelijk. Een afzonderlijke controle met de schone bron
levert eveneens doorlopende audio en behoud van de payload na acquisitie.
Daarmee is voor die controles geen terugkerende audio-uitval door het mergen
gevonden. Dit is geen uitputtende controle van alle shared-herstelpaden.

## Grenzen van de vergelijking

- `noise_seed` was niet vastgezet; de gebruiker bevestigt dat de gegenereerde
  ruis niet identiek was. De shared-modelparameters verschillen wel uitsluitend
  in `tx_power_w`, maar de feitelijke ruisrealisatie kan ook de uitkomst beïnvloeden.
- Dezelfde inhoud is door de gebruiker opgegeven; de bronstreams over modi
  heen zijn niet als identieke byte- of codecstream geverifieerd. Hun PTS
  verschillen, zodat absolute PTS niet rechtstreeks tussen modi zijn uitgelijnd.
- Acquisitie en later starten van de partnerdecoder beïnvloeden de totale
  opbrengst. Er zijn geen herhaalde runs met meerdere vaste ruiszaden gedaan.
- RS-fouten, TS-continuïteitsfouten, vervangende bronpakketten en herstelde
  symbolen zijn verschillende tellers. De healthvensters van beide decoders
  overlappen in tijd en hebben niet overal dezelfde dekking tussen proeven.
- Dit is een experimenteel bondingprotocol, geen vrijgave van volledige
  ETSI/DVB-conformiteit of een foutloos afspeelprofiel.

Voor een strakkere vervolgvergelijking kunnen dezelfde broninhoud en
propagatie-instellingen worden gebruikt met een vastgelegd ruiszaad en
meerdere herhalingen per modus en vermogen.

## Vervolgvraag: bescherming bij langere fading

De gebruiker wil mogelijk ook de andere bescherming voor langere fading
proberen. De nulwaarde bij XOR16 betekent **geen succesvol gemeld herstel**,
niet dat herstel overbodig was. XOR16 kan één ontbrekend bronsymbool in een
groep herstellen als de pariteit en alle overige bronsymbolen op tijd
beschikbaar zijn. Bij meerdere ontbrekende symbolen, ontbrekende pariteit
of te late ontvangst kan herstel uitblijven. De huidige metingen bepalen niet
welke van die oorzaken per groep optrad.

Een zinvolle volgende proef is **hybrid16 met tijdspreiding**. Hybrid16 voegt
naast XOR een extra kopie toe van het bronsymbool dat de PES/NAL-inspectie het
belangrijkst acht. Tijdspreiding verdeelt de verzending van groepen over een
langere periode; dat kan helpen tegen uitval in aaneengesloten stukken, maar
vraagt mogelijk meer afspeelbuffer en geeft geen garantie bij langdurige
uitval van beide kanalen.

Deze proef vereist een passend bronbudget: bij de huidige standaard
kanaalbitrate en aankondigingstijd is het transportplafond voor hybrid16
**47.541 bit/s**, met **46.000 bit/s** als meegeleverde transporttestwaarde.
De huidige 50.100-bit/s-bron past daar niet ongewijzigd in. Het actuele
zendercontract en de benodigde buffer moeten vóór de proef worden gecontroleerd.
Houd bij voorkeur vermogen en ruiszaad vast. Als de bronbitrate verandert,
moet die wijziging ook in de vergelijking worden vermeld.

Dit is een voorgestelde vervolgproef; hybrid16 is in deze meetreeks nog niet
gemeten en er zijn hiervoor geen zender- of ontvangerinstellingen gewijzigd.

## Tekstbasis voor een later websiteartikel

> In drie proeven met gesimuleerde ontvangst vergeleken we gescheiden
> video/audio met shared bonding met XOR16. Op 250 W leverden de gescheiden
> kanalen de meeste decodeerbare audio en videoframes. Bij shared bracht een
> verhoging van 125 naar 250 W duidelijk meer audio en beeld terug, maar in
> deze opnamen werd geen herstel door XOR gemeld. Alle varianten hielden
> onderbrekingen. Omdat de gegenereerde ruis tussen de proeven verschilde,
> beschouwen we dit als een praktijkwaarneming en niet als bewijs dat één
> bondingmethode onder alle omstandigheden beter is.

## Bewaarde gegevens

Het gecombineerde meetoverzicht staat in
[`benchmarks/dvbt-bonding-drie-proeven-2026-11-15.json`](../benchmarks/dvbt-bonding-drie-proeven-2026-11-15.json).
De afzonderlijke rapporten bevatten instellingen, inputhashes en beperkingen:

- [`dvbt-bonding-plus3db-2026-11-15.json`](../benchmarks/dvbt-bonding-plus3db-2026-11-15.json)
- [`dvbt-bonding-shared-125w-2026-11-15.json`](../benchmarks/dvbt-bonding-shared-125w-2026-11-15.json)
- [`dvbt-bonding-shared-250w-2026-11-15.json`](../benchmarks/dvbt-bonding-shared-250w-2026-11-15.json)

Volledige statusresponses, decoderlogs, IQ-kopieën, TS, PCM en analysescripts
staan respectievelijk in `build/bonding-plus3db-check/`,
`build/bonding-shared-comparison/` en `build/bonding-shared-250w-new-check/`.
De eerdere schone-broncontrole staat in `build/bonding-clean-source-check/`.
