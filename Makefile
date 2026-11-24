CC ?= cc
CXXFLAGS ?= -O3 -Wall -Wextra -std=gnu++17
CFLAGS ?= -O2 -Wall -Wextra -std=c11
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?=
.DEFAULT_GOAL := all

BUILD_DIR := build
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS := $(shell pkg-config --libs gtk+-3.0) -lutil
GTK_CFLAGS += $(shell pkg-config --cflags libvlc)
GTK_LIBS += $(shell pkg-config --libs libvlc)
USB_CFLAGS := $(shell pkg-config --cflags alsa samplerate)
USB_LIBS := $(shell pkg-config --libs alsa samplerate)
GTK_CFLAGS += $(USB_CFLAGS)
GTK_LIBS += $(USB_LIBS)
GLIB_CFLAGS := $(shell pkg-config --cflags glib-2.0)
FILTER_CFLAGS := $(shell pkg-config --cflags fftw3f)
FILTER_LIBS := $(shell pkg-config --libs fftw3f)

# Native Pluto RX uses the libiio 0.x API, matching the ATV receiver.
HAVE_LIBIIO := $(shell pkg-config --atleast-version=0.23 --max-version=0.99 libiio && echo 1)
ifeq ($(HAVE_LIBIIO),1)
PLUTO_CFLAGS := -DHAVE_LIBIIO $(shell pkg-config --cflags libiio)
PLUTO_LIBS := $(shell pkg-config --libs libiio)
endif

# Optional native LimeSDR Mini RX. Set PKG_CONFIG_PATH for a private LimeSuite.
HAVE_LIME := $(shell pkg-config --exists LimeSuite && echo 1)
ifeq ($(HAVE_LIME),1)
LIME_CFLAGS := -DHAVE_LIME $(shell pkg-config --cflags LimeSuite)
LIME_LIBS := $(shell pkg-config --libs LimeSuite) -Wl,-rpath,$(shell pkg-config --variable=libdir LimeSuite)
endif

# Rebuild the SDR helper when the optional backend or its location changes.
.PHONY: FORCE_LIME_CONFIG
$(BUILD_DIR)/lime-config: FORCE_LIME_CONFIG | $(BUILD_DIR)
	@printf '%s\n' '$(LIME_CFLAGS) $(LIME_LIBS) $(PLUTO_CFLAGS) $(PLUTO_LIBS)' > $@.tmp
	@cmp -s $@.tmp $@ || cp $@.tmp $@

GLIB_LIBS := $(shell pkg-config --libs glib-2.0)
JSON_CFLAGS := $(shell pkg-config --cflags json-c)
JSON_LIBS := $(shell pkg-config --libs json-c)


include components/dab/component.mk

# Pinned LeanDVB C++ backend; all GUI code remains compiled as C.
LEAN_DIR := third_party/leansdr
LEAN_FLAGS := -I$(LEAN_DIR)/src -Isrc -DLEANSDR_EXTENSIONS -DVERSION=\"linrad-leansdr-84c59e1\" -Wno-sign-compare -Wno-unused-variable -Wno-unused-parameter -Wno-missing-field-initializers
LEAN_OBJECTS := $(BUILD_DIR)/leansdr/leandvb.o $(BUILD_DIR)/leansdr/backend.o $(BUILD_DIR)/leansdr/ldpc_pool.o
$(BUILD_DIR)/leansdr/leandvb.o: $(LEAN_DIR)/src/apps/leandvb.cc $(wildcard $(LEAN_DIR)/src/leansdr/*.h) src/leansdr_monitor.hh src/decoder_monitor.h src/ldpc_pool.h src/leansdr_backend.h
	mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LEAN_FLAGS) -pthread -c $< -o $@
$(BUILD_DIR)/leansdr/backend.o: src/leansdr_backend.cc src/leansdr_backend.h
	mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
$(BUILD_DIR)/leansdr/ldpc_pool.o: src/ldpc_pool.cc src/ldpc_pool.h
	mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -pthread -c $< -o $@
LDPC_DIR := third_party/ldpc
$(BUILD_DIR)/ldpc_tables.o: $(LDPC_DIR)/tables_handler.cc $(wildcard $(LDPC_DIR)/*.hh) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -Wno-unused-parameter -c $< -o $@
$(BUILD_DIR)/ldpc_tool: $(LDPC_DIR)/ldpc_tool.cc $(wildcard $(LDPC_DIR)/*.hh) $(BUILD_DIR)/ldpc_tables.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -Wno-unused-variable -Wno-unused-parameter $< $(BUILD_DIR)/ldpc_tables.o -o $@ $(LDFLAGS)
ifeq ($(shell $(CXX) -dumpmachine | cut -d- -f1),x86_64)
LDPC_FAST := $(BUILD_DIR)/ldpc_tool_avx2
$(BUILD_DIR)/ldpc_tool_avx2: $(LDPC_DIR)/ldpc_tool.cc $(wildcard $(LDPC_DIR)/*.hh) $(BUILD_DIR)/ldpc_tables.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -mavx2 -Wno-unused-variable -Wno-unused-parameter $< $(BUILD_DIR)/ldpc_tables.o -o $@ $(LDFLAGS)
endif
all: $(BUILD_DIR)/ldpc_tool $(LDPC_FAST)

# Project-owned DVB-T receiver; developed here and compiled into linrad_dvb.
# Keep its core separate from the host adapter in src/dvbt_backend.c.
DVBT_DIR := components/dvbt
DVBT_SOURCES := $(filter-out $(DVBT_DIR)/src/main.c,$(wildcard $(DVBT_DIR)/src/*.c))
DVBT_OBJECTS := $(patsubst $(DVBT_DIR)/src/%.c,$(BUILD_DIR)/components/dvbt/%.o,$(DVBT_SOURCES)) $(BUILD_DIR)/components/dvbt/backend.o
DVBT_CFLAGS := -I$(DVBT_DIR)/include -Isrc $(FILTER_CFLAGS) -DLINRAD_EMBEDDED_DVBT -DRBDVBT_VERSION=\"0.1.5\"
include components/dvbt2/component.mk

# Optional S2 GPU executor; one persistent GPU context with production CPU retry.
ifneq ($(OS),Windows_NT)
all: $(BUILD_DIR)/ldpc_tool_gpu
$(BUILD_DIR)/ldpc_tool_gpu: src/dvbs2_gpu_helper.cpp src/ldpc_pool.h $(DVBT2_OBJECTS) $(DVBT2_RUST) $(BUILD_DIR)/leansdr/ldpc_pool.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DVBT2_FLAGS) $< $(DVBT2_OBJECTS) $(DVBT2_RUST) $(BUILD_DIR)/leansdr/ldpc_pool.o -o $@ $(LDFLAGS) -ldl -lm $(LDLIBS)
$(BUILD_DIR)/test_dvbs2_gpu_device_failure: src/dvbs2_gpu_helper.cpp tests/dvbs2_gpu_device_failure.cpp $(filter-out $(BUILD_DIR)/components/dvbt2/dsp/gpu_ldpc_stub.o $(BUILD_DIR)/components/dvbt2/dsp/gpu_ldpc_d3d11.o $(BUILD_DIR)/components/dvbt2/dsp/gpu_ldpc_vulkan.o,$(DVBT2_OBJECTS)) $(DVBT2_RUST) $(BUILD_DIR)/leansdr/ldpc_pool.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DVBT2_FLAGS) $^ -o $@ $(LDFLAGS) -ldl -lm $(LDLIBS)
endif

# Project-owned TSscatterFix recovery library; RGB callbacks are displayed by GTK.
SCATTER_DIR := components/tsscatterfix
SCATTER_SOURCES := $(addprefix $(SCATTER_DIR)/src/,recovery.c media.c rank_model.c psi.c ts_packet.c)
SCATTER_OBJECTS := $(patsubst $(SCATTER_DIR)/src/%.c,$(BUILD_DIR)/components/tsscatterfix/%.o,$(SCATTER_SOURCES))
SCATTER_CFLAGS := -I$(SCATTER_DIR)/src $(shell pkg-config --cflags libavcodec libavformat libavutil libswscale)
TELETEXT_CFLAGS ?= $(shell pkg-config --cflags zvbi-0.2)
TELETEXT_LIBS ?= $(shell pkg-config --libs zvbi-0.2)
SCATTER_CFLAGS += $(TELETEXT_CFLAGS)
SCATTER_LIBS := $(shell pkg-config $(FFMPEG_PKG_CONFIG_FLAGS) --libs libavformat libavcodec libswscale libavutil) $(TELETEXT_LIBS)

$(BUILD_DIR)/components/tsscatterfix/%.o: $(SCATTER_DIR)/src/%.c $(wildcard $(SCATTER_DIR)/src/*.h)
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SCATTER_CFLAGS) -c $< -o $@

MOON_OBJECTS := $(BUILD_DIR)/moon.o $(BUILD_DIR)/astronomy.o
$(BUILD_DIR)/moon.o: components/moon/moon.c components/moon/moon.h src/maidenhead.h src/iq_shift.h third_party/astronomy/astronomy.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(GLIB_CFLAGS) -c $< -o $@
$(BUILD_DIR)/astronomy.o: third_party/astronomy/astronomy.c third_party/astronomy/astronomy.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

GUI_SOURCES := $(MOON_OBJECTS) src/iq_file.c src/decoder_view.c src/video_view.c src/dsmcc_output.c src/scatter_backend.c src/scatter_view.c src/ts_info.c src/ts_info_view.c src/usb_receiver.c src/usb_demod.c src/usb_tci.c
GUI_HEADERS := src/dsmcc_output.h src/usb_receiver.h src/usb_demod.h src/usb_tci.h src/usb_tap.h
GUI_HEADERS += src/lime_rx.h src/rx_limits.h
BOND_SOURCES := $(wildcard components/bonding/src/*.c)
GUI_SOURCES += src/dvbt_bonding.c $(BOND_SOURCES)
GUI_HEADERS += src/dvbt_bonding.h $(wildcard components/bonding/include/*.h components/bonding/src/*.h)
GTK_CFLAGS += -Icomponents/bonding/include
GUI_SOURCES += src/status_http.c src/program_view.c src/srt_output.c src/dab_view.c
GUI_HEADERS += src/sdr_rx_status.h src/status_http.h src/program_view.h src/srt_output.h src/dab_view.h components/dab/channels.h components/dab/country.h
GUI_HEADERS += src/decoder_margin.h src/hackrf_gain.h
GUI_HEADERS += src/signal_meter.h
GUI_HEADERS += src/reception_log.h src/maidenhead.h
GUI_HEADERS += $(BUILD_DIR)/generated/linrad_build_info.h $(BUILD_DIR)/generated/api_docs.h
GTK_CFLAGS += -I$(BUILD_DIR)/generated
GUI_HEADERS += src/leansdr_backend.h src/decoder_view.h src/decoder_monitor.h src/dvbt_backend.h src/dvbt2_backend.h src/video_view.h src/scatter_view.h src/scatter_backend.h src/ts_info.h src/ts_info_view.h

$(BUILD_DIR)/components/dvbt/%.o: $(DVBT_DIR)/src/%.c $(wildcard $(DVBT_DIR)/include/*.h) src/dvbt_backend.h src/decoder_monitor.h
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -O3 $(DVBT_CFLAGS) -pthread -c $< -o $@

$(BUILD_DIR)/components/dvbt/backend.o: src/dvbt_backend.c src/dvbt_backend.h src/decoder_monitor.h $(wildcard $(DVBT_DIR)/include/*.h)
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DVBT_CFLAGS) -pthread -c $< -o $@

.PHONY: all package dist package-windows test clean

$(BUILD_DIR)/generated/api_docs.h: tools/build-api-docs.py docs/api/reference.html docs/api/openapi.json
	python3 tools/build-api-docs.py $@

.PHONY: build-info-force
$(BUILD_DIR)/generated/linrad_build_info.h: build-info-force VERSION tools/build-info.sh
	bash tools/build-info.sh $@

.PHONY: test-build-info
test-build-info:
	python3 tests/test_build_info.py
test: test-build-info

package-windows:
	python3 tools/build-windows-pluto.py
	python3 tools/build-windows-lime.py
	python3 tools/build-windows.py
	python3 dev/package_windows.py

dist:
	python3 dev/dist.py

all: $(BUILD_DIR)/linrad_dvb $(BUILD_DIR)/linrad_udp_to_stdout $(BUILD_DIR)/sdr_to_stdout $(BUILD_DIR)/iq_file_to_stdout

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/linrad_dvb: src/linrad_dvb.c $(BUILD_DIR)/dab_worker $(GUI_SOURCES) $(GUI_HEADERS) $(LEAN_OBJECTS) $(DVBT_OBJECTS) $(DVBT2_LINK) $(SCATTER_OBJECTS) src/iq_file.h src/iq_shift.h src/sdriq.h src/rtlsdr.h src/funcube.h src/gui_theme.h src/iq_view.h src/iq_monitor.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DVBT_CFLAGS) $(SCATTER_CFLAGS) $(GTK_CFLAGS) $(JSON_CFLAGS) $< $(GUI_SOURCES) $(LEAN_OBJECTS) $(DVBT_OBJECTS) $(DVBT2_LINK) $(SCATTER_OBJECTS) $(SCATTER_LIBS) -o $@ $(LDFLAGS) $(GTK_LIBS) $(JSON_LIBS) $(FILTER_LIBS) -pthread -lm -ldl -lstdc++ $(LDLIBS)

$(BUILD_DIR)/iq_file_to_stdout: src/iq_file_to_stdout.c $(MOON_OBJECTS) src/iq_shift.h src/iq_file.c src/iq_file.h src/iq_bandfilter.c src/iq_bandfilter.h src/iq_monitor.c src/usb_tap.c src/usb_tap.h src/iq_monitor.h src/tune_control.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(GLIB_CFLAGS) $(JSON_CFLAGS) $(FILTER_CFLAGS) $< $(MOON_OBJECTS) src/iq_file.c src/iq_bandfilter.c src/iq_monitor.c src/usb_tap.c -o $@ $(LDFLAGS) $(GLIB_LIBS) $(JSON_LIBS) $(FILTER_LIBS) -pthread -lm $(LDLIBS)

$(BUILD_DIR)/linrad_udp_to_stdout: src/linrad_udp_to_stdout.c $(MOON_OBJECTS) src/iq_monitor.c src/usb_tap.c src/usb_tap.h src/iq_monitor.h src/iq_bandfilter.c src/iq_bandfilter.h src/tune_control.h src/iq_record.c src/iq_record.h src/iq_output.c src/iq_output.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(GLIB_CFLAGS) $(FILTER_CFLAGS) $< $(MOON_OBJECTS) src/iq_bandfilter.c src/iq_monitor.c src/usb_tap.c src/iq_record.c src/iq_output.c -o $@ $(LDFLAGS) $(GLIB_LIBS) $(FILTER_LIBS) -pthread -lm $(LDLIBS)

package: all
	cp LICENSE $(BUILD_DIR)/LICENSE
	cp NOTICE $(BUILD_DIR)/NOTICE
	mkdir -p $(BUILD_DIR)/docs/licenses
	cp docs/credits.md $(BUILD_DIR)/docs/credits.md
	cp docs/licenses/MIT-legacy.txt docs/licenses/Astronomy-Engine-MIT.txt $(BUILD_DIR)/docs/licenses/
	cp $(LEAN_DIR)/LICENSE.txt $(BUILD_DIR)/leansdr-LICENSE.txt
	cp $(LDPC_DIR)/LICENSE $(BUILD_DIR)/ldpc-LICENSE.txt
	cp $(DVBT_DIR)/LICENSE $(BUILD_DIR)/rbdvbt-LICENSE.txt
	cp $(DVBT2_DIR)/LICENSE $(BUILD_DIR)/dvbt2-LICENSE.txt
	cp $(SCATTER_DIR)/LICENSE $(BUILD_DIR)/tsscatterfix-LICENSE.txt
	cp components/bonding/LICENSE $(BUILD_DIR)/bonding-LICENSE.txt
	@echo "Start: ./build/linrad_dvb (config: linrad-dvb.ini)"

$(BUILD_DIR)/gui_smoke: tests/gui_smoke.c tests/sdr_status_endpoint_test.h src/linrad_dvb.c $(GUI_SOURCES) $(GUI_HEADERS) $(LEAN_OBJECTS) $(DVBT_OBJECTS) $(DVBT2_LINK) $(SCATTER_OBJECTS) src/iq_file.h src/iq_shift.h src/sdriq.h src/rtlsdr.h src/funcube.h src/gui_theme.h src/iq_view.h src/iq_monitor.h src/tune_control.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DVBT_CFLAGS) $(SCATTER_CFLAGS) $(GTK_CFLAGS) $(JSON_CFLAGS) $< $(GUI_SOURCES) $(LEAN_OBJECTS) $(DVBT_OBJECTS) $(DVBT2_LINK) $(SCATTER_OBJECTS) $(SCATTER_LIBS) -o $@ $(LDFLAGS) $(GTK_LIBS) $(JSON_LIBS) $(FILTER_LIBS) -pthread -lm -ldl -lstdc++ $(LDLIBS)

$(BUILD_DIR)/dab_gui_smoke: tests/dab_gui_smoke.c src/linrad_dvb.c $(GUI_SOURCES) $(GUI_HEADERS) $(LEAN_OBJECTS) $(DVBT_OBJECTS) $(DVBT2_LINK) $(SCATTER_OBJECTS) src/iq_file.h src/iq_shift.h src/sdriq.h src/rtlsdr.h src/funcube.h src/gui_theme.h src/iq_view.h src/iq_monitor.h src/tune_control.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DVBT_CFLAGS) $(SCATTER_CFLAGS) $(GTK_CFLAGS) $(JSON_CFLAGS) $< $(GUI_SOURCES) $(LEAN_OBJECTS) $(DVBT_OBJECTS) $(DVBT2_LINK) $(SCATTER_OBJECTS) $(SCATTER_LIBS) -o $@ $(LDFLAGS) $(GTK_LIBS) $(JSON_LIBS) $(FILTER_LIBS) -pthread -lm -ldl -lstdc++ $(LDLIBS)

$(BUILD_DIR)/video_smoke: tests/video_smoke.c src/video_view.c src/video_view.h src/dsmcc_output.c src/scatter_backend.c src/scatter_backend.h src/ts_info.c src/ts_info.h $(SCATTER_OBJECTS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc $(GTK_CFLAGS) $(SCATTER_CFLAGS) tests/video_smoke.c src/video_view.c src/dsmcc_output.c src/scatter_backend.c src/ts_info.c $(SCATTER_OBJECTS) -o $@ $(LDFLAGS) -Wl,--wrap=libvlc_audio_set_volume $(GTK_LIBS) $(SCATTER_LIBS) -pthread -lm $(LDLIBS)

test: all $(BUILD_DIR)/gui_smoke $(BUILD_DIR)/dab_gui_smoke $(BUILD_DIR)/video_smoke
	python3 tests/test_linrad.py
	python3 tests/test_soapy_inputs.py

.PHONY: test-reception-log
test-reception-log:
	python3 tests/test_reception_log.py
test: test-reception-log

$(BUILD_DIR)/bearing_test: tests/bearing_test.c src/maidenhead.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(GTK_CFLAGS) $< -o $@ $(LDFLAGS) $(GTK_LIBS) -lm $(LDLIBS)

.PHONY: test-bearing
test-bearing: $(BUILD_DIR)/bearing_test
	$(BUILD_DIR)/bearing_test
test: test-bearing

clean:
	rm -rf $(BUILD_DIR)

SDR_SOURCES := src/sdr_to_stdout.c $(MOON_OBJECTS) src/pluto_rx.c src/funcube.c src/afedri.c src/lime_rx.c src/iq_output.c src/iq_record.c src/iq_bandfilter.c src/iq_monitor.c src/usb_tap.c
$(BUILD_DIR)/sdr_to_stdout: $(SDR_SOURCES) src/hackrf_gain.h src/sdr_rx_status.h src/pluto_rx.h src/snapshot_file.h src/b210_mixer.h components/dab/channels.h src/rx_limits.h src/afedri.h src/lime_rx.h src/usb_tap.h src/iq_monitor.h src/iq_bandfilter.h src/iq_output.h src/iq_convert.h src/iq_shift.h src/sdriq.h src/rtlsdr.h src/funcube.h src/tune_control.h src/iq_record.h $(BUILD_DIR)/lime-config | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(GLIB_CFLAGS) $(PLUTO_CFLAGS) $(LIME_CFLAGS) $(FILTER_CFLAGS) $(shell pkg-config --cflags SoapySDR uhd alsa) $(SDR_SOURCES) -pthread -o $@ $(LDFLAGS) $(GLIB_LIBS) $(shell pkg-config --libs SoapySDR uhd alsa) $(PLUTO_LIBS) $(LIME_LIBS) $(FILTER_LIBS) -pthread -lm $(LDLIBS)

$(BUILD_DIR)/test_iq_output: tests/test_iq_output.c src/iq_output.c src/iq_output.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_iq_output.c src/iq_output.c -o $@ $(LDFLAGS) -pthread $(LDLIBS)

.PHONY: test-iq-output
$(BUILD_DIR)/test_iq_output_backpressure: tests/test_iq_output_backpressure.c src/iq_output.c src/iq_output.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDFLAGS) -pthread $(LDLIBS)

test-iq-output: $(BUILD_DIR)/test_iq_output $(BUILD_DIR)/test_iq_output_backpressure
	./$(BUILD_DIR)/test_iq_output
	./$(BUILD_DIR)/test_iq_output_backpressure

test: test-iq-output

$(BUILD_DIR)/test_iq_record: tests/test_iq_record.c src/iq_record.c src/iq_record.h src/iq_output.c src/iq_output.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< src/iq_record.c src/iq_output.c -o $@ $(LDFLAGS) -pthread -lm $(LDLIBS)

.PHONY: test-iq-record
test-iq-record: $(BUILD_DIR)/test_iq_record
	./$(BUILD_DIR)/test_iq_record

test: test-iq-record

$(BUILD_DIR)/iq_conversion_probe: tests/iq_conversion_probe.c src/iq_convert.h src/iq_shift.h src/sdriq.h src/tune_control.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDFLAGS) -lm $(LDLIBS)

.PHONY: test-iq-conversion
test-iq-conversion: $(BUILD_DIR)/iq_conversion_probe
	python3 tests/test_iq_conversion.py

test: test-iq-conversion

.PHONY: test-lime
ifeq ($(HAVE_LIME),1)
$(BUILD_DIR)/test_lime_rx: tests/test_lime_rx.c src/lime_rx.c src/lime_rx.h $(BUILD_DIR)/lime-config | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LIME_CFLAGS) tests/test_lime_rx.c src/lime_rx.c -o $@ $(LDFLAGS) -lm $(LDLIBS)
test-lime: $(BUILD_DIR)/test_lime_rx
	./$(BUILD_DIR)/test_lime_rx
else
test-lime:
	@echo "Skipping native Lime RX tests: LimeSuite development files unavailable"
endif
test: test-lime

$(BUILD_DIR)/test_tune_control: tests/test_tune_control.c src/tune_control.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDFLAGS) -lm $(LDLIBS)

.PHONY: test-tune-control
test-tune-control: $(BUILD_DIR)/test_tune_control
	./$(BUILD_DIR)/test_tune_control

test: test-tune-control

$(BUILD_DIR)/bandfilter_probe: tests/bandfilter_probe.c src/iq_bandfilter.c src/iq_bandfilter.h src/iq_monitor.c src/iq_monitor.h src/usb_tap.c src/usb_tap.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(FILTER_CFLAGS) tests/bandfilter_probe.c src/iq_bandfilter.c src/iq_monitor.c src/usb_tap.c -o $@ $(LDFLAGS) $(FILTER_LIBS) -pthread -lm $(LDLIBS)

.PHONY: test-bandfilter
test-bandfilter: $(BUILD_DIR)/bandfilter_probe
	python3 tests/test_bandfilter.py

test: test-bandfilter

$(BUILD_DIR)/iq_shift_probe: tests/iq_shift_probe.c src/iq_shift.h src/iq_bandfilter.c src/iq_bandfilter.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(FILTER_CFLAGS) $< src/iq_bandfilter.c -o $@ $(LDFLAGS) $(FILTER_LIBS) -lm $(LDLIBS)

.PHONY: test-iq-shift
test-iq-shift: $(BUILD_DIR)/iq_shift_probe
	python3 tests/test_iq_shift.py

test: test-iq-shift

$(BUILD_DIR)/monitor_probe: tests/monitor_probe.c src/iq_monitor.c src/usb_tap.c src/usb_tap.h src/iq_monitor.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(FILTER_CFLAGS) tests/monitor_probe.c src/iq_monitor.c src/usb_tap.c -o $@ $(LDFLAGS) $(FILTER_LIBS) -pthread -lm $(LDLIBS)

.PHONY: test-iq-monitor
test-iq-monitor: $(BUILD_DIR)/monitor_probe $(BUILD_DIR)/test_iq_monitor_async
	python3 tests/test_iq_monitor.py
	./$(BUILD_DIR)/test_iq_monitor_async

$(BUILD_DIR)/test_iq_monitor_async: tests/test_iq_monitor_async.c src/iq_monitor.c src/iq_monitor.h src/usb_tap.c src/usb_tap.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(FILTER_CFLAGS) $< src/usb_tap.c -o $@ $(LDFLAGS) $(FILTER_LIBS) -pthread -lm $(LDLIBS)

$(BUILD_DIR)/test_sdr_rx_status: tests/test_sdr_rx_status.c src/sdr_rx_status.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(GLIB_CFLAGS) $< -o $@ $(LDFLAGS) $(GLIB_LIBS) -lm $(LDLIBS)

.PHONY: test-sdr-rx-status
test-sdr-rx-status: $(BUILD_DIR)/test_sdr_rx_status $(BUILD_DIR)/gui_smoke
	./$(BUILD_DIR)/test_sdr_rx_status
	./$(BUILD_DIR)/gui_smoke --test-sdr-status

test: test-sdr-rx-status

test: test-iq-monitor

.PHONY: test-file-replay
test-file-replay: all
	python3 tests/test_file_replay.py

test: test-file-replay test-moon

.PHONY: test-moon
test-moon: $(BUILD_DIR)/iq_file_to_stdout $(BUILD_DIR)/linrad_dvb
	python3 tests/test_moon.py

$(BUILD_DIR)/test_dvbt_stream: tests/test_dvbt_stream.c $(DVBT_DIR)/src/probe_constellation.c $(DVBT_OBJECTS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -O3 $(DVBT_CFLAGS) $< $(filter-out $(BUILD_DIR)/components/dvbt/probe_constellation.o,$(DVBT_OBJECTS)) -o $@ $(LDFLAGS) $(FILTER_LIBS) -pthread -lm $(LDLIBS)

$(BUILD_DIR)/test_dvbt_stream_scalar: tests/test_dvbt_stream.c $(DVBT_DIR)/src/probe_constellation.c $(DVBT_OBJECTS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -O3 $(DVBT_CFLAGS) -DRBDVBT_VITERBI_SCALAR_ONLY $< $(filter-out $(BUILD_DIR)/components/dvbt/probe_constellation.o,$(DVBT_OBJECTS)) -o $@ $(LDFLAGS) $(FILTER_LIBS) -pthread -lm $(LDLIBS)

ifneq ($(filter aarch64%,$(shell $(CC) -dumpmachine)),)
DVBT_NEON_TEST := $(BUILD_DIR)/test_viterbi_neon
$(BUILD_DIR)/test_viterbi_neon: tests/test_viterbi_neon.c $(DVBT_DIR)/include/viterbi_neon.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -O3 $< -o $@ $(LDFLAGS)
endif

$(BUILD_DIR)/test_dvbt_frontend: tests/test_dvbt_frontend.c $(DVBT_DIR)/src/probe_constellation.c $(DVBT_OBJECTS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -O3 $(DVBT_CFLAGS) $< $(filter-out $(BUILD_DIR)/components/dvbt/probe_constellation.o,$(DVBT_OBJECTS)) -o $@ $(LDFLAGS) $(FILTER_LIBS) -pthread -lm $(LDLIBS)

.PHONY: test-dvbt
$(BUILD_DIR)/test_dvbt_psi: tests/test_dvbt_psi.c $(DVBT_DIR)/src/dvbt_outer.c $(DVBT_OBJECTS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -O3 $(DVBT_CFLAGS) $< $(filter-out $(BUILD_DIR)/components/dvbt/dvbt_outer.o,$(DVBT_OBJECTS)) -o $@ $(LDFLAGS) $(FILTER_LIBS) -pthread -lm $(LDLIBS)

test-dvbt: $(BUILD_DIR)/test_dvbt_stream $(BUILD_DIR)/test_dvbt_stream_scalar $(BUILD_DIR)/test_dvbt_psi $(BUILD_DIR)/test_dvbt_frontend $(DVBT_NEON_TEST)
	./$(BUILD_DIR)/test_dvbt_stream
	LINRAD_DVBT_VITERBI_SCALAR=1 ./$(BUILD_DIR)/test_dvbt_stream
	./$(BUILD_DIR)/test_dvbt_stream_scalar
	./$(BUILD_DIR)/test_dvbt_psi
	./$(BUILD_DIR)/test_dvbt_frontend
	$(if $(DVBT_NEON_TEST),./$(DVBT_NEON_TEST))

test: test-dvbt

$(BUILD_DIR)/scatter_probe: tests/scatter_probe.c src/dsmcc_output.c src/scatter_backend.c src/scatter_backend.h src/ts_info.c src/ts_info.h $(SCATTER_OBJECTS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc $(GLIB_CFLAGS) $(SCATTER_CFLAGS) tests/scatter_probe.c src/dsmcc_output.c src/scatter_backend.c src/ts_info.c $(SCATTER_OBJECTS) -o $@ $(LDFLAGS) $(GLIB_LIBS) $(SCATTER_LIBS) -pthread -lm $(LDLIBS)

.PHONY: test-scatter
test-scatter: $(BUILD_DIR)/scatter_probe
	python3 tests/test_scatter.py

test: test-scatter

$(BUILD_DIR)/ts_info_probe: tests/ts_info_probe.c src/ts_info.c src/ts_info.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc $(GLIB_CFLAGS) $(JSON_CFLAGS) $(SCATTER_CFLAGS) tests/ts_info_probe.c src/ts_info.c -o $@ $(LDFLAGS) $(GLIB_LIBS) $(JSON_LIBS) $(SCATTER_LIBS) -pthread $(LDLIBS)

.PHONY: test-ts-info
test-ts-info: $(BUILD_DIR)/ts_info_probe
	python3 tests/test_ts_info.py

test: test-ts-info

$(BUILD_DIR)/ts_info_relay: tests/ts_info_relay.c src/dsmcc_output.c src/scatter_backend.c src/scatter_backend.h src/ts_info.c src/ts_info.h $(SCATTER_OBJECTS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc $(GLIB_CFLAGS) $(SCATTER_CFLAGS) tests/ts_info_relay.c src/dsmcc_output.c src/scatter_backend.c src/ts_info.c $(SCATTER_OBJECTS) -o $@ $(LDFLAGS) $(GLIB_LIBS) $(SCATTER_LIBS) -pthread -lm $(LDLIBS)

test-ts-info: $(BUILD_DIR)/ts_info_relay

$(BUILD_DIR)/usb_demod_probe: tests/usb_demod_probe.c src/usb_demod.c src/usb_demod.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc $(USB_CFLAGS) tests/usb_demod_probe.c src/usb_demod.c -o $@ $(LDFLAGS) $(USB_LIBS) -lm $(LDLIBS)

$(BUILD_DIR)/usb_receiver_probe: tests/usb_receiver_probe.c src/usb_receiver.c src/usb_demod.c src/usb_tci.c src/usb_tap.c src/usb_receiver.h src/usb_demod.h src/usb_tci.h src/usb_tap.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc $(GLIB_CFLAGS) $(USB_CFLAGS) tests/usb_receiver_probe.c src/usb_receiver.c src/usb_demod.c src/usb_tci.c src/usb_tap.c -o $@ $(LDFLAGS) $(GLIB_LIBS) $(USB_LIBS) -pthread -lm $(LDLIBS)

.PHONY: test-usb
test-usb: all $(BUILD_DIR)/usb_demod_probe $(BUILD_DIR)/usb_receiver_probe
	python3 tests/test_usb.py

test: test-usb

.PHONY: test-afedri
test-afedri: all $(BUILD_DIR)/afedri_audio_probe
	python3 tests/test_afedri.py

test: test-afedri

$(BUILD_DIR)/afedri_audio_probe: tests/afedri_audio_probe.c src/usb_receiver.c src/usb_demod.c src/usb_tci.c src/usb_receiver.h src/usb_demod.h src/usb_tci.h src/usb_tap.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc $(GLIB_CFLAGS) $(USB_CFLAGS) tests/afedri_audio_probe.c src/usb_receiver.c src/usb_demod.c src/usb_tci.c -o $@ $(LDFLAGS) $(GLIB_LIBS) $(USB_LIBS) -pthread -lm $(LDLIBS)

# Deterministic baseband fixtures; these tools never access SDR hardware.
$(BUILD_DIR)/leandvbtx: $(LEAN_DIR)/src/apps/leandvbtx.cc $(wildcard $(LEAN_DIR)/src/leansdr/*.h) tests/leansdr_test_hooks.cc | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LEAN_FLAGS) $< tests/leansdr_test_hooks.cc -o $@ $(LDFLAGS)

.PHONY: test-leansdr
test-leansdr: all $(BUILD_DIR)/leandvbtx
	python3 tests/test_leansdr.py

test: test-leansdr

$(BUILD_DIR)/test_bch: tests/test_bch.cc tests/leansdr_test_hooks.cc $(wildcard $(LEAN_DIR)/src/leansdr/*.h) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LEAN_FLAGS) tests/test_bch.cc tests/leansdr_test_hooks.cc -o $@ $(LDFLAGS)

.PHONY: test-bch
test-bch: $(BUILD_DIR)/test_bch
	./$(BUILD_DIR)/test_bch

test: test-bch

$(BUILD_DIR)/ldpc_fixture: tests/ldpc_fixture.cc $(LDPC_DIR)/ldpc_tool.cc $(wildcard $(LDPC_DIR)/*.hh) $(BUILD_DIR)/ldpc_tables.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -Wno-unused-variable -Wno-unused-parameter -I$(LDPC_DIR) $< $(BUILD_DIR)/ldpc_tables.o -o $@ $(LDFLAGS)

ifeq ($(shell $(CXX) -dumpmachine | cut -d- -f1),x86_64)
LDPC_TEST_FAST := $(BUILD_DIR)/ldpc_fixture_avx2
$(BUILD_DIR)/ldpc_fixture_avx2: tests/ldpc_fixture.cc $(LDPC_DIR)/ldpc_tool.cc $(wildcard $(LDPC_DIR)/*.hh) $(BUILD_DIR)/ldpc_tables.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -mavx2 -Wno-unused-variable -Wno-unused-parameter -I$(LDPC_DIR) $< $(BUILD_DIR)/ldpc_tables.o -o $@ $(LDFLAGS)
endif

.PHONY: test-ldpc
test-ldpc: $(BUILD_DIR)/ldpc_tool $(LDPC_FAST) $(BUILD_DIR)/ldpc_fixture $(LDPC_TEST_FAST)
	python3 tests/test_ldpc_batches.py

test: test-ldpc

.PHONY: test-build-helper
test-build-helper:
	python3 tests/test_build_helper.py
	python3 tests/test_odroid_build.py

test: test-build-helper

.PHONY: test-srt
test-srt: all
	python3 tests/test_srt_output.py

test: test-srt

test: test-dab

ifeq ($(HAVE_LIBIIO),1)
$(BUILD_DIR)/test_pluto_rx: tests/test_pluto_rx.c src/pluto_rx.c src/pluto_rx.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PLUTO_CFLAGS) -Isrc tests/test_pluto_rx.c src/pluto_rx.c -o $@ $(LDFLAGS) -lm $(LDLIBS)
.PHONY: test-pluto
test-pluto: $(BUILD_DIR)/test_pluto_rx
	./$(BUILD_DIR)/test_pluto_rx
endif

.PHONY: test-status-http
test-status-http: $(BUILD_DIR)/linrad_dvb $(BUILD_DIR)/linrad_udp_to_stdout
	python3 tests/test_status_http.py
test: test-status-http

# Experimental shared-SDR headless DVB-T repeater (Linux supervisor).
$(BUILD_DIR)/test_dvbt_multichannel: tests/test_dvbt_multichannel.c $(DVBT_DIR)/src/probe_constellation.c $(DVBT_OBJECTS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -O3 $(DVBT_CFLAGS) $< $(filter-out $(BUILD_DIR)/components/dvbt/probe_constellation.o,$(DVBT_OBJECTS)) -o $@ $(LDFLAGS) $(FILTER_LIBS) -pthread -lm $(LDLIBS)

.PHONY: test-dvbt-multirx
test-dvbt-multirx: $(BUILD_DIR)/test_dvbt_multichannel $(BUILD_DIR)/linrad_dvb
	./$(BUILD_DIR)/test_dvbt_multichannel
	python3 tests/test_dvbt_multirx.py

test: test-dvbt-multirx

BOND_HEADERS := $(wildcard components/bonding/include/*.h components/bonding/src/*.h)
BOND_TESTS := $(addprefix $(BUILD_DIR)/bonding/,test_bonding test_discovery test_ts_components test_native_av test_abi_v1)

$(BUILD_DIR)/bonding/test_%: components/bonding/tests/test_%.c $(BOND_SOURCES) $(BOND_HEADERS) components/bonding/tests/fixtures/dvb_bond_v1_client.h
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Icomponents/bonding/include $< $(BOND_SOURCES) -o $@

.PHONY: test-bonding-component
test-bonding-component: $(BOND_TESTS)
	@set -e; for t in $(BOND_TESTS); do ./$$t; done

$(BUILD_DIR)/bonding_fixture: tests/bonding_fixture.c $(BOND_SOURCES) $(BOND_HEADERS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Icomponents/bonding/include $< $(BOND_SOURCES) -o $@

.PHONY: test-dvbt-bonding
test-dvbt-bonding: test-bonding-component $(BUILD_DIR)/linrad_dvb $(BUILD_DIR)/bonding_fixture $(BUILD_DIR)/test_dvbt_psi
	./$(BUILD_DIR)/test_dvbt_psi
	python3 tests/test_dvbt_bonding.py

test: test-dvbt-bonding

# Local Linux USB receiver. The Windows build has its own source list.
KNUCKER_PRIVATE := components/knucker/private
ifneq ($(wildcard $(KNUCKER_PRIVATE)/main.c),)
GTK_CFLAGS += -DHAVE_LOCAL_KNUCKER
$(BUILD_DIR)/linrad_dvb: $(BUILD_DIR)/knucker_rx $(BUILD_DIR)/knucker-receiver.tsv
$(BUILD_DIR)/knucker_rx: $(wildcard $(KNUCKER_PRIVATE)/*.c $(KNUCKER_PRIVATE)/*.h) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -D_POSIX_C_SOURCE=200809L $(shell pkg-config --cflags libusb-1.0) $(filter %.c,$^) -o $@ $(LDFLAGS) -pthread $(shell pkg-config --libs libusb-1.0)
$(BUILD_DIR)/knucker-receiver.tsv: $(KNUCKER_PRIVATE)/receiver.tsv | $(BUILD_DIR)
	cp $< $@
endif
$(BUILD_DIR)/linrad_dvb $(BUILD_DIR)/gui_smoke $(BUILD_DIR)/dab_gui_smoke: components/knucker/status.h
