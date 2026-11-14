# Mac-side harness (clang): every portable file + simh.cpp -> tools/simh/simh (DESIGN 2.8, 16.3). Owner: T01.
# Usage from the jet folder: make -C tools/simh && tools/simh/simh test math
# New portable module? Add it to SRCS (and to GOBJS in the main Makefile). Render code compiles out with
# -DE_NO_RENDER (e_world/e_mesh loaders link without a renderer); e_terrain_draw.cpp is render-only (not here).
R      = ../..
CXX    = clang++
CXXFLAGS = -std=c++98 -O2 -Wall -Wno-unused-function -I$(R)/engine/core -I$(R)/engine/coll -I$(R)/engine/world \
           -I$(R)/engine/rend -I$(R)/src -DE_NO_RENDER
SRCS   = $(R)/engine/core/e_base.cpp $(R)/engine/core/e_math.cpp $(R)/engine/core/e_log.cpp \
         $(R)/engine/core/e_cfg.cpp $(R)/engine/core/e_file.cpp $(R)/engine/core/e_image.cpp \
         $(R)/engine/core/e_prof.cpp \
         $(R)/engine/coll/e_coll.cpp \
         $(R)/engine/world/e_world.cpp $(R)/engine/world/e_mesh.cpp $(R)/engine/world/e_terrain.cpp \
         $(R)/src/game.cpp $(R)/src/params.cpp $(R)/src/flight.cpp $(R)/src/pilot.cpp $(R)/src/takeoff.cpp \
         $(R)/src/world.cpp $(R)/src/radar.cpp $(R)/src/target.cpp $(R)/src/missile.cpp $(R)/src/weapons.cpp \
         $(R)/src/dlz.cpp $(R)/src/ai.cpp $(R)/src/mission.cpp $(R)/src/replay.cpp $(R)/src/tests.cpp \
         $(R)/src/tests_flight.cpp $(R)/src/tests_pilot.cpp $(R)/src/tests_takeoff.cpp $(R)/src/tests_world.cpp \
         $(R)/src/tests_terrain.cpp $(R)/src/tests_radar.cpp $(R)/src/tests_target.cpp $(R)/src/tests_missile.cpp \
         $(R)/src/tests_dlz.cpp $(R)/src/tests_ai.cpp $(R)/src/tests_mission.cpp \
         simh.cpp
HDRS   = $(wildcard $(R)/engine/core/*.h $(R)/engine/coll/*.h $(R)/engine/world/*.h $(R)/engine/rend/*.h $(R)/src/*.h)

simh: $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) -o simh $(SRCS) -lm

clean:
	rm -f simh

.PHONY: clean
