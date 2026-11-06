# Decoder development and measurements

[Back to README](../README.md)

Commands below run from the repository root. The detailed notes are in Dutch unless stated otherwise.

### Ingebouwde DVB-S/S2-decoder en LDPC

De GUI start `linrad_dvb --decoder-worker=leansdr`, zonder de oude X11-vensters.
De vastgelegde bronversies en wijzigingen staan in
[third_party/leansdr/ORIGIN](../third_party/leansdr/ORIGIN) en
[third_party/ldpc/ORIGIN](../third_party/ldpc/ORIGIN). `make package` levert ook de
bijbehorende licenties mee. De oorspronkelijke checkouts worden niet aangepast.

De gekozen DVB-S2-constellatie en FEC vormen een vaste beperking: bij **QPSK 1/2**
wordt uitsluitend MODCOD 4 geaccepteerd. De decoder moet de frameheader nog lezen
voor onder meer framelengte en pilots, maar frames met een andere MODCOD worden
niet doorgestuurd naar de foutcorrectie. Alleen geaccepteerde frames mogen het
infopaneel bijwerken. Zonder lock toont dat paneel de ingestelde modulatie en
FEC; de lockindicatoren geven afzonderlijk aan of er werkelijk ontvangst is.

De gewone start kiest DVB-S2-acquisitie en verwerking standaard automatisch
op basis van de ingestelde symbolrate, ook na het openen van een IQ-bestand:

| Symbolrate | Acquisitie | LDPC-verwerking |
|---|---|---|
| Tot en met 333 ksym/s, inclusief contest op 125k | Continu zoeken (`--fastlock`) | Low latency, één frame per batch |
| Boven 333 ksym/s, inclusief PI6EHV op 5M 8PSK | Normaal zoeken | Throughput, maximaal 32 frames per batch |

Beide gebruiken standaard één worker, RRC rejection 10 en 8 interpolatiestappen.
Er is geen aparte startconfiguratie nodig: kies de juiste modulatie, FEC en
symbolrate in **Mode**. DVB-S behoudt zijn bestaande `--fastlock --hq`-instelling.
Dit is een keuze op basis van de uitgevoerde metingen, geen automatische
meting van signaalsterkte of een optimalisatie voor ieder mogelijk kanaal.

In **Mode** staan vier DVB-S2-instellingen:

- **Error correction**: soft-decision LDPC (standaard) of de bestaande bit-flipping.
- **LDPC workers**: 1–8 workers in totaal. Begin met één; meer workers kosten extra
  geheugen en helpen alleen wanneer FEC de doorvoer beperkt.
- **LDPC processing**: standaard *Automatic (symbol rate)*. Handmatig:
  *Low latency* verwerkt steeds één frame zonder te wachten op extra frames;
  *Throughput* verzamelt maximaal 32 frames, met hoogstens 2 ms wachttijd voor
  batchvorming. Dit is niet de totale ontvangstvertraging.
- **DVB-S2 acquisition**: standaard *Automatic (symbol rate)*; *Continuous search*
  en *Normal search* blijven als handmatige keuzes beschikbaar.

De instellingen worden met de bestaande configuratie-opslag bewaard:

```ini
[leandvb]
backend=embedded
ldpc_mode=soft
helpers=1
latency=auto
acquisition=auto
```

Automatische keuzes worden bij iedere nieuwe pipeline opnieuw afgeleid;
wisselen van 125k naar 5M en terug vergt dus geen aangepaste configuratie.
Oude expliciete `latency=low`/`throughput`-instellingen blijven handmatige keuzes.
Extra decoderargumenten zoals een expliciete `--fastlock` of `--hq` blijven
eveneens van kracht; laat die weg uit `dvbs2_args` voor automatische acquisitie.

De worker beheert begrensde wachtrijen, bewaart framevolgorde en werkt resterende
frames bij bestandseinde af. De helper gebruikt een privéprotocol met expliciete
batchlengtes; grote kernel-pipebuffers of wijzigingen aan `/proc/sys` zijn niet
nodig. Stop beëindigt de volledige procesgroep. Op x86_64 bouwt `make` ook
`ldpc_tool_avx2`; `ldpc_tool` controleert de CPU en kiest die variant automatisch.
`LINRAD_LDPC_PORTABLE=1` forceert de draagbare variant voor vergelijking.
De AVX2-helper gebruikt voor 1–16 beschikbare frames de bestaande SSE4.1-kern
met 16 SIMD-lanes. Ook één frame wordt daarmee sneller verwerkt, zonder extra
batchvorming. Batches van 17–32 blijven ongesplitst in de AVX2-kern. Andere
builds behouden hun bestaande decoderkeuze; de limiet blijft 25 iteraties.

Voor **5 MSym/s, 8PSK 3/4** kiest Automatic dus **Throughput**, met standaard **1 worker**.
Meer workers zijn niet automatisch sneller: zij kunnen de beschikbare frames
over kleinere SIMD-batches verdelen. Meet met de eigen opname en machine.
De BCH-controle gebruikt nu tabellen om acht bits tegelijk te verwerken;
de syndromen en foutcorrectie blijven gelijk. `make test-bch` controleert
315 schone, herstelbare en onherstelbare gevallen over alle 21 S2-FEC-tabellen.

Gemeten op Ryzen 7 5800H met
`linrad-dvb-20261101T024957.298790Z-1291000000-8000000.iq`
(cs16, 8 MS/s, **19,415 s IQ**), directe bestandsinvoer zonder afspeelpauzes:

| Decoder | Workers / verwerking | Verwerkingstijd |
|---|---|---:|
| Voor BCH-optimalisatie | 1 / Low latency | 32,92 s |
| Voor BCH-optimalisatie | 1 / Throughput | 13,59 s |
| Voor BCH-optimalisatie | 4 / Throughput | 10,37 s |
| Met BCH-optimalisatie | 1 / Throughput | 5,76 s |
| Met BCH-optimalisatie | 4 / Throughput | 9,57 s |

Alle vijf runs leverden dezelfde 27.015.224 bytes MPEG-TS op (SHA-256
`b98c57bf044669098ae1bd4b4871a65deb327d87f6f4892e1a2a8dff6f652ccc`).
Dit zijn losse metingen van de decoder, inclusief LDPC-subprocessen, zonder
videorenderer. Realtime-invoer vormt kleinere batches en kan meer CPU gebruiken;
de tabel is dus geen garantie voor de volledige liveketen. Reproduceer met:

```sh
/usr/bin/time ./build/linrad_dvb --decoder-worker=leansdr \
  --s16 -f 8000000 --standard DVB-S2 --sr 5000000 \
  --sampler rrc --rrc-steps 8 --framesizes 3 --modcods 0x4000 \
  --ldpc-helper builtin --nhelpers 1 --ldpc-batch 32 \
  < linrad-dvb-20261101T024957.298790Z-1291000000-8000000.iq \
  > /tmp/dvbs2-5msym.ts
```

Met de realtime `iq_file_to_stdout` ervoor duurde de geoptimaliseerde run
met één worker **19,55 s**, met dezelfde TS-uitvoer. De totale CPU-tijd van
bronhelper, decoder en LDPC samen was 25,18 s (gemiddeld circa 1,29 CPU-core).
De videorenderer zat niet in deze meting.

Voor smalbandige weak-signalontvangst kiest Automatic **Low latency, 1 worker**
en continu zoeken. De BCH-optimalisatie verandert geen
filtering, acquisitie, LDPC-iteratielimiet of codeersterkte.

**LDPC-onderzoek met live bestandsinvoer (2026-11-05).**
`tools/ldpc_experiment.py` bouwt afzonderlijke onderzoekshelpers in een opgegeven
werkmap. De normale decoder en LDPC-helper worden daarbij niet vervangen.
De twee referenties zijn de volledige `recordings/fbn2.raw` (DL4FBN, QPSK
125 ksym/s, korte frames, 1/2, 215,686 s) en
`recordings/linrad-dvb-20261101T103906.819269Z-1291000000-6000000.iq`
(1291 MHz, 8PSK 5 Msym/s, normale frames, 3/4, 48,999 s).
De bron biedt de originele samplerates van 1.010.526 en 6.000.000 samples/s aan;
bij DL4FBN wordt de RAW-header van 41 bytes overgeslagen. Als de decoder de
invoer tijdelijk tegenhoudt, vertraagt de bron zonder samples over te slaan.
De werkelijk gemeten afspeelduur hoort daarom bij iedere meting.

DL4FBN behoudt **één frame per keer, één worker en continuous search**. De snelle
opname gebruikt de bestaande throughput-instelling: maximaal 32 frames, één
worker en maximaal 2 ms wachten op batchvorming. Alle varianten behouden de
limiet van 25 LDPC-iteraties en dezelfde foutcorrectieformules.

Profilering van de snelle opname gaf 11.293 frames in 6.440 batches; 91% van de
batches bevatte slechts één of twee frames. De LDPC-kern gebruikte 32,27 CPU-s,
tegenover 1,56 CPU-s voor inpakken en uitpakken. Bij DL4FBN waren er 477 losse
frames: 18,43 CPU-s LDPC-werk en ongeveer 0,006 CPU-s bufferomzetting. Van deze
pogingen bereikten 247 de iteratielimiet zonder een geslaagde LDPC-pariteitscheck.
De BCH-stap blijft ook dan ongewijzigd; een mislukte LDPC-check betekent niet
automatisch dat BCH niets meer kan herstellen.

De kandidaat `single16` gebruikt de bestaande SSE4.1-vectorbewerkingen voor
1–16 beschikbare frames en behoudt AVX2 voor grotere batches. **Zestien lanes
betekent bij DL4FBN nog steeds één echt frame**, met de overige lanes ongebruikt:
er wordt niet op extra frames gewacht. Deze proef is gericht op x86 met AVX2.
`medium` laat de bestaande scalarberekening voor één frame staan; `packing`
slaat alleen overbodig nulmaken over. Deze varianten zijn onderzoeksopties,
geen nieuwe GUI-instellingen.

De tool kan echte LLR-batches met hun decoderuitvoer vastleggen en opnieuw
aanbieden. De losse LLR-proef is ongepaceerd en meet alleen de helper; dat is
geen bewijs voor dezelfde winst in de live ontvanger. Voor de uiteindelijke
vergelijking gebruikt `single16-plain` geen meetinstrumentatie en worden CPU-tijd
van decoder inclusief helpers, CPU-tijd van de bron, afspeelduur, eerste TS,
alle TS-bytes en alle BB-framebytes apart vastgelegd. Videoweergave en GUI zitten
niet in deze meting.

De volledige live vergelijking op Ryzen 7 5800H geeft:

| Opname | Bestaand: CPU-s decoder + helpers | `single16`: CPU-s decoder + helpers | Minder CPU |
| --- | ---: | ---: | ---: |
| DL4FBN, één frame per keer | 47,13 | 39,34 | **16,5%** |
| 1291 MHz, throughput | 49,83 | 46,37 | **6,9%** |

Voor DL4FBN is dit één volledig A/B-paar; voor 1291 MHz de medianen van twee
volledige runs per variant. Inclusief de bronhelper bedragen de besparingen
15,3% en 6,9%. Alle negen volledige runs (inclusief afzonderlijke profielruns)
leverden per opname identieke TS- en BB-bytes: DL4FBN 965 TS-pakketten en
206.565 BB-bytes; 1291 MHz 362.845 TS-pakketten en 68.327.892 BB-bytes.
Alle samples zijn door de bron aangeboden. DL4FBN liep in beide varianten door
backpressure uit tot ongeveer 217,4 s; de snelle opname duurde ongeveer 49,1 s.
De eerste TS bleef rond 0,9 s respectievelijk 0,3 s. Dit is CPU-winst, geen
claim dat er meer beeld of meer ontvangstgevoeligheid is gewonnen.

De aanvullende profielrun verklaart een deel van het verschil met de losse
LDPC-proef: de snellere helper krijgt bij live invoer vaker maar één frame.
Op 1291 MHz steeg het aantal batches van 6.440 naar 7.879 voor dezelfde 11.293
frames; daardoor daalde de gemeten LDPC-kern-CPU daar slechts van 32,27 naar
29,08 s. Winst uit ongepaceerde helpertests mag dus niet rechtstreeks worden
vertaald naar de live keten. Alleen minder buffergeheugen nulmaken gaf in de
losse proeven geen overtuigende winst.

De [meetgegevens](../benchmarks/ldpc-2026-11-05.json) bevatten de invoer- en
uitvoerhashes, commando's, individuele runs, batchverdelingen en 15 bitgelijke
LLR-vergelijkingen. Na aanvullende controles is `single16` ingebouwd in de
normale AVX2-helper. `make test-ldpc` controleert alle **21 LDPC-tabellen en 52
ondersteunde MODCOD/frameformaat-combinaties**, met batches van 1, 2, 15, 16, 17,
31 en 32 frames en herhaalde overgangen tussen grote en kleine batches.
De proef vergelijkt alle gedecodeerde LLR-bytes met het eerdere decoderbeleid
en controleert codewoorden/pariteit met een afzonderlijke encoder. Per tabel
worden 119 frames getest: schoon, herstelbaar, verzadigd of onherstelbaar.
Gereserveerde MODCOD-combinaties, ongeldige batchlengtes en afgebroken invoer
worden eveneens gecontroleerd. De integratiecontroles en volledige herhaling
met beide opnamen staan in het [integratierapport](../benchmarks/ldpc-integration-2026-11-05.json).
De ingebouwde versie leverde bij één aanvullende volledige live run per opname
opnieuw exact dezelfde TS- en BB-bytes, met alle samples verwerkt. Decoder plus
helpers gebruikten 38,30 CPU-s voor DL4FBN en 46,97 CPU-s voor 1291 MHz, tegenover
de eerdere referenties van 47,13 en 49,83 CPU-s. De bronduur was 217,395 en
49,113 s; de eerste TS kwam na 0,906 en 0,298 s. Dit zijn aanvullende controles
tegen de bewaarde referenties, geen nieuwe herhaalde A/B-meetreeks.
Het enkel-framepad gebruikt meer werkgeheugen dan de scalarvariant;
deze x86-metingen doen geen uitspraak over ARM of langdurige RF-ontvangst.

Reproduceer de volledige live A/B-vergelijking (lokale opnamen vereist):

```sh
python3 - <<'PY'
import json
from pathlib import Path
report = json.loads(Path('benchmarks/ldpc-2026-11-05.json').read_text())
Path('/tmp/ldpc-cases.json').write_text(json.dumps(report['cases']))
PY
python3 tools/ldpc_experiment.py build /tmp/ldpc-check --variants baseline-plain,profile,single16-plain
python3 tools/ldpc_experiment.py run /tmp/ldpc-check /tmp/ldpc-cases.json --label baseline --helper /tmp/ldpc-check/baseline-plain
python3 tools/ldpc_experiment.py run /tmp/ldpc-check /tmp/ldpc-cases.json --label candidate --helper /tmp/ldpc-check/single16-plain
```

De onderzoeksbuild gebruikt standaard de helperbron uit Git-revisie `eb2e09d`
(vóór deze wijziging); de Git-historie moet dus beschikbaar zijn. De variant
`current` gebruikt de huidige bron. `run` zonder `--helper` test de ingebouwde
helper uit de normale build.

Voor profilering kan `--helper /tmp/ldpc-check/profile --capture-every N` worden
gebruikt, met een label dat begint met `profile-` of exact `profile`. De meting
hierboven gebruikte iedere batch bij DL4FBN en iedere zestiende batch op
1291 MHz. `merge` combineert vastgelegde batches per opname/MODCOD/frameformaat;
`replay` vergelijkt onderzoekshelpers met de opgeslagen decoderuitvoer en stopt
bij een byteverschil. Bestaande outputs met dezelfde labels worden overschreven.

`tests/test_weak_signals.py` vergelijkt een bewaarde decoder van vóór de
wijziging met de actuele build, steeds op exact dezelfde IQ-samples. Het
zwaartepunt is **125 ksym/s**, voor DVB-S en DVB-S2 QPSK; daarnaast worden
35, 66, 250 en 333 ksym/s getest. De synthetische gevallen bevatten AWGN,
frequentieafwijking/drift en diepe fading. De 125k-sweeps gebruiken twee
ruisrealisaties, verschillende FEC's, normale/korte S2-frames en pilots aan/uit.
De test vereist zowel gedeeltelijke als goede ontvangst, vergelijkt alle
TS-bytes en gedecodeerde S2-BB-frames, en controleert synthetische TS-payloads
tegen de verzonden inhoud. Legacy DVB-S kan beschadigde RS-pakketten
doorlaten bij lockverlies; die worden geteld en moeten ook bytegelijk blijven.

```sh
python3 tests/test_weak_signals.py --reference /pad/naar/linrad_dvb-voor-wijziging
python3 tests/test_weak_signals.py --reference /pad/naar/linrad_dvb-voor-wijziging \
  --recordings-only --output build/diagnostics/weak-recordings
```

De tweede opdracht gebruikt de volledige lokale `fbn2.raw`, `org.raw` en
`h.raw` met hun metadata. Deze IQ-bestanden zijn geen onderdeel van Git.
`results.json`, TS/BB-uitvoer en logs komen in de opgegeven uitvoermap
(standaard `build/diagnostics/weak-signals`). De vergelijking gebruikt bij
beide binaries dezelfde huidige LDPC-helper en standaard batches van één
frame: zo wordt de BCH-wijziging geïsoleerd. Dit is een deterministische
regressiecontrole, geen geijkte gevoeligheidsmeting of videoweergavetest.
Drie representatieve 125k-gevallen (normale en korte frames, ontvangst rond
de grens en herstel na fading) zitten ook in `make test-leansdr`. Die snelle
controle heeft geen oude binary nodig en controleert zowel een minimumaantal
herstelde pakketten als hun inhoud en volgorde.

De uitgevoerde vergelijking na de BCH-wijziging gaf **116/116 identieke
synthetische resultaten**, inclusief alle TS-bytes en S2-BB-frames. De volledige
echte opnamen bleven eveneens gelijk: `fbn2.raw` leverde 550 TS-pakketten en
`org.raw` 8.561. `h.raw` gaf in beide versies geen uitvoer en bewijst daarmee
geen behoud van ontvangstgevoeligheid. De JSON-resultaten staan lokaal in
`build/diagnostics/weak-signals/results.json` en
`build/diagnostics/weak-recordings/results.json`; de referentiebinary is bewaard
als `build/diagnostics/weak-reference/linrad_dvb-before`.

Een aparte proef met de volledige zwakke `fbn2.raw` (circa 216 s, DVB-S2
QPSK 125k, FEC 1/2, korte frames) vergeleek zwaardere ontvangstinstellingen:

| Instelling, steeds Low latency / 1 worker | Ontvangen TS-pakketten |
|---|---:|
| Bestaande acquisitie, RRC rejection 10, 8 stappen | 550 |
| `--fastlock`, RRC rejection 10, 8 stappen | **965** |
| Bestaande acquisitie, RRC rejection 30, 8 stappen | 450 |
| `--fastlock`, RRC rejection 30, 8 stappen | 776 |
| `--fastlock`, RRC rejection 30, 16 stappen | 615 |
| `--fastlock`, RRC rejection 10, 8 stappen, experimentele helper met 50 iteraties | 970 |

Met `--fastlock` stijgt ook het aantal pakketten op video-PID 256 van 496
naar 867; de extra uitvoer bestaat dus grotendeels uit videodata. De volledige
opname kostte met deze instelling circa 31,5 s verwerking. Een realtimeproef
met de eerste 40 s duurde 40,4 s met 8,57 CPU-seconden voor bron en decoder
samen, exclusief videoweergave. Bij de normale acquisitie slaat de decoder
na mislukte synchronisatie delen van het signaal over om CPU te besparen;
`--fastlock` blijft zoeken. Ook de referentiedecoder levert daarmee dezelfde
965 pakketten: dit is winst uit de acquisitie-instelling. De BCH-versnelling
zelf behoudt dezelfde foutcorrectie. Deze ene opname bewijst geen algemene
gevoeligheidswinst in dB of ononderbroken beeld.

Bij de daaropvolgende praktijkcontrole bevestigde de gebruiker volledig beeld
van DL4FBN, waar dat vóór deze wijziging niet lukte.

Deze combinatie is nu de automatische keuze tot en met 333 ksym/s:
`--fastlock`, RRC rejection 10, 8 stappen en Low latency / 1 worker. De helper
houdt zijn bestaande 25 iteraties. Start gewoon de ontvanger; aparte profielen
voor weak signals of de 5 MSym/s-repeater zijn niet nodig.

```sh
./build/linrad_dvb
```

Het decoderpaneel toont QPSK/8PSK, MER (geen SNR), frequentieafwijking,
synchronisatie, RS/BCH-resultaten en TS-tellers. Voor S2 worden de daadwerkelijk
herkende modulatie/FEC, framegrootte en pilots getoond. Onbekende metingen krijgen
`—`; onbekende lockstatus blijft grijs. De geselecteerde servicenaam komt uit de
bestaande TS-parser. Constellatiepunten zijn genormaliseerd op de LeanDVB-schaal.
Telemetrie gebruikt niet-blokkerende lokale UDP met maximaal 20 updates/s.

**D margin** in Service info geeft voor DVB-S/S2 de geschatte ontvangstreserve:
gemeten MER minus de referentiedrempel voor de modulatie en FEC. Bijvoorbeeld
MER 10,0 dB bij DVB-S2 8PSK 3/4 met normale frames geeft `+2,1 dB`.
Rood betekent minder dan 0 dB reserve, oranje 0 tot 2 dB en groen vanaf 2 dB
(op de getoonde, afgeronde waarde). De tooltip toont de gebruikte drempel.
Zonder actuele MER, bekende sync/PL-lock of ondersteunde parameters staat er `—`;
FEC/TS-lock is niet vereist, zodat ook negatieve reserve zichtbaar kan blijven.
Bij DVB-T staat er voorlopig `—`, omdat hiervoor geen passende referentie is
ingevoerd. De berekening draait alleen in de GUI en verandert de decoder niet.

De DVB-S2-drempels komen uit
[ETSI EN 302 307-1 V1.4.1, tabel 13](https://www.etsi.org/deliver/etsi_en/302300_302399/30230701/01.04.01_60/en_30230701v010401p.pdf).
De werkelijk herkende MODCOD en framegrootte bepalen de drempel. Voor korte
frames gebruiken we een benaderde toeslag van 0,30 dB, de bovengrens van de
0,2–0,3 dB die de norm noemt. DVB-S gebruikt
[EN 300 421 V1.1.2, tabel 3](https://www.etsi.org/deliver/etsi_en/300400_300499/300421/01.01.02_60/en_300421v010102p.pdf),
omgerekend van Eb/N0 naar Es/N0 met `10 log10(2 × FEC × 188/204)`.
MER benadert Es/N0 bij witte ruis. Dit is een signaalrapport volgens hetzelfde
principe als MiniTioune, geen gekalibreerde ontvangstgrens van onze decoder:
de S2-norm veronderstelt ideale synchronisatie en 50 LDPC-iteraties, onze helper
gebruikt 25. Fading, interferentie en de MER-schatter beïnvloeden de praktijk.
S2 meet MER op bekende PL-header-symbolen; DVB-S gebruikt besliste symbolen,
waardoor de schatting bij veel ruis te gunstig kan zijn.

Optimalisaties uit de vergelijking met OnAir/DATVision worden eerst afzonderlijk
beproefd met dezelfde IQ-opnamen en hardware als de huidige versie. Meet zowel
realtime als maximale doorvoer: totale CPU-tijd inclusief helpers, geheugen,
wachtrijen/sampleverlies, tijd tot eerste geldige TS en gedecodeerde pakketten.
De 125 ksym/s-proeven met fading en korte signaaloplevingen, 5 Msym/s 8PSK 3/4
en DVB-T met meerdere FEC's blijven aparte acceptatiegevallen. Alleen een
herhaalbare winst zonder slechtere ontvangst rechtvaardigt integratie;
bitgelijke optimalisaties moeten dezelfde TS-bytes opleveren. Wijzigingen in
numeriek vereisen ook een vergelijking rond de ontvangstgrens en bij heracquisitie.
Voor acceptatie van **live SDR-verwerking** moet de bron op een vaste sampleklok
blijven leveren. Een bestandsbron die bij tegendruk vertraagt kan onvoldoende
doorvoer verbergen. Gebruik voor die controle een begrensde invoerbuffer met
expliciete verliescounters, controleer wachtrijgroei en vergelijk de werkelijke
bronduur met de nominale signaalduur. De onderzoekstool biedt dit met `--sdr-live`.

Het afzonderlijke [DVB-T-Viterbi-onderzoek](../components/dvbt/README.md#viterbi-simd-investigation-2026-11-05)
test een AVX2-proef met behoud van de floating-pointberekeningen. In 24 live
bestandsruns bleef de TS-uitvoer exact gelijk, met 2,4–13,3% minder decoder-CPU,
inclusief de 35/40 kHz-opnamen en een proef met ruis/fading/drift. Alle vijf FEC's
zijn bovendien per decoderstap vergeleken. De optimalisatie is nu ingebouwd:
automatisch AVX2 op geschikte x86-CPU's en NEON bij een AArch64-build, zoals
voor de ODROID-N2. Andere builds behouden de scalarroute. Het onderzoek en de
meetgegevens blijven afzonderlijk beschikbaar.
De [aanvullende hogere-FEC-proef](../components/dvbt/benchmarks/viterbi-high-fec-2026-11-05.json)
gebruikt zelf gegenereerde IQ met een vaste bronklok, zonder vertragen bij
tegendruk: 3/4, 5/6 en 7/8 op 250/500 kHz, plus 60 seconden 500 kHz/7/8 en
een fadingproef. Alle 24 runs hielden de invoer bij zonder sampleverlies of
groeiende FEC-wachtrij en leverden per geval identieke TS-bytes. Dit is een
SDR-invoersimulatie op deze x86-machine; de NEON-doorvoer moet op de N2 zelf
worden gemeten. `LINRAD_DVBT_VITERBI_SCALAR=1` forceert de oude rekenroute voor
een A/B-vergelijking. FEC, traceback, wachtrijen en ontvangstinstellingen blijven
gelijk. De decoder meldt `kernel=avx2-float`, `neon-float` of `scalar-float`.

De acquisitiecorrectie uit [LeanDVB issue #41](https://github.com/pabr/leansdr/issues/41)
van Ondřej OK1CDJ is opgenomen: een volledig zoekvenster voor normale frames,
betere eerste PLSCODE-selectie en afwijzing van onwaarschijnlijke eerste locks.
Daarnaast zijn transcriptiefouten in LDPC-tabellen B.2, C.3 en C.7 gecorrigeerd.
Alle 21 LDPC-tabellen worden vergeleken met de onafhankelijke xdsopl-tabellen.
De in issue #41 genoemde resterende carrierambiguïteit bij QPSK 4/5 short frames
is hiermee niet als opgelost aangemerkt.

`make test-leansdr` gebruikt gegenereerde TS en RRC-gevormde IQ, zonder hardware:
DVB-S 1/2 en 2/3, S2 QPSK/8PSK, normal/short frames, pilots aan/uit,
frequentieafwijking, ruis, parallelle verwerking, EOF, ongeldige opties en
telemetrie. De QPSK 3/5- en 5/6-regressies uit issue #41 worden afzonderlijk getest.
De tests controleren de terugontvangen pakketten tegen de oorspronkelijke TS.
De bestaande GUI-test controleert ook de satellietvelden en resetgedrag.
Deze tests bewijzen geen realtime doorvoer op 5 Msym/s of ontvangstgevoeligheid
op echte hardware; die blijven afzonderlijke meetpunten.

Help voor de applicatie en de meegebouwde decoders/helpers:

```bash
./build/linrad_dvb --help
./build/linrad_dvb --decoder-worker=leansdr --help
./build/linrad_dvb --decoder-worker=dvbt --help
./build/ldpc_tool --help
./build/ldpc_tool --version
```

De decoder-workeropties zijn bedoeld voor directe IQ-invoer en TS-uitvoer.
De LDPC-helper verwerkt soft-bitframes; hij ontvangt geen IQ of MPEG-TS.
Dezelfde commando’s staan onder **About → Command-line reference**.

Voor vergelijking met een externe decoder kan expliciet `backend=external`
worden ingesteld; alleen dan wordt `[paths] leandvb` gebruikt. De GUI-telemetrie
is beschikbaar voor de ingebouwde backend. Bestaande `--gui`-argumenten worden
bij de ingebouwde backend geaccepteerd en genegeerd.

### Ingebouwde DVB-T-decoder en verdeling van het werk

`make` bouwt de hier onderhouden broncode in `components/dvbt` mee in
`build/linrad_dvb`. De oorspronkelijke auteur is **Rob Hardenberg / PE1ITR**;
herkomst, ontwikkelinstructies en licentie staan in
[components/dvbt/README.md](../components/dvbt/README.md).
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
gedeeld met DVB-S/S2; de velden passen zich aan de gekozen standaard aan.

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
