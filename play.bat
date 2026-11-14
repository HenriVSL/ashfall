@echo off
rem Jet free flight (docs\PLAYTEST_M1.md): fullscreen 800x600, Low preset, dev keys on (F9 = air start, F10 = feel
rem overlay). Skips the title (-mode free); ashfall.bat starts at the title. Combat: training.bat (drones) and sortie.bat [easy|normal|hard] (docs\PLAYTEST_M2.md).
rem Every flight is recorded to last.rpl, the log is jet.log.
if exist jet.exe goto run
M:
cd \dev\jet
:run
jet.exe -fullscreen -res 800 600 -preset low -dev -mode free
