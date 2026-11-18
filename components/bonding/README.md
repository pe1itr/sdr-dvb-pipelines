# DVB-T bonding over twee 40 kHz-kanalen

Kun je vanaf één zender één programma over twee DVB-T-kanalen van elk 40 kHz uitzenden en zo **minstens 1,7 keer de bruikbare transportcapaciteit van één 40 kHz-kanaal** halen? Om dat te onderzoeken hebben we twee manieren van bonding op MPEG-TS-niveau beproefd. Bij beide kan de kijker of luisteraar op een van de kanalen afstemmen. Linrad DVB ontdekt de partner en de bondingmodus automatisch.

De vraag is ook interessant voor radioamateurs in landen waar de maximale bandbreedte op VHF beperkt is. Wat telt hier als één uitzending? Op de radio zijn er twee DVB-T-signalen op verschillende frequenties, elk afzonderlijk af te stemmen en te demoduleren. Voor de kijker die beide ontvangt, vormen ze samen één programma. Bij *Separate lanes* kun je zelfs alleen naar het geluid op kanaal B luisteren terwijl iemand anders op kanaal A het beeld bekijkt. Of je dit twee aparte uitzendingen noemt of één gecombineerde uitzending, laten we als begripsvraag open. Hoe lokale bandbreedteregels zo’n opstelling behandelen, valt buiten deze proef.

## Bonding op MPEG-TS-niveau

De **bonding splitter** zit in de zender, tussen de programmamultiplexer en de DVB-T-modulatie. Hij neemt één MPEG-transportstream (MPEG-TS) aan en levert twee transportstreams, één voor elk 40 kHz-kanaal. De **bonding merger** zit in de ontvanger, na de DVB-T-demodulatie en foutcorrectie van beide kanalen. Hij verwerkt de twee ontvangen transportstreams tot één MPEG-TS voor de speler, opname of netwerkuitgang.

```text
                               ┌─ TS A → DVB-T-kanaal A (40 kHz) → TS A ─┐
Bron-TS → bonding splitter ────┤                                         ├─ bonding merger → uitgangs-TS
                               └─ TS B → DVB-T-kanaal B (40 kHz) → TS B ─┘
               zender                                                           ontvanger
```

## Twee manieren om de kanalen te gebruiken

| | Shared (distribution 0) | Separate lanes (distribution 2) |
| --- | --- | --- |
| Wat levert de splitter? | Hij verdeelt de bronpakketten en herstelgegevens over TS A en TS B. | TS A bevat een zelfstandig videoprogramma. TS B bevat een zelfstandig audioprogramma. |
| Wat ontvang je op één kanaal? | Een deel van het bondingtransport. Voor reconstructie van het volledige programma gebruikt de ontvanger beide kanalen. | **Elk kanaal is afzonderlijk te ontvangen én te decoderen met een gewone DVB-T-decoder die 40 kHz ondersteunt.** Stem af op A voor alleen beeld of op B voor alleen geluid. |
| Wat doet de merger? | Hij zet de bronpakketten terug in volgorde en gebruikt de beschikbare herstelgegevens om ontbrekende pakketten te reconstrueren. | Hij voegt de ontvangen video en audio op hun gemeenschappelijke tijdlijn samen tot één afspeelbare TS, met behoud van de gecodeerde media en hun presentatietimestamps. |

Bij **Separate lanes** heeft elk kanaal eigen programmatabellen en timing. Er is geen XOR-bescherming. Valt B weg, dan kan het beeld op A doorgaan. Wie alleen B ontvangt, kan rechtstreeks naar het audioprogramma luisteren. Voor de gecombineerde uitvoer heeft de merger het programma van A nodig. Hij bouwt de programmatabellen en pakkettellers opnieuw op.

### Beschermingsprofielen van Shared

De splitter bundelt telkens maximaal zeven MPEG-TS-pakketten tot één **bronsymbool**. Per groep bronsymbolen berekent hij één extra symbool met XOR-pariteit. Als één bronsymbool ontbreekt en de pariteit en alle overige symbolen van die groep op tijd binnen zijn, kan de merger de ontbrekende pakketten exact terugrekenen. De zender kiest het profiel en kondigt het aan in de PMT. De ontvanger neemt het automatisch over.

| Profiel | Wat verstuurt de splitter per volle groep? | Wat kan de merger herstellen? | Max. bron-TS bij de testinstellingen |
| --- | --- | --- | ---: |
| **XOR8** | 8 bronsymbolen + 1 pariteitssymbool | Eén ontbrekend bronsymbool per groep van 8. | 47.541 bit/s |
| **XOR16** (standaard) | 16 bronsymbolen + 1 pariteitssymbool | Eén ontbrekend bronsymbool per groep van 16. De lagere pariteitskosten laten meer ruimte voor de bron-TS. | 50.337 bit/s |
| **Hybrid16** | 16 bronsymbolen + 1 pariteitssymbool + 1 kopie op de andere lane. De splitter kiest het te kopiëren symbool op basis van de PES/NAL-inhoud. | Ontbreekt het gekozen symbool, dan kan de kopie het vervangen. Met de pariteit kan vervolgens nog één ander ontbrekend symbool uit dezelfde groep worden hersteld. | 47.541 bit/s |

De bitratelimieten gelden bij 30.160 bit/s per kanaal en een aankondigingsinterval van 2 seconden. De testbron van 50.100 bit/s past daarmee alleen bij XOR16. Bij andere instellingen geeft `dvb_bond_source_limit_v1()` het bijbehorende bronbudget. [Publieke C-API](include/dvb_bond.h)

Bij elk profiel kan de splitter **interleaving** gebruiken. Hij verstuurt dan afwisselend symbolen uit verschillende herstelgroepen. Zo kan een korte signaaluitval één symbool in elk van meerdere groepen treffen, die de merger elk afzonderlijk kan herstellen. Meer tijdspreiding kan een langere afspeelbuffer vragen. Zijn bij de afspeeldeadline nog bronpakketten zoek, dan vult de merger hun plaats met null-pakketten.

## Automatische detectie in Linrad DVB

1. Kies DVB-T met 40 kHz bandbreedte en stem af op het midden van A of B. Beide kanalen moeten binnen de IQ-invoer van dezelfde SDR of opname vallen. In de proeven was de kanaalafstand 50 kHz en de IQ-samplefrequentie 250 kS/s.
2. De ontvanger leest de bondingaankondiging uit de programmatabel (PMT) van het afgestemde kanaal. Daarin staan de sessie, de modus en de partnerfrequentie. Linrad DVB start een tweede decoder op de aangekondigde afstand, doorgaans +50 of −50 kHz, binnen dezelfde IQ-invoer.
3. Zodra de partner een overeenkomende aankondiging teruggeeft, toont de indicator **Paired**. De merger levert één TS voor afspelen, opnemen of netwerkuitvoer. De standaard afspeelbuffer is acht seconden.

Wil je een lane apart bekijken of beluisteren, kies dan vóór de ontvangststart **Mode → Disable automatic DVB-T bonding**. De ontvanger blijft de aankondiging herkennen en geeft de afgestemde kanaal-TS rechtstreeks door. Via de **Bonding**-indicator kun je de partner later alsnog bijschakelen. [Bediening en statusvelden](../../docs/dvbt-bonding.md)

## Wat is aangetoond?

In de transportsimulator is de capaciteitsdoelstelling gehaald. Bij 24.000 bit/s per gesimuleerd kanaal werd een synthetische bron-TS van 40.800 bit/s met XOR8 byte voor byte hersteld (**1,7×**). Met XOR16 lukte dat bij 43.200 bit/s (**1,8×**). Deze proef meet het transport van bondingframes binnen het simulatiemodel.

De DVB-T-proeven gebruikten QPSK, 2K, FEC 1/2 en GI 1/32. De ingestelde bron-TS van 50.100 bit/s tegenover 30.160 bit/s kanaal-TS geeft een verhouding van **1,66×**. Voor bevestiging van 1,7× over RF ontbreekt een meting van de netto bruikbare doorvoer tegenover één ongebonden 40 kHz-kanaal onder dezelfde omstandigheden.

De onafhankelijke audiolane is ook afzonderlijk gedecodeerd. Bij een schone IQ-opname leverde B 55,680 seconden aaneengesloten audio. Na samenvoegen bleef 55,168 seconden over. Na het instapverlies was de resterende gecodeerde audiopayload byte voor byte gelijk. In drie aanvullende opnamen uit een propagatiemodel zijn beide modi ook bij onderbroken ontvangst vergeleken. [Audiovergelijking](../../docs/dvbt-bonding.md#audiovergelijking-kmvkdn--15-november-2026) · [Verslag van de drie ontvangstproeven](../../docs/dvbt-bonding-drie-proeven-20261115.md)

Metingen van het RF-spectrum van de twee 40 kHz-signalen bij hoog zendvermogen worden later aan dit verslag toegevoegd.

## Gebruik in andere software en tests

De component biedt een C-API voor de zender (één bron-TS in, twee kanaal-TS'en uit) en de ontvanger (twee kanaal-TS'en in, één TS uit). Een generator kan via JSON de vereiste bronbitrate, timing en mediabudgetten opvragen. De ontvangende applicatie krijgt een gebeurtenis om de partnerdecoder te starten en kan de mergerstatus via JSON uitlezen. [Publieke C-API](include/dvb_bond.h)

Voer vanuit de hoofdmap van deze repository `make test-bonding-component` uit voor de vijf C-tests voor pakketherstel, partnerdetectie, TS-transport, zelfstandige audio/video en compatibiliteit met de v1-API/ABI. Met `make test-dvbt-bonding` test je ook de integratie met Linrad DVB, waaronder detectie vanaf beide kanalen en behoud van de mediapayload na samenvoegen.

Dit experimentele protocol is ontwikkeld door Rob Hardenberg (PE1ITR). Deze repository bevat componentversie 1.6.0 onder GPL-3.0-only. [Herkomst en revisie](ORIGIN) · [Licentie](LICENSE)
