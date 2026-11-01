CC ?= cc
CXXFLAGS ?= -O3 -Wall -Wextra -std=gnu++17
CFLAGS ?= -O2 -Wall -Wextra -std=c11
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?=
.DEFAULT_GOAL := all

BUILD_DIR := build
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS := $(shell pkg-config --libs gtk+-3.0)
GTK_CFLAGS += $(shell pkg-config --cflags libvlc)
GTK_LIBS += $(shell pkg-config --libs libvlc)
USB_CFLAGS := $(shell pkg-config --cflags alsa samplerate)
USB_LIBS := $(shell pkg-config --libs alsa samplerate)
GTK_CFLAGS += $(USB_CFLAGS)
GTK_LIBS += $(USB_LIBS)
GLIB_CFLAGS := $(shell pkg-config --cflags glib-2.0)
FILTER_CFLAGS := $(shell pkg-config --cflags fftw3f)
FILTER_LIBS := $(shell pkg-config --libs fftw3f)

GLIB_LIBS := $(shell pkg-config --libs glib-2.0)
JSON_CFLAGS := $(shell pkg-config --cflags json-c)
JSON_LIBS := $(shell pkg-config --libs json-c)


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

# The DVB-T receiver is compiled from the pinned source snapshot into linrad_dvb.
# No installed rbdvbt_rx executable or neighbouring checkout is needed.
DVBT_DIR := third_party/rbdvbt
DVBT_SOURCES := $(filter-out $(DVBT_DIR)/src/main.c,$(wildcard $(DVBT_DIR)/src/*.c))
DVBT_OBJECTS := $(patsubst $(DVBT_DIR)/src/%.c,$(BUILD_DIR)/dvbt/%.o,$(DVBT_SOURCES)) $(BUILD_DIR)/dvbt/backend.o
DVBT_CFLAGS := -I$(DVBT_DIR)/include -Isrc $(FILTER_CFLAGS) -DLINRAD_EMBEDDED_DVBT -DRBDVBT_VERSION=\"0.1.5\"
# Pinned TSscatterfix recovery library; RGB callbacks are displayed by GTK.
SCATTER_DIR := third_party/tsscatterfix
SCATTER_SOURCES := $(addprefix $(SCATTER_DIR)/src/,recovery.c media.c rank_model.c psi.c ts_packet.c)
SCATTER_OBJECTS := $(patsubst $(SCATTER_DIR)/src/%.c,$(BUILD_DIR)/scatter/%.o,$(SCATTER_SOURCES))
SCATTER_CFLAGS := -I$(SCATTER_DIR)/src $(shell pkg-config --cflags libavcodec libavformat libavutil libswscale)
TELETEXT_CFLAGS ?= $(shell pkg-config --cflags zvbi-0.2)
TELETEXT_LIBS ?= $(shell pkg-config --libs zvbi-0.2)
SCATTER_CFLAGS += $(TELETEXT_CFLAGS)
SCATTER_LIBS := $(shell pkg-config --libs libavcodec libavformat libavutil libswscale) $(TELETEXT_LIBS)

$(BUILD_DIR)/scatter/%.o: $(SCATTER_DIR)/src/%.c $(wildcard $(SCATTER_DIR)/src/*.h)
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SCATTER_CFLAGS) -c $< -o $@

GUI_SOURCES := src/iq_file.c src/decoder_view.c src/video_view.c src/scatter_backend.c src/scatter_view.c src/ts_info.c src/ts_info_view.c src/usb_receiver.c src/usb_demod.c src/usb_tci.c
GUI_HEADERS := src/usb_receiver.h src/usb_demod.h src/usb_tci.h src/usb_tap.h
GUI_SOURCES += src/program_view.c
GUI_HEADERS += src/program_view.h
GUI_HEADERS += src/leansdr_backend.h src/decoder_view.h src/decoder_monitor.h src/dvbt_backend.h src/video_view.h src/scatter_view.h src/scatter_backend.h src/ts_info.h src/ts_info_view.h

$(BUILD_DIR)/dvbt/%.o: $(DVBT_DIR)/src/%.c $(wildcard $(DVBT_DIR)/include/*.h) src/dvbt_backend.h src/decoder_monitor.h
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -O3 $(DVBT_CFLAGS) -pthread -c $< -o $@

$(BUILD_DIR)/dvbt/backend.o: src/dvbt_backend.c src/dvbt_backend.h src/decoder_monitor.h $(wildcard $(DVBT_DIR)/include/*.h)
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DVBT_CFLAGS) -pthread -c $< -o $@

.PHONY: all package test clean

all: $(BUILD_DIR)/linrad_dvb $(BUILD_DIR)/linrad_udp_to_stdout $(BUILD_DIR)/sdr_to_stdout $(BUILD_DIR)/iq_file_to_stdout

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/linrad_dvb: src/linrad_dvb.c $(GUI_SOURCES) $(GUI_HEADERS) $(LEAN_OBJECTS) $(DVBT_OBJECTS) $(SCATTER_OBJECTS) src/iq_file.h src/sdriq.h src/rtlsdr.h src/gui_theme.h src/iq_view.h src/iq_monitor.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DVBT_CFLAGS) $(SCATTER_CFLAGS) $(GTK_CFLAGS) $(JSON_CFLAGS) $< $(GUI_SOURCES) $(LEAN_OBJECTS) $(DVBT_OBJECTS) $(SCATTER_OBJECTS) $(SCATTER_LIBS) -o $@ $(LDFLAGS) $(GTK_LIBS) $(JSON_LIBS) $(FILTER_LIBS) -pthread -lm -lstdc++ $(LDLIBS)

$(BUILD_DIR)/iq_file_to_stdout: src/iq_file_to_stdout.c src/iq_file.c src/iq_file.h src/iq_bandfilter.c src/iq_bandfilter.h src/iq_monitor.c src/usb_tap.c src/usb_tap.h src/iq_monitor.h src/tune_control.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(GLIB_CFLAGS) $(JSON_CFLAGS) $(FILTER_CFLAGS) $< src/iq_file.c src/iq_bandfilter.c src/iq_monitor.c src/usb_tap.c -o $@ $(LDFLAGS) $(GLIB_LIBS) $(JSON_LIBS) $(FILTER_LIBS) -lm $(LDLIBS)

$(BUILD_DIR)/linrad_udp_to_stdout: src/linrad_udp_to_stdout.c src/iq_monitor.c src/usb_tap.c src/usb_tap.h src/iq_monitor.h src/iq_bandfilter.c src/iq_bandfilter.h src/tune_control.h src/iq_record.c src/iq_record.h src/iq_output.c src/iq_output.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(GLIB_CFLAGS) $(FILTER_CFLAGS) $< src/iq_bandfilter.c src/iq_monitor.c src/usb_tap.c src/iq_record.c src/iq_output.c -o $@ $(LDFLAGS) $(GLIB_LIBS) $(FILTER_LIBS) -pthread -lm $(LDLIBS)

package: all
	cp $(LEAN_DIR)/LICENSE.txt $(BUILD_DIR)/leansdr-LICENSE.txt
	cp $(LDPC_DIR)/LICENSE $(BUILD_DIR)/ldpc-LICENSE.txt
	cp third_party/rbdvbt/LICENSE $(BUILD_DIR)/rbdvbt-LICENSE.txt
	cp $(SCATTER_DIR)/LICENSE $(BUILD_DIR)/tsscatterfix-LICENSE.txt
	@echo "Start: ./build/linrad_dvb (config: linrad-dvb.ini)"

$(BUILD_DIR)/gui_smoke: tests/gui_smoke.c src/linrad_dvb.c $(GUI_SOURCES) $(GUI_HEADERS) $(LEAN_OBJECTS) $(DVBT_OBJECTS) $(SCATTER_OBJECTS) src/iq_file.h src/sdriq.h src/rtlsdr.h src/gui_theme.h src/iq_view.h src/iq_monitor.h src/tune_control.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DVBT_CFLAGS) $(SCATTER_CFLAGS) $(GTK_CFLAGS) $(JSON_CFLAGS) $< $(GUI_SOURCES) $(LEAN_OBJECTS) $(DVBT_OBJECTS) $(SCATTER_OBJECTS) $(SCATTER_LIBS) -o $@ $(LDFLAGS) $(GTK_LIBS) $(JSON_LIBS) $(FILTER_LIBS) -pthread -lm -lstdc++ $(LDLIBS)

$(BUILD_DIR)/video_smoke: tests/video_smoke.c src/video_view.c src/video_view.h src/scatter_backend.c src/scatter_backend.h src/ts_info.c src/ts_info.h $(SCATTER_OBJECTS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc $(GTK_CFLAGS) $(SCATTER_CFLAGS) tests/video_smoke.c src/video_view.c src/scatter_backend.c src/ts_info.c $(SCATTER_OBJECTS) -o $@ $(LDFLAGS) $(GTK_LIBS) $(SCATTER_LIBS) -pthread -lm $(LDLIBS)

test: all $(BUILD_DIR)/gui_smoke $(BUILD_DIR)/video_smoke
	python3 tests/test_linrad.py

clean:
	rm -rf $(BUILD_DIR)

$(BUILD_DIR)/sdr_to_stdout: src/afedri.c src/afedri.h src/sdr_to_stdout.c src/iq_monitor.c src/usb_tap.c src/usb_tap.h src/iq_monitor.h src/iq_bandfilter.c src/iq_bandfilter.h src/iq_output.c src/iq_output.h src/iq_convert.h src/sdriq.h src/rtlsdr.h src/tune_control.h src/iq_record.c src/iq_record.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(FILTER_CFLAGS) $(shell pkg-config --cflags SoapySDR uhd) src/sdr_to_stdout.c src/afedri.c src/iq_output.c src/iq_record.c src/iq_bandfilter.c src/iq_monitor.c src/usb_tap.c -pthread -o $@ $(LDFLAGS) $(shell pkg-config --libs SoapySDR uhd) $(FILTER_LIBS) -lm $(LDLIBS)

$(BUILD_DIR)/test_iq_output: tests/test_iq_output.c src/iq_output.c src/iq_output.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_iq_output.c src/iq_output.c -o $@ $(LDFLAGS) -pthread $(LDLIBS)

.PHONY: test-iq-output
test-iq-output: $(BUILD_DIR)/test_iq_output
	./$(BUILD_DIR)/test_iq_output

test: test-iq-output

$(BUILD_DIR)/test_iq_record: tests/test_iq_record.c src/iq_record.c src/iq_record.h src/iq_output.c src/iq_output.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< src/iq_record.c src/iq_output.c -o $@ $(LDFLAGS) -pthread -lm $(LDLIBS)

.PHONY: test-iq-record
test-iq-record: $(BUILD_DIR)/test_iq_record
	./$(BUILD_DIR)/test_iq_record

test: test-iq-record

$(BUILD_DIR)/iq_conversion_probe: tests/iq_conversion_probe.c src/iq_convert.h src/sdriq.h src/tune_control.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDFLAGS) -lm $(LDLIBS)

.PHONY: test-iq-conversion
test-iq-conversion: $(BUILD_DIR)/iq_conversion_probe
	python3 tests/test_iq_conversion.py

test: test-iq-conversion

$(BUILD_DIR)/test_tune_control: tests/test_tune_control.c src/tune_control.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDFLAGS) -lm $(LDLIBS)

.PHONY: test-tune-control
test-tune-control: $(BUILD_DIR)/test_tune_control
	./$(BUILD_DIR)/test_tune_control

test: test-tune-control

$(BUILD_DIR)/bandfilter_probe: tests/bandfilter_probe.c src/iq_bandfilter.c src/iq_bandfilter.h src/iq_monitor.c src/iq_monitor.h src/usb_tap.c src/usb_tap.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(FILTER_CFLAGS) tests/bandfilter_probe.c src/iq_bandfilter.c src/iq_monitor.c src/usb_tap.c -o $@ $(LDFLAGS) $(FILTER_LIBS) -lm $(LDLIBS)

.PHONY: test-bandfilter
test-bandfilter: $(BUILD_DIR)/bandfilter_probe
	python3 tests/test_bandfilter.py

test: test-bandfilter

$(BUILD_DIR)/monitor_probe: tests/monitor_probe.c src/iq_monitor.c src/usb_tap.c src/usb_tap.h src/iq_monitor.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(FILTER_CFLAGS) tests/monitor_probe.c src/iq_monitor.c src/usb_tap.c -o $@ $(LDFLAGS) $(FILTER_LIBS) -lm $(LDLIBS)

.PHONY: test-iq-monitor
test-iq-monitor: $(BUILD_DIR)/monitor_probe
	python3 tests/test_iq_monitor.py

test: test-iq-monitor

.PHONY: test-file-replay
test-file-replay: all
	python3 tests/test_file_replay.py

test: test-file-replay

$(BUILD_DIR)/test_dvbt_stream: tests/test_dvbt_stream.c $(DVBT_DIR)/src/probe_constellation.c $(DVBT_OBJECTS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -O3 $(DVBT_CFLAGS) $< $(filter-out $(BUILD_DIR)/dvbt/probe_constellation.o,$(DVBT_OBJECTS)) -o $@ $(LDFLAGS) $(FILTER_LIBS) -pthread -lm $(LDLIBS)

.PHONY: test-dvbt
$(BUILD_DIR)/test_dvbt_psi: tests/test_dvbt_psi.c $(DVBT_DIR)/src/dvbt_outer.c $(DVBT_OBJECTS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -O3 $(DVBT_CFLAGS) $< $(filter-out $(BUILD_DIR)/dvbt/dvbt_outer.o,$(DVBT_OBJECTS)) -o $@ $(LDFLAGS) $(FILTER_LIBS) -pthread -lm $(LDLIBS)

test-dvbt: $(BUILD_DIR)/test_dvbt_stream $(BUILD_DIR)/test_dvbt_psi
	./$(BUILD_DIR)/test_dvbt_stream
	./$(BUILD_DIR)/test_dvbt_psi

test: test-dvbt

$(BUILD_DIR)/scatter_probe: tests/scatter_probe.c src/scatter_backend.c src/scatter_backend.h src/ts_info.c src/ts_info.h $(SCATTER_OBJECTS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc $(GLIB_CFLAGS) $(SCATTER_CFLAGS) tests/scatter_probe.c src/scatter_backend.c src/ts_info.c $(SCATTER_OBJECTS) -o $@ $(LDFLAGS) $(GLIB_LIBS) $(SCATTER_LIBS) -pthread -lm $(LDLIBS)

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

$(BUILD_DIR)/ts_info_relay: tests/ts_info_relay.c src/scatter_backend.c src/scatter_backend.h src/ts_info.c src/ts_info.h $(SCATTER_OBJECTS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc $(GLIB_CFLAGS) $(SCATTER_CFLAGS) tests/ts_info_relay.c src/scatter_backend.c src/ts_info.c $(SCATTER_OBJECTS) -o $@ $(LDFLAGS) $(GLIB_LIBS) $(SCATTER_LIBS) -pthread -lm $(LDLIBS)

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
