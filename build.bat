@echo off
rem Both entry points build the complete emulator, including its GPU library.
call "%~dp0cmbuild.bat" %*
exit /b %errorlevel%
