# Jet T02 probe - NMAKE makefile (Visual C++ 6.0). Build: ~/Documents/86box/samba/w98 build jet\probe
# Needs ..\engine\engine.lib (built by `w98 build jet`). Owner: T02.
CFLAGS = /nologo /W3 /O2 /G6 /DWIN32 /D_WINDOWS /I..\sdk /I..\engine\core /I..\engine\coll /I..\engine\world \
         /I..\engine\rend /I..\engine\plat
LIBS   = user32.lib gdi32.lib winmm.lib ddraw.lib dsound.lib dinput.lib advapi32.lib

probe.exe: probe.obj ..\engine\engine.lib
	link /nologo /subsystem:windows /out:probe.exe probe.obj ..\engine\engine.lib $(LIBS)

probe.obj: probe.cpp ..\engine\core\*.h ..\engine\rend\*.h ..\engine\plat\*.h ..\engine\world\*.h
	cl $(CFLAGS) /c probe.cpp

clean:
	-del probe.obj
	-del probe.exe
