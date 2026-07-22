@echo off
REM ============================================================
REM  loader.exe'i calistirir ve TUM ciktisini (stdout + stderr)
REM  hem "loader_log.txt" dosyasina kaydeder hem de konsolda gosterir.
REM
REM  Kullanim:
REM    run.bat PPSA02929-app0\eboot.bin
REM
REM  Not: cmd'nin "> dosya 2>&1" yonlendirmesi OS seviyesindedir;
REM  hem C (printf) hem C++ (cout/cerr) ciktisini temiz sekilde,
REM  satir sarmalamasi olmadan yakalar. Koddaki fflush/endl cagrilari
REM  sayesinde bir cokme/kilitlenme aninda bile ciktinin buyuk kismi
REM  diske yazilmis olur.
REM ============================================================
setlocal
set "LOGFILE=loader_log.txt"

echo [run.bat] Calistiriliyor: loader.exe %*
echo [run.bat] Cikti "%LOGFILE%" dosyasina kaydediliyor...
echo.

loader.exe %* > "%LOGFILE%" 2>&1

echo.
echo ============================================================
echo [run.bat] Bitti. Tam log: %LOGFILE%
echo ============================================================
echo.
echo ------------------- KONSOL CIKTISI -------------------------
type "%LOGFILE%"
endlocal
