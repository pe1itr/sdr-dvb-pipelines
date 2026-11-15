# Experimentele DVB-T channel bonding

De ontvanger gebruikt de v1-merger uit `dvb-channel-bonding`, vastgelegd in
`components/bonding/ORIGIN`. Twee zelfstandige DVB-T-decoders gebruiken dezelfde
IQ-invoer en dezelfde digitale afstemming/het kanaalfilter als de headless
multirx-repeater. Elke decoder levert zijn volledige kanaal-TS na RS-correctie
aan de merger. De gereconstrueerde of geremuxte TS gaat bij ingeschakelde bonding
naar de bestaande TSscatterFix-, programma-, opname- en spelerverwerking.
Het protocol is experimenteel en geen algemene DVB-bondingstandaard.

De component is bijgewerkt naar **1.5.1**, bronrevisie `a82d373`. De zender
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

Versie 1.5.1 publiceert voor gedeelde XOR16-bonding met één interleavegroep
en 50.100 bit/s een bron-TS-timingprofiel via het zendercontract. Dit beschrijft
onder meer PCR op de video-PID met maximaal 100 ms tussenruimte en PAT/PMT
met maximaal 500 ms tussenruimte. Het profiel staat automatische inrichting
van bron-TS-timing toe; codecinstellingen en volledige ETSI-conformiteit worden
daarmee niet vrijgegeven. Andere configuraties krijgen dit profiel niet
als hun transportcapaciteit of profielinstellingen niet passen. De ontvanger
heeft hiervoor geen nieuwe instelling of aangepaste afspeelvertraging nodig.

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
  handmatig te bonden. De keuze geldt bij de volgende ontvangststart.
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
