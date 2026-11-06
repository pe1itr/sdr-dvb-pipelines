DAB_DIR := components/dab
DAB_ARCH := linux_$(shell uname -m)
DAB_LIB_DIR := $(DAB_DIR)/lib/$(DAB_ARCH)
DAB_RADIO := $(shell pkg-config --exists fdk-aac alsa && test -f $(DAB_LIB_DIR)/libdabsdr.so.4.0.1 && echo 1)
DAB_NAMES := libdxdabsdr dab_detector decoder_backend candidate_fic fic_identity_accumulator channel_filter mot_slideshow dynamic_label
DAB_SHARED_DSP := $(BUILD_DIR)/components/dvbt2/dsp/resampler.o $(BUILD_DIR)/components/dvbt2/dsp/dsp_compat.o
DAB_FLAGS := -Icomponents/dvbt2/include -I$(DAB_DIR)/src -Isrc $(JSON_CFLAGS) $(shell pkg-config --cflags samplerate)
ifeq ($(DAB_RADIO),1)
DAB_NAMES += dab_ensemble dab_plus_radio
DAB_FLAGS += -DHAVE_DAB_RADIO -I$(DAB_LIB_DIR) $(shell pkg-config --cflags fdk-aac alsa)
DAB_LIBS := $(shell pkg-config --libs fdk-aac alsa) -L$(BUILD_DIR) -l:libdabsdr.so.4 -Wl,-rpath,'$$ORIGIN'
DAB_LIBRARY := $(BUILD_DIR)/libdabsdr.so.4
$(DAB_LIBRARY): $(DAB_LIB_DIR)/libdabsdr.so.4.0.1 | $(BUILD_DIR)
	cp $< $@
endif
DAB_OBJECTS := $(addprefix $(BUILD_DIR)/components/dab/,$(addsuffix .o,$(DAB_NAMES)))
.PHONY: FORCE_DAB_CONFIG
$(BUILD_DIR)/dab-config: FORCE_DAB_CONFIG | $(BUILD_DIR)
	@printf '%s\n' '$(DAB_FLAGS) $(DAB_RADIO) $(DAB_LIB_DIR)' > $@.tmp
	@cmp -s $@.tmp $@ || cp $@.tmp $@
$(BUILD_DIR)/components/dab/%.o: $(DAB_DIR)/src/%.cpp $(wildcard $(DAB_DIR)/src/*.hpp) $(BUILD_DIR)/dab-config
	mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DAB_FLAGS) -pthread -c $< -o $@
$(BUILD_DIR)/dab_worker: src/dab_worker.cpp $(DAB_SHARED_DSP) $(DAB_OBJECTS) $(DAB_LIBRARY) $(DAB_DIR)/channels.h src/dab_resampler.hpp $(BUILD_DIR)/dab-config | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DAB_FLAGS) $< $(DAB_OBJECTS) $(DAB_SHARED_DSP) $(DAB_LIBS) $(JSON_LIBS) $(shell pkg-config --libs samplerate) -pthread -o $@
all: $(BUILD_DIR)/dab_worker

DAB_TESTS := libdxdabsdr_tii_synthetic fic_identity_accumulator channel_filter mot_slideshow dynamic_label
ifeq ($(DAB_RADIO),1)
DAB_TESTS += dab_plus_radio
endif
$(BUILD_DIR)/dab_test_%: $(DAB_DIR)/tests/%_test.cpp $(DAB_OBJECTS) $(DAB_LIBRARY)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DAB_FLAGS) $< $(DAB_OBJECTS) $(DAB_LIBS) $(JSON_LIBS) $(shell pkg-config --libs samplerate) -pthread -o $@
.PHONY: test-dab
test-dab: $(BUILD_DIR)/test_dab_resampler $(BUILD_DIR)/linrad_dvb $(BUILD_DIR)/sdr_to_stdout $(BUILD_DIR)/test_b210_mixer $(BUILD_DIR)/dab_worker $(addprefix $(BUILD_DIR)/dab_test_,$(DAB_TESTS))
	@set -e; for t in $(DAB_TESTS); do ./$(BUILD_DIR)/dab_test_$$t; done
	./$(BUILD_DIR)/test_dab_resampler
	python3 tests/test_dab.py
	./$(BUILD_DIR)/test_b210_mixer

$(BUILD_DIR)/test_b210_mixer: tests/test_b210_mixer.c src/b210_mixer.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(shell pkg-config --cflags uhd) $< -o $@ $(shell pkg-config --libs uhd) -pthread -lm

$(BUILD_DIR)/test_dab_resampler: tests/test_dab_resampler.cpp src/dab_resampler.hpp $(BUILD_DIR)/components/dab/channel_filter.o $(DAB_SHARED_DSP)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -Icomponents/dvbt2/include -I$(DAB_DIR)/src -Isrc $(shell pkg-config --cflags samplerate) $< $(BUILD_DIR)/components/dab/channel_filter.o $(DAB_SHARED_DSP) $(shell pkg-config --libs samplerate) -o $@
