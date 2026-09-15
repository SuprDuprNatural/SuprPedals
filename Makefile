# Supr pedals — LV2 plugins for PiPedal (SuprOctave, SuprOctavePlus,
# SuprEnvelopeFilter, SuprCompressor, SuprVU, SuprTuner, SuprSans, SuprBand,
# SuprChorus, SuprClack).
#
# On the Raspberry Pi:
#   make            build the plugins
#   sudo make install
#
# Anywhere (including macOS, no LV2 needed):
#   make test       build + run the offline DSP tests
#   make demo       render demo WAVs to build/demo/

PREFIX  ?= /usr/local
LV2_DIR ?= $(PREFIX)/lib/lv2
BUNDLE1  = suproctave.lv2
BUNDLE2  = suproctaveplus.lv2
BUNDLE3  = suprenvfilter.lv2
BUNDLE4  = suprcompressor.lv2
BUNDLE5  = suprvu.lv2
BUNDLE6  = suprtuner.lv2
BUNDLE7  = suprsans.lv2
BUNDLE8  = suprband.lv2
BUNDLE9  = suprchorus.lv2
BUNDLE10 = suprfuzz.lv2
BUNDLE11 = suprtransient.lv2
BUNDLE12 = suprclack.lv2
BUNDLE13 = suprforge.lv2
BUNDLE14 = suprecho.lv2
BUNDLE15 = suprspace.lv2

CXX      ?= g++
CXXFLAGS += -O3 -ffast-math -Wall -std=c++17

# Stateful DSP must produce identical samples across host block lengths.
# GCC fast-math/FMA specialization can change the recurrence (including
# Transient on ARM); use the same strict flags for wrappers and their tests.
STRICT_DSP_CXXFLAGS = $(filter-out -ffast-math,$(CXXFLAGS)) -ffp-contract=off

# Use system LV2 headers if available, otherwise the vendored copy.
LV2_CFLAGS := $(shell pkg-config --cflags lv2 2>/dev/null)
ifeq ($(strip $(LV2_CFLAGS)),)
LV2_CFLAGS := -Iinclude
endif

all: build/suprphase.so build/suprecho.so build/suprspace.so build/suproctave.so build/suproctaveplus.so build/suprenvfilter.so build/suprcompressor.so build/suprvu.so build/suprtuner.so build/suprsans.so build/suprchorus.so build/suprband.so build/suprfuzz.so build/suprtransient.so build/suprclack.so build/suprforge.so

build/suprforge.so: Makefile src/SuprForge.cpp src/ForgeDsp.h src/SansDsp.h src/OctaverDsp.h
	@mkdir -p build
	$(CXX) $(STRICT_DSP_CXXFLAGS) $(LV2_CFLAGS) -fPIC -shared -o $@ src/SuprForge.cpp

build/test_forge: Makefile test/test_forge.cpp src/SuprForge.cpp src/ForgeDsp.h src/SansDsp.h src/OctaverDsp.h
	@mkdir -p build
	$(CXX) $(STRICT_DSP_CXXFLAGS) $(LV2_CFLAGS) -Isrc -o $@ test/test_forge.cpp src/SuprForge.cpp

build/suproctave.so: Makefile src/SuprOctave.cpp src/OctaverDsp.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(LV2_CFLAGS) -fPIC -shared -o $@ src/SuprOctave.cpp

build/suproctaveplus.so: Makefile src/SuprOctavePlus.cpp src/OctaverPlusDsp.h src/OctaverDsp.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(LV2_CFLAGS) -fPIC -shared -o $@ src/SuprOctavePlus.cpp

build/suprenvfilter.so: Makefile src/SuprEnvFilter.cpp src/EnvFilterDsp.h src/OctaverDsp.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(LV2_CFLAGS) -fPIC -shared -o $@ src/SuprEnvFilter.cpp

build/suprcompressor.so: Makefile src/SuprCompressor.cpp src/CompressorDsp.h src/OctaverDsp.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(LV2_CFLAGS) -fPIC -shared -o $@ src/SuprCompressor.cpp

build/suprvu.so: Makefile src/SuprVu.cpp src/VuMeterDsp.h src/OctaverDsp.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(LV2_CFLAGS) -fPIC -shared -o $@ src/SuprVu.cpp

build/suprtuner.so: Makefile src/SuprTuner.cpp src/TunerDsp.h src/OctaverDsp.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(LV2_CFLAGS) -fPIC -shared -o $@ src/SuprTuner.cpp

build/suprsans.so: Makefile src/SuprSans.cpp src/SansDsp.h src/OctaverDsp.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(LV2_CFLAGS) -fPIC -shared -o $@ src/SuprSans.cpp

build/suprtransient.so: Makefile src/SuprTransient.cpp src/TransientDsp.h src/OctaverDsp.h
	@mkdir -p build
	$(CXX) $(STRICT_DSP_CXXFLAGS) $(LV2_CFLAGS) -fPIC -shared -o $@ src/SuprTransient.cpp

build/suprclack.so: Makefile src/SuprClack.cpp src/ClackDsp.h src/TunerDsp.h src/OctaverDsp.h
	@mkdir -p build
	$(CXX) $(STRICT_DSP_CXXFLAGS) $(LV2_CFLAGS) -fPIC -shared -o $@ src/SuprClack.cpp

build/suprband.so: Makefile src/SuprBand.cpp src/BandDsp.h src/SansDsp.h src/OctaverDsp.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(LV2_CFLAGS) -fPIC -shared -o $@ src/SuprBand.cpp

build/suprfuzz.so: Makefile src/SuprFuzz.cpp src/FuzzDsp.h src/SansDsp.h src/OctaverPlusDsp.h src/OctaverDsp.h
	@mkdir -p build
	$(CXX) $(STRICT_DSP_CXXFLAGS) $(LV2_CFLAGS) -fPIC -shared -o $@ src/SuprFuzz.cpp

build/suprchorus.so: Makefile src/SuprChorus.cpp src/ChorusDsp.h src/OctaverDsp.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(LV2_CFLAGS) -fPIC -shared -o $@ src/SuprChorus.cpp

build/test_octaver: Makefile test/test_octaver.cpp $(wildcard src/*.h)
	@mkdir -p build
	$(CXX) $(STRICT_DSP_CXXFLAGS) -Isrc -o $@ test/test_octaver.cpp

build/test_tuner: Makefile test/test_tuner.cpp src/SuprTuner.cpp src/TunerDsp.h src/OctaverDsp.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(LV2_CFLAGS) -Isrc -o $@ test/test_tuner.cpp src/SuprTuner.cpp

# The measurement rig SuprFuzz and SuprSans were fitted with. Dependency-free,
# unlike its counterpart tools/nam_probe.cpp, which needs NeuralAudio and so
# builds out of CMakeLists.txt with `make nam`.
build/fuzz_probe: Makefile tools/fuzz_probe.cpp src/FuzzDsp.h src/SansDsp.h src/OctaverPlusDsp.h src/OctaverDsp.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -Isrc -o $@ tools/fuzz_probe.cpp

tools: build/fuzz_probe

test: build/test_phase build/test_octaver build/test_tuner build/test_forge build/test_echo build/test_space
	./build/test_phase
	python3 test/validate_tone_motion.py
	./build/test_octaver
	./build/test_tuner
	./build/test_forge
	python3 test/validate_forge.py
	./build/test_echo
	./build/test_space
	python3 test/validate_time_space.py

demo: build/test_octaver
	@mkdir -p build/demo
	./build/test_octaver --demo build/demo
	@echo "Demo WAVs written to build/demo/"

install: all
	install -d $(DESTDIR)$(LV2_DIR)/suprphase.lv2
	install -m 644 build/suprphase.so ttl/suprphase.ttl ttl/phase-presets.ttl $(DESTDIR)$(LV2_DIR)/suprphase.lv2/
	install -m 644 ttl/phase-manifest.ttl $(DESTDIR)$(LV2_DIR)/suprphase.lv2/manifest.ttl
	install -d $(DESTDIR)$(LV2_DIR)/$(BUNDLE14) $(DESTDIR)$(LV2_DIR)/$(BUNDLE15)
	install -m 644 build/suprecho.so ttl/suprecho.ttl ttl/echo-presets.ttl $(DESTDIR)$(LV2_DIR)/$(BUNDLE14)/
	install -m 644 ttl/echo-manifest.ttl $(DESTDIR)$(LV2_DIR)/$(BUNDLE14)/manifest.ttl
	install -m 644 build/suprspace.so ttl/suprspace.ttl ttl/space-presets.ttl $(DESTDIR)$(LV2_DIR)/$(BUNDLE15)/
	install -m 644 ttl/space-manifest.ttl $(DESTDIR)$(LV2_DIR)/$(BUNDLE15)/manifest.ttl
	install -d $(DESTDIR)$(LV2_DIR)/$(BUNDLE1)
	install -m 644 build/suproctave.so ttl/suproctave.ttl $(DESTDIR)$(LV2_DIR)/$(BUNDLE1)/
	install -m 644 ttl/octave-manifest.ttl $(DESTDIR)$(LV2_DIR)/$(BUNDLE1)/manifest.ttl
	install -d $(DESTDIR)$(LV2_DIR)/$(BUNDLE2)
	install -m 644 build/suproctaveplus.so ttl/suproctaveplus.ttl $(DESTDIR)$(LV2_DIR)/$(BUNDLE2)/
	install -m 644 ttl/plus-manifest.ttl $(DESTDIR)$(LV2_DIR)/$(BUNDLE2)/manifest.ttl
	install -d $(DESTDIR)$(LV2_DIR)/$(BUNDLE3)
	install -m 644 build/suprenvfilter.so ttl/suprenvfilter.ttl $(DESTDIR)$(LV2_DIR)/$(BUNDLE3)/
	install -m 644 ttl/envfilter-manifest.ttl $(DESTDIR)$(LV2_DIR)/$(BUNDLE3)/manifest.ttl
	install -d $(DESTDIR)$(LV2_DIR)/$(BUNDLE4)
	install -m 644 build/suprcompressor.so ttl/suprcompressor.ttl $(DESTDIR)$(LV2_DIR)/$(BUNDLE4)/
	install -m 644 ttl/compressor-manifest.ttl $(DESTDIR)$(LV2_DIR)/$(BUNDLE4)/manifest.ttl
	install -d $(DESTDIR)$(LV2_DIR)/$(BUNDLE5)
	install -m 644 build/suprvu.so ttl/suprvu.ttl $(DESTDIR)$(LV2_DIR)/$(BUNDLE5)/
	install -m 644 ttl/vu-manifest.ttl $(DESTDIR)$(LV2_DIR)/$(BUNDLE5)/manifest.ttl
	install -d $(DESTDIR)$(LV2_DIR)/$(BUNDLE6)
	install -m 644 build/suprtuner.so ttl/suprtuner.ttl $(DESTDIR)$(LV2_DIR)/$(BUNDLE6)/
	install -m 644 ttl/tuner-manifest.ttl $(DESTDIR)$(LV2_DIR)/$(BUNDLE6)/manifest.ttl
	install -d $(DESTDIR)$(LV2_DIR)/$(BUNDLE7)
	install -m 644 build/suprsans.so ttl/suprsans.ttl $(DESTDIR)$(LV2_DIR)/$(BUNDLE7)/
	install -m 644 ttl/sans-manifest.ttl $(DESTDIR)$(LV2_DIR)/$(BUNDLE7)/manifest.ttl
	install -d $(DESTDIR)$(LV2_DIR)/$(BUNDLE8)
	install -m 644 build/suprband.so ttl/suprband.ttl $(DESTDIR)$(LV2_DIR)/$(BUNDLE8)/
	install -m 644 ttl/band-manifest.ttl $(DESTDIR)$(LV2_DIR)/$(BUNDLE8)/manifest.ttl
	install -d $(DESTDIR)$(LV2_DIR)/$(BUNDLE9)
	install -m 644 build/suprchorus.so ttl/suprchorus.ttl $(DESTDIR)$(LV2_DIR)/$(BUNDLE9)/
	install -m 644 ttl/chorus-manifest.ttl $(DESTDIR)$(LV2_DIR)/$(BUNDLE9)/manifest.ttl
	install -d $(DESTDIR)$(LV2_DIR)/$(BUNDLE10)
	install -m 644 build/suprfuzz.so ttl/suprfuzz.ttl $(DESTDIR)$(LV2_DIR)/$(BUNDLE10)/
	install -m 644 ttl/fuzz-manifest.ttl $(DESTDIR)$(LV2_DIR)/$(BUNDLE10)/manifest.ttl
	install -d $(DESTDIR)$(LV2_DIR)/$(BUNDLE11)
	install -m 644 build/suprtransient.so ttl/suprtransient.ttl $(DESTDIR)$(LV2_DIR)/$(BUNDLE11)/
	install -m 644 ttl/transient-manifest.ttl $(DESTDIR)$(LV2_DIR)/$(BUNDLE11)/manifest.ttl
	install -d $(DESTDIR)$(LV2_DIR)/$(BUNDLE12)
	install -m 644 build/suprclack.so ttl/suprclack.ttl $(DESTDIR)$(LV2_DIR)/$(BUNDLE12)/
	install -m 644 ttl/clack-manifest.ttl $(DESTDIR)$(LV2_DIR)/$(BUNDLE12)/manifest.ttl
	install -d $(DESTDIR)$(LV2_DIR)/$(BUNDLE13)
	install -m 644 build/suprforge.so ttl/suprforge.ttl ttl/forge-presets.ttl $(DESTDIR)$(LV2_DIR)/$(BUNDLE13)/
	install -m 644 ttl/forge-manifest.ttl $(DESTDIR)$(LV2_DIR)/$(BUNDLE13)/manifest.ttl

uninstall:
	rm -rf $(DESTDIR)$(LV2_DIR)/suprphase.lv2
	rm -rf $(DESTDIR)$(LV2_DIR)/$(BUNDLE14) $(DESTDIR)$(LV2_DIR)/$(BUNDLE15)
	rm -rf $(DESTDIR)$(LV2_DIR)/$(BUNDLE13)
	rm -rf $(DESTDIR)$(LV2_DIR)/$(BUNDLE1) $(DESTDIR)$(LV2_DIR)/$(BUNDLE2) $(DESTDIR)$(LV2_DIR)/$(BUNDLE3) $(DESTDIR)$(LV2_DIR)/$(BUNDLE4) $(DESTDIR)$(LV2_DIR)/$(BUNDLE5) $(DESTDIR)$(LV2_DIR)/$(BUNDLE6) $(DESTDIR)$(LV2_DIR)/$(BUNDLE7) $(DESTDIR)$(LV2_DIR)/$(BUNDLE8) $(DESTDIR)$(LV2_DIR)/$(BUNDLE9) $(DESTDIR)$(LV2_DIR)/$(BUNDLE10) $(DESTDIR)$(LV2_DIR)/$(BUNDLE11) $(DESTDIR)$(LV2_DIR)/$(BUNDLE12)

clean:
	rm -rf build

# --- SuprNAM ---------------------------------------------------------------
#
# The neural amp modeller is the one plugin here that is not dependency-free
# header DSP: it links NeuralAudio, and through it NAM Core, RTNeural, Eigen
# and math_approx, all of which are CMake projects. So it builds out of tree
# via CMakeLists.txt rather than joining the rules above. It also needs the
# full LV2 extension headers (atom, patch, worker, state), not the vendored
# single header — apt install lv2-dev, or brew install lv2.
#
#   make nam                      build it
#   make nam SUPR_CPU=cortex-a72  ... tuned for a Pi 4 (a76 for a Pi 5)
#   make nam-test                 run the graph, host and model checks
#   sudo make nam-install
#
# nam-test's model checks want two or more .nam files in models/; that
# directory is gitignored, so drop your own in and they will be picked up.

NAM_BUILD ?= build/nam
SUPR_CPU  ?=
NAM_JOBS  := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

nam:
	@test -f modules/NeuralAudio/NeuralAudio/CMakeLists.txt || \
	  { echo "modules/NeuralAudio is empty. Run:"; \
	    echo "    git submodule update --init --recursive"; exit 1; }
	cmake -S . -B $(NAM_BUILD) -DCMAKE_BUILD_TYPE=Release \
	    -DCMAKE_INSTALL_PREFIX=$(PREFIX) -DSUPR_CPU=$(SUPR_CPU)
	cmake --build $(NAM_BUILD) -j$(NAM_JOBS)

nam-test: nam
	python3 test/validate_ttl.py ttl/suprnam.ttl
	$(NAM_BUILD)/test_nam
	$(NAM_BUILD)/test_nam_host $(NAM_BUILD)/suprnam.so ttl/suprnam.ttl
	$(NAM_BUILD)/test_nam_models models

nam-install: nam
	cmake --install $(NAM_BUILD)

nam-clean:
	rm -rf $(NAM_BUILD)

.PHONY: all test demo tools install uninstall clean nam nam-test nam-install nam-clean

# Finite checks and deterministic feedback require strict floating point.
TIME_SPACE_CXXFLAGS = $(filter-out -ffast-math,$(CXXFLAGS)) -ffp-contract=off

build/suprecho.so: Makefile src/SuprEcho.cpp src/EchoDsp.h src/TimeSpaceDsp.h
	@mkdir -p build
	$(CXX) $(TIME_SPACE_CXXFLAGS) $(LV2_CFLAGS) -fPIC -shared -o $@ src/SuprEcho.cpp

build/test_echo: Makefile test/test_echo.cpp src/SuprEcho.cpp src/EchoDsp.h src/TimeSpaceDsp.h
	@mkdir -p build
	$(CXX) $(TIME_SPACE_CXXFLAGS) $(LV2_CFLAGS) -Isrc -o $@ test/test_echo.cpp src/SuprEcho.cpp

build/suprspace.so: Makefile src/SuprSpace.cpp src/SpaceDsp.h src/TimeSpaceDsp.h
	@mkdir -p build
	$(CXX) $(TIME_SPACE_CXXFLAGS) $(LV2_CFLAGS) -fPIC -shared -o $@ src/SuprSpace.cpp

build/test_space: Makefile test/test_space.cpp src/SuprSpace.cpp src/SpaceDsp.h src/TimeSpaceDsp.h
	@mkdir -p build
	$(CXX) $(TIME_SPACE_CXXFLAGS) $(LV2_CFLAGS) -Isrc -o $@ test/test_space.cpp src/SuprSpace.cpp

# Tone/motion: finite checks and reproducible sample-wise state.
TONE_MOTION_CXXFLAGS = $(filter-out -ffast-math,$(CXXFLAGS)) -ffp-contract=off





build/suprphase.so: Makefile src/SuprPhase.cpp src/PhaseDsp.h src/SansDsp.h src/OctaverDsp.h
	@mkdir -p build
	$(CXX) $(TONE_MOTION_CXXFLAGS) $(LV2_CFLAGS) -fPIC -shared -o $@ src/SuprPhase.cpp

build/test_phase: test/test_phase.cpp Makefile src/SuprPhase.cpp src/PhaseDsp.h src/SansDsp.h src/OctaverDsp.h
	@mkdir -p build
	$(CXX) $(TONE_MOTION_CXXFLAGS) $(LV2_CFLAGS) -Isrc -o $@ test/test_phase.cpp src/SuprPhase.cpp

build/test_band: Makefile test/test_band.cpp test/test_octaver.cpp src/SuprBand.cpp $(wildcard src/*.h)
	@mkdir -p build
	$(CXX) $(STRICT_DSP_CXXFLAGS) $(LV2_CFLAGS) -Isrc -o $@ test/test_band.cpp src/SuprBand.cpp

test: test-signal-integrity

test-signal-integrity: build/test_band

.PHONY: test-signal-integrity

build/test_clean: Makefile test/test_clean.cpp test/test_octaver.cpp src/SuprCompressor.cpp $(wildcard src/*.h)
	@mkdir -p build
	$(CXX) $(STRICT_DSP_CXXFLAGS) $(LV2_CFLAGS) -Isrc -o $@ test/test_clean.cpp src/SuprCompressor.cpp

test-signal-integrity: build/test_clean build/test_clack
	./build/test_band
	./build/test_clean
	./build/test_clack
	python3 test/validate_signal_integrity.py

build/test_clack: Makefile test/test_clack.cpp test/test_octaver.cpp src/SuprClack.cpp $(wildcard src/*.h)
	@mkdir -p build
	$(CXX) $(STRICT_DSP_CXXFLAGS) $(LV2_CFLAGS) -Isrc -o $@ test/test_clack.cpp src/SuprClack.cpp
