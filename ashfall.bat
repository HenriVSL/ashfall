@echo off
rem Ashfall: Island Intercept - starts at the title screen (FLY / OPTIONS / RECORDS / QUIT; docs\PLAY_HOWTO.md).
rem Resolution, preset, window/fullscreen, keys and volumes come from jet.ini (set them in OPTIONS).
rem Direct starts for play-tests: play.bat (free flight), training.bat, sortie.bat [easy|hard].
if exist jet.exe goto run
M:
cd \dev\jet
:run
jet.exe
