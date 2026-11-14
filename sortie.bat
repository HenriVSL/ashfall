@echo off
rem Jet M2 SORTIE (docs\PLAYTEST_M2.md): runway start, one wave of bandits after the takeoff, 3 lives.
rem Difficulty: sortie.bat (Normal), sortie.bat easy, sortie.bat hard (e.g. Start > Run: M:\dev\jet\sortie.bat hard).
rem Fullscreen 800x600, Low preset, dev keys on. Recorded to last.rpl, log jet.log.
set JETDIFF=1
if "%1"=="easy" set JETDIFF=0
if "%1"=="EASY" set JETDIFF=0
if "%1"=="hard" set JETDIFF=2
if "%1"=="HARD" set JETDIFF=2
if exist jet.exe goto run
M:
cd \dev\jet
:run
jet.exe -fullscreen -res 800 600 -preset low -dev -mode sortie -diff %JETDIFF%
set JETDIFF=
