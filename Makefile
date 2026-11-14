# Jet - NMAKE makefile (Visual C++ 6.0), DESIGN 2.8. Build in the VM: ~/Documents/86box/samba/w98 build jet
# Stage 1: engine sources -> engine\obj\*.obj -> engine\engine.lib (sdk\dx6guid.cpp included; never dxguid.lib).
# Stage 2: game sources (src\) -> obj\*.obj, linked with engine.lib -> jet.exe.
# All objects depend on all headers (simple and safe). New module? Add its .obj to EOBJS or GOBJS (and its .cpp to
# tools/simh/Makefile if it is portable). Owner: T01; other tasks ask the orchestrator to add files.
CFLAGS = /nologo /W3 /O2 /G6 /DWIN32 /D_WINDOWS /Isdk /Iengine\core /Iengine\coll /Iengine\world /Iengine\rend \
         /Iengine\plat /Isrc
LIBS   = user32.lib gdi32.lib winmm.lib ddraw.lib dsound.lib dinput.lib advapi32.lib

EOBJS  = engine\obj\e_base.obj engine\obj\e_math.obj engine\obj\e_log.obj engine\obj\e_cfg.obj \
         engine\obj\e_file.obj engine\obj\e_image.obj engine\obj\e_prof.obj \
         engine\obj\e_coll.obj \
         engine\obj\e_world.obj engine\obj\e_mesh.obj engine\obj\e_tex.obj \
         engine\obj\e_terrain.obj engine\obj\e_terrain_draw.obj \
         engine\obj\e_rend.obj engine\obj\r_null.obj engine\obj\r_d3d6.obj \
         engine\obj\e_plat.obj engine\obj\e_input.obj engine\obj\e_audio.obj engine\obj\e_ui.obj \
         engine\obj\dx6guid.obj
GOBJS  = obj\main.obj obj\game.obj obj\params.obj obj\flight.obj obj\pilot.obj obj\takeoff.obj obj\world.obj \
         obj\radar.obj obj\target.obj obj\missile.obj obj\weapons.obj obj\dlz.obj obj\ai.obj obj\mission.obj \
         obj\replay.obj obj\tests.obj obj\tests_flight.obj obj\tests_pilot.obj obj\tests_takeoff.obj \
         obj\tests_world.obj obj\tests_terrain.obj obj\tests_radar.obj obj\tests_target.obj obj\tests_missile.obj \
         obj\tests_dlz.obj obj\tests_ai.obj obj\tests_mission.obj \
         obj\camera.obj obj\scene.obj obj\hud.obj obj\hud_font.obj obj\hud_map.obj obj\hud_tgt.obj obj\hud_wpn.obj \
         obj\fx.obj obj\snd.obj obj\snd_combat.obj obj\menu.obj obj\bench.obj obj\scatter.obj
HDRS   = engine\core\*.h engine\coll\*.h engine\world\*.h engine\rend\*.h engine\plat\*.h src\*.h sdk\*.h

jet.exe: obj engine\obj engine\engine.lib $(GOBJS)
	link /nologo /subsystem:windows /out:jet.exe $(GOBJS) engine\engine.lib $(LIBS)

engine\engine.lib: engine\obj $(EOBJS)
	lib /nologo /out:engine\engine.lib $(EOBJS)

obj:
	-mkdir obj

engine\obj:
	-mkdir engine\obj

{engine\core}.cpp{engine\obj}.obj:
	cl $(CFLAGS) /c /Fo$@ $<

{engine\coll}.cpp{engine\obj}.obj:
	cl $(CFLAGS) /c /Fo$@ $<

{engine\world}.cpp{engine\obj}.obj:
	cl $(CFLAGS) /c /Fo$@ $<

{engine\rend}.cpp{engine\obj}.obj:
	cl $(CFLAGS) /c /Fo$@ $<

{engine\plat}.cpp{engine\obj}.obj:
	cl $(CFLAGS) /c /Fo$@ $<

{sdk}.cpp{engine\obj}.obj:
	cl $(CFLAGS) /c /Fo$@ $<

{src}.cpp{obj}.obj:
	cl $(CFLAGS) /c /Fo$@ $<

$(EOBJS) $(GOBJS): $(HDRS)

clean:
	-del engine\obj\*.obj
	-del engine\engine.lib
	-del obj\*.obj
	-del jet.exe
