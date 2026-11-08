@echo off
setlocal
cd /d "%~dp0"
if not exist "linrad_dvb.exe" (
    echo Plaats dit bestand naast linrad_dvb.exe en start het opnieuw.
    pause
    exit /b 1
)
:logname
set "profile_log=dvbt2-profile-%RANDOM%-%RANDOM%.log"
if exist "%profile_log%" goto logname
set "LINRAD_DIAGNOSTIC_LOG=1"
set "DECT2_STAGE_PROF=1"
echo Tijdmeting DVB-T2. Logbestand: %profile_log%
echo Kies DVB-T2 en start de B210 of speel een IQ-opname af.
echo Tijdmetingen verschijnen elke 5 seconden, ook via het statusendpoint.
echo Sluit daarna de applicatie om dit logbestand te kunnen delen.
echo De tijden zijn cumulatieve CPU-seconden per decoderstap, zonder wachttijd.
"%~dp0linrad_dvb.exe" %* > "%profile_log%" 2>&1
set "profile_exit=%ERRORLEVEL%"
echo.
echo Applicatie afgesloten met code %profile_exit%.
echo Logbestand: "%~dp0%profile_log%"
findstr /C:"DVB-T2 profile:" "%profile_log%"
if errorlevel 1 echo Geen tijdsoverzicht gevonden. Laat DVB-T2 minstens 10 seconden ontvangen.
pause
exit /b %profile_exit%
