# Native Windows x64

Dit is een experimentele native Windows-port van Linrad DVB. Het ZIP-package
bevat echte Windows `.exe`-bestanden, DLL's en plugins. WSL en MSYS2 zijn niet
nodig om het programma te gebruiken. Gebruik Windows 10/11, 64-bit.

## Starten

1. Pak **de volledige ZIP** uit in een schrijfbare map, bijvoorbeeld
   `C:\Users\Rob\Apps\LinradDVB`. Start niet vanuit de ZIP-weergave.
2. Start `linrad_dvb.exe`. Het consolevenster bevat foutmeldingen en decoderlogs.
3. Kies **Input**, **Mode** en **Start SDR**. Met **W** bewaar je de instellingen
   in `linrad-dvb.ini` naast de executable.
4. Houd de DLL's, `plugins/`, `lib/` en `share/` bij het programma. Je kunt de
   volledige map verplaatsen. Een nieuwe ZIP bevat een voorbeeldconfiguratie;
   bewaar je eigen INI voordat je een bestaande installatie vervangt.

De vier ingebouwde decoders blijven beschikbaar: DVB-S, DVB-S2, DVB-T en DVB-T2.
Het DVB-T2-bereik blijft gelijk aan de Linux-versie: de ondersteunde 8 MHz
broadcastmodi. Ingebouwde video gebruikt de meegeleverde libVLC-plugins.
Audio monitoring gebruikt het standaard Windows-audioapparaat via PortAudio;
laat `[usb] device=default` staan.

## Invoer en drivers

| Invoer | Windows-package |
| --- | --- |
| Linrad UDP | Ingebouwd; stem IP, UDP-poort, sample rate en IQ-formaat af op Linrad |
| IQ-bestand | Ingebouwd; s16, u8 en cf32, inclusief JSON-metadata |
| AFEDRI Ethernet | Ingebouwd; dezelfde TCP/UDP-instellingen als op Linux |
| HackRF | SoapyHackRF en libusb meegeleverd; passende Windows USB-driver nodig |
| RTL-SDR | SoapyRTLSDR en libusb meegeleverd; passende Windows USB-driver nodig |
| USRP B210 | UHD meegeleverd; USB-driver en bijpassende UHD FPGA/firmware-images apart nodig |
| FUNcube Pro+ | Directe HID/audio-invoer nog niet geporteerd; gebruik Linrad UDP |
| LimeSDR Mini | Native LimeSuite-backend niet meegebouwd; gebruik Linrad UDP |
| SDRplay, Airspy, RFSpace | Vereisen aanvullende compatibele SoapySDR 0.8 Windows-modules en leveranciersdrivers; niet meegeleverd/gevalideerd |

Voeg alleen passende 64-bit SoapySDR 0.8-modules toe aan
`lib/SoapySDR/modules0.8`; eventuele afhankelijkheden moeten ook beschikbaar
zijn. Linux `.so`-drivers werken niet op Windows. Hardwareontvangst moet nog op
een echte Windows-pc worden getest. Windows ondersteunt geen Linux
`SO_RXQ_OVFL`-teller; AFEDRI telt wel de ontvangen sequentiegaten en queuedrops.

Voorbeeld zonder GUI:

```powershell
.\linrad_dvb.exe --print-pipeline --standard DVB-S2 --sr 125000 --fec 1/2
.\linrad_dvb.exe --help
```

De externe speler `vlc.exe` wordt niet meegeleverd: de ingebouwde libVLC-speler
wel. FFmpeg en FFplay zijn meegeleverd. De bestaande externe GPU-knop gebruikt
CUDA/NVIDIA-instellingen en is geen algemene Windows-GPU-autodetectie.

## Statusendpoint

Via **Log → Status endpoint…** schakel je het endpoint voor alle modi direct
aan of uit. Kies `0.0.0.0` als luisteradres voor toegang vanaf andere computers.
Sluit het venster en druk **W** om de instelling voor volgende starts op te slaan.

DAB-status gaat via een pipe naar de applicatie en is met
`--status-port 8877 --status-bind 0.0.0.0` op `http://ONTVANGER-IP:8877/status` uit te lezen.
De applicatie schrijft daarvoor geen `state.json`. De JSON bevat ook recente
logmeldingen en fouten. Sta de TCP-poort toe in de Windows-firewall voor het
privénetwerk; dit uitleesendpoint heeft geen authenticatie.
## Bouwen en packagen op Linux

Vereist Python 3 met `pefile`, pkg-config, GNU tar/zstd, MinGW-w64 GCC/G++ met
`-mcrtdll=ucrt`, en een Rust-toolchain met target `x86_64-pc-windows-gnu`.
Rust-compiler en targetbibliotheek moeten uit dezelfde toolchain komen.
De C/C++-code en de SDK gebruiken de Windows UCRT. Bij GCC 15 met een GCC 16
SDK koppelt de builder de TLS-destructorhelper uit de eigen `libsupc++.a`
statisch mee; die static library moet dus ook geïnstalleerd zijn.

```sh
python3 tools/windows-deps.py build/windows-sdk
rustup target add x86_64-pc-windows-gnu
python3 tools/build-windows.py -j 4
python3 dev/package_windows.py
```

De dependency-helper downloadt naar een lokale SDK en controleert de SHA-256
uit de MSYS2-index. Hij installeert niets in het hostsysteem. Reken op enkele
gigabytes vrije schijfruimte; gebruik hiervoor geen kleine tmpfs.
`dependency-packages.json` registreert de precieze pakketversies/hashes.
De package-helper volgt recursief de PE-importtabellen en weigert ontbrekende
niet-systeem-DLL's én ontbrekende geïmporteerde functies.
`dll-imports.json` bewaart de gecontroleerde importboom.
De ZIP staat in `dist/linrad_dvb-windows-x64.zip`, naast de SHA-256-checksum.

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

Tests opnieuw uitvoeren op de cross-buildhost (Wine en `build/leandvbtx` nodig):

```sh
python3 tools/build-windows.py --tests -j 4
python3 dev/package_windows.py --stage-only
cp build/windows/windows_tls.exe build/windows/test_dvbt2_fec.exe build/windows/test_dvbt2_l1.exe build/linrad_dvb-windows-x64/
python3 tests/test_windows.py
```

Maak daarna opnieuw het definitieve package; de testprogramma's worden niet
meegeleverd. `tests/windows_gui_smoke.c` is de aanvullende GUI/video-test.
