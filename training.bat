@echo off
rem Jet M2 TRAINING (docs\PLAYTEST_M2.md): runway start, 3 non-shooting drones ahead, unlimited missiles.
rem Fullscreen 800x600, Low preset, dev keys on. Recorded to last.rpl, log jet.log.
if exist jet.exe goto run
M:
cd \dev\jet
:run
jet.exe -fullscreen -res 800 600 -preset low -dev -mode training
