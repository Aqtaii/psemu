@echo off
REM ============================================================
REM  loader.exe'i calistirir; TUM ciktisini (stdout + stderr)
REM  konsolda CANLI gosterir VE "loader_log.txt"e UTF-8 yazar.
REM
REM  Kullanim:
REM    run.bat PPSA02929-app0\eboot.bin
REM
REM  Nasil: 'cmd /c "loader.exe ... 2>&1"' stderr'i OS seviyesinde
REM  stdout'a birlestirir; PowerShell her satiri hem konsola
REM  ([Console]::WriteLine) hem UTF-8 StreamWriter'a (AutoFlush)
REM  yazar -> ANLIK akar, cokme aninda bile dosya tam kalir,
REM  ve dosya UTF-8'dir (grep/analiz temiz; Tee-Object'in UTF-16
REM  + null-byte sorunu YOK).
REM ============================================================
setlocal
set "LOGFILE=loader_log.txt"

echo [run.bat] Calistiriliyor: loader.exe %*
echo [run.bat] Cikti CANLI: ekran + "%LOGFILE%" (UTF-8)
echo.

powershell -NoProfile -ExecutionPolicy Bypass -Command "$sw=[System.IO.StreamWriter]::new('%LOGFILE%',$false,[System.Text.Encoding]::UTF8); $sw.AutoFlush=$true; try { cmd /c '.\loader.exe %* 2>&1' | ForEach-Object { [Console]::WriteLine($_); $sw.WriteLine($_) } } finally { $sw.Close() }"

echo.
echo ============================================================
echo [run.bat] Bitti. Tam log: %LOGFILE%
echo ============================================================
endlocal
