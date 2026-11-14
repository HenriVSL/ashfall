# e_terrain_view.mak - T04 terrain/sea viewer (screenshots + bench outside jet.exe). NMAKE, VC6. Owner: T04.
# Build from M:\dev\jet after engine\engine.lib exists:  w98 build jet /f engine\world\e_terrain_view.mak
CFLAGS = /nologo /W3 /O2 /G6 /DWIN32 /D_WINDOWS /Isdk /Iengine\core /Iengine\coll /Iengine\world /Iengine\rend \
         /Iengine\plat
LIBS   = user32.lib gdi32.lib winmm.lib ddraw.lib dsound.lib dinput.lib advapi32.lib

engine\world\e_terrain_view.exe: engine\world\e_terrain_view.obj engine\engine.lib
	link /nologo /subsystem:windows /out:engine\world\e_terrain_view.exe engine\world\e_terrain_view.obj engine\engine.lib $(LIBS)

engine\world\e_terrain_view.obj: engine\world\e_terrain_view.cpp engine\world\*.h engine\core\*.h engine\rend\*.h engine\plat\*.h
	cl $(CFLAGS) /c /Foengine\world\e_terrain_view.obj engine\world\e_terrain_view.cpp

clean:
	-del engine\world\e_terrain_view.obj
	-del engine\world\e_terrain_view.exe
