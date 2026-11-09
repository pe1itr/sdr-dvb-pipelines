@echo off
cd /d "%~dp0"
echo Close the receiver before running this check.
"%~dp0linrad_dvb.exe" --decoder-worker=dvbt2 --gpu-check > "%~dp0dvbt2-gpu-check.log" 2>&1
set gpu_result=%errorlevel%
type "%~dp0dvbt2-gpu-check.log"
echo GPU check exit code: %gpu_result% (0 = self-test passed, 1 = CPU fallback)
pause
exit /b %gpu_result%
