# BATC Knucker op Raspberry Pi

De publieke Knucker-ingang is beschikbaar op ARM Linux. De officiële
BATC-binary vereist een 32-bits ARM-runtime (armhf), ook op een Pi met een
64-bits besturingssysteem.

Een clone van de officiële BATC-repository volstaat om de ontvangersoftware
te verkrijgen. Bijvoorbeeld vanaf je eigen account op de Pi:

```sh
git clone https://github.com/BritishAmateurTelevisionClub/ryde-build.git "$HOME/ryde-build"
```

Selecteer **Input → BATC Knucker** en vul bij **Path to CombiTunerExpress binary** het volledige
pad naar `configs/dvbt/CombiTunerExpress` in je clone in. Bij een clone in
`/home/pi/ryde-build` wordt dat **`/home/pi/ryde-build/configs/dvbt/CombiTunerExpress`**.
Dit is het pad in de voorbeeld-INI. Pas het aan als je clone elders staat. Druk op **W** om het pad te bewaren.

Je kunt ook alleen het bestand ophalen via **Download raw file** op GitHub.
Maak het na een losse download uitvoerbaar en vul het gekozen pad in. Voor
een bestaande Ryde-installatie kun je `/home/pi/dvbt/CombiTunerExpress` invullen.
[Officiële binary van BATC](https://github.com/BritishAmateurTelevisionClub/ryde-build/blob/master/configs/dvbt/CombiTunerExpress)

Stel de tunerfrequentie in tussen 44 000 en 1 002 000 kHz, in stappen van
1 kHz. Kies onder **Mode** de
DVB-T-bandbreedte. Bij **150–500** kun je een geheel aantal kHz invoeren,
bijvoorbeeld 333. De bredere standen van CombiTunerExpress zijn 1000, 1700,
2000, 5000, 6000, 7000 en 8000 kHz. De ontvanger detecteert constellatie, FEC en GI.
[Knucker-project van BATC](https://wiki.batc.org.uk/Knucker#Software)
