CXX := clang++
BREW_PREFIX ?= /opt/homebrew

CXXFLAGS := -std=c++17 -O0 -g -Wall -Wextra
CPPFLAGS := -I$(BREW_PREFIX)/include
LDFLAGS := -L$(BREW_PREFIX)/lib

MACOS_FRAMEWORKS := \
	-framework CoreAudio \
	-framework CoreMIDI \
	-framework CoreFoundation

COMMON_AUDIO_LIBS := -lportaudio -laubio -lrtmidi
STATIC_WEB_AUDIO_LIBS := \
	$(BREW_PREFIX)/opt/portaudio/lib/libportaudio.a \
	$(BREW_PREFIX)/opt/aubio/lib/libaubio.a \
	$(BREW_PREFIX)/opt/libsamplerate/lib/libsamplerate.a

PORTABLE_FRAMEWORKS := \
	$(MACOS_FRAMEWORKS) \
	-framework AudioToolbox \
	-framework AudioUnit \
	-framework CoreServices \
	-framework Accelerate

.PHONY: all clean dev-world dev-midi dev-web test-pitch test-spectrum test-polyphonic test-formants test-roundtrip-latency test-collier-effects test-collier-features test-vibrato-lock test-unvoiced-modes test-unvoiced-hold test-latency-control test-parallel-control test-parallel-matrix app-macos package-source release

all: harmonizer_world harmonizer harmonizer_web

harmonizer_world: harmonizer_world.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $< $(LDFLAGS) $(COMMON_AUDIO_LIBS) -lSDL2 $(MACOS_FRAMEWORKS) -o $@

harmonizer: harmonizer_midi.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $< $(LDFLAGS) $(COMMON_AUDIO_LIBS) -lrubberband -lpthread $(MACOS_FRAMEWORKS) -o $@

harmonizer_web: harmonizer_web.cpp harmonizer_rubberband_engine.hpp collier_effects.hpp backends/output_bridge_controller.hpp web/index.html
	$(CXX) $(CXXFLAGS) -O3 -DNDEBUG $(CPPFLAGS) harmonizer_web.cpp $(LDFLAGS) -lportaudio -laubio -lrubberband -lpthread -framework CoreAudio -framework CoreFoundation -o $@

harmonizer_web_portable: harmonizer_web.cpp harmonizer_rubberband_engine.hpp collier_effects.hpp backends/output_bridge_controller.hpp web/index.html
	rubberband_source="$$(./scripts/fetch_rubberband.sh)"; \
	$(CXX) $(CXXFLAGS) -O3 -DNDEBUG $(CPPFLAGS) -I"$$rubberband_source" \
		harmonizer_web.cpp "$$rubberband_source/single/RubberBandSingle.cpp" \
		$(STATIC_WEB_AUDIO_LIBS) -lpthread $(PORTABLE_FRAMEWORKS) -o $@

app-macos: harmonizer_web_portable
	./scripts/build_macos_app.sh

package-source:
	./scripts/package_source.sh

release: app-macos package-source

pitch_analyzer: pitch_analyzer.cpp
	$(CXX) -std=c++17 -O3 -DNDEBUG -Wall -Wextra $(CPPFLAGS) $< $(LDFLAGS) -laubio -o $@

spectrum_analyzer: spectrum_analyzer.cpp
	$(CXX) -std=c++17 -O3 -DNDEBUG -Wall -Wextra $< -o $@

polyphonic_analyzer: polyphonic_analyzer.cpp analysis_audio.hpp
	$(CXX) -std=c++17 -O3 -DNDEBUG -Wall -Wextra $< -o $@

formant_analyzer: formant_analyzer.cpp analysis_audio.hpp
	$(CXX) -std=c++17 -O3 -DNDEBUG -Wall -Wextra $< -o $@

latency_probe: latency_probe.cpp analysis_audio.hpp
	$(CXX) -std=c++17 -O3 -DNDEBUG -Wall -Wextra $(CPPFLAGS) $< $(LDFLAGS) -lportaudio $(PORTABLE_FRAMEWORKS) -o $@

watch_files_macos: scripts/watch_files_macos.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

dev-world: harmonizer_world
	./scripts/dev_harmonizer.sh harmonizer_world harmonizer_world.cpp

dev-midi: harmonizer
	./scripts/dev_harmonizer.sh harmonizer harmonizer_midi.cpp

dev-web: harmonizer_web
	./scripts/dev_harmonizer.sh harmonizer_web harmonizer_web.cpp harmonizer_rubberband_engine.hpp collier_effects.hpp web/index.html

test-pitch: pitch_analyzer
	./scripts/test_pitch_fixtures.sh

test-spectrum: spectrum_analyzer
	./spectrum_analyzer --help

test-polyphonic:
	./scripts/build_backend_lab.sh
	./scripts/test_polyphonic_chords.sh

test-formants:
	./scripts/build_backend_lab.sh
	./scripts/test_formant_preservation.sh

test-roundtrip-latency:
	./scripts/build_backend_lab.sh
	./scripts/test_roundtrip_latency.sh

test-collier-effects:
	./scripts/build_backend_lab.sh
	./build-backend-lab/collier_effects_test

test-collier-features:
	./scripts/build_backend_lab.sh
	./scripts/test_collier_features.sh

test-vibrato-lock: harmonizer_web pitch_analyzer
	./scripts/test_vibrato_lock.sh

test-unvoiced-modes: harmonizer_web
	./scripts/test_unvoiced_hold.sh

test-unvoiced-hold: test-unvoiced-modes

test-latency-control:
	./scripts/build_backend_lab.sh
	./build-backend-lab/output_bridge_controller_test

test-parallel-control:
	./scripts/build_backend_lab.sh
	./build-backend-lab/parallel_pitch_tracker_test
	./scripts/test_parallel_handoff.sh
	./scripts/test_low_pitch_harmonizer.sh
	HARMONIZER_TEST_BINARY=./build-backend-lab/harmonizer_web_parallel ./scripts/test_unvoiced_hold.sh

test-parallel-matrix:
	./scripts/build_backend_lab.sh
	./scripts/test_parallel_pitch_matrix.sh

clean:
	rm -f harmonizer harmonizer_world harmonizer_web harmonizer_web_portable pitch_analyzer spectrum_analyzer polyphonic_analyzer formant_analyzer latency_probe watch_files_macos
	rm -rf harmonizer.dSYM harmonizer_world.dSYM harmonizer_web.dSYM pitch_analyzer.dSYM
