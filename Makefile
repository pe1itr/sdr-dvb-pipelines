CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?=
.DEFAULT_GOAL := all

BUILD_DIR := build
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS := $(shell pkg-config --libs gtk+-3.0)
GLIB_CFLAGS := $(shell pkg-config --cflags glib-2.0)
FILTER_CFLAGS := $(shell pkg-config --cflags fftw3f)
FILTER_LIBS := $(shell pkg-config --libs fftw3f)

GLIB_LIBS := $(shell pkg-config --libs glib-2.0)
JSON_CFLAGS := $(shell pkg-config --cflags json-c)
JSON_LIBS := $(shell pkg-config --libs json-c)

# The DVB-T receiver is compiled from the pinned source snapshot into linrad_dvb.
# No installed rbdvbt_rx executable or neighbouring checkout is needed.
DVBT_DIR := third_party/rbdvbt
DVBT_SOURCES := $(filter-out $(DVBT_DIR)/src/main.c,$(wildcard $(DVBT_DIR)/src/*.c))
DVBT_OBJECTS := $(patsubst $(DVBT_DIR)/src/%.c,$(BUILD_DIR)/dvbt/%.o,$(DVBT_SOURCES)) $(BUILD_DIR)/dvbt/backend.o
DVBT_CFLAGS := -I$(DVBT_DIR)/include -Isrc $(FILTER_CFLAGS) -DLINRAD_EMBEDDED_DVBT -DRBDVBT_VERSION=\"0.1.5\"
GUI_SOURCES := src/iq_file.c src/decoder_view.c
GUI_HEADERS := src/decoder_view.h src/decoder_monitor.h src/dvbt_backend.h

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

$(BUILD_DIR)/linrad_dvb: src/linrad_dvb.c $(GUI_SOURCES) $(GUI_HEADERS) $(DVBT_OBJECTS) src/iq_file.h src/sdriq.h src/gui_theme.h src/iq_view.h src/iq_monitor.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DVBT_CFLAGS) $(GTK_CFLAGS) $(JSON_CFLAGS) $< $(GUI_SOURCES) $(DVBT_OBJECTS) -o $@ $(LDFLAGS) $(GTK_LIBS) $(JSON_LIBS) $(FILTER_LIBS) -pthread -lm $(LDLIBS)

$(BUILD_DIR)/iq_file_to_stdout: src/iq_file_to_stdout.c src/iq_file.c src/iq_file.h src/iq_bandfilter.c src/iq_bandfilter.h src/iq_monitor.c src/iq_monitor.h src/tune_control.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(GLIB_CFLAGS) $(JSON_CFLAGS) $(FILTER_CFLAGS) $< src/iq_file.c src/iq_bandfilter.c src/iq_monitor.c -o $@ $(LDFLAGS) $(GLIB_LIBS) $(JSON_LIBS) $(FILTER_LIBS) -lm $(LDLIBS)

$(BUILD_DIR)/linrad_udp_to_stdout: src/linrad_udp_to_stdout.c src/iq_monitor.c src/iq_monitor.h src/iq_bandfilter.c src/iq_bandfilter.h src/tune_control.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(GLIB_CFLAGS) $(FILTER_CFLAGS) $< src/iq_bandfilter.c src/iq_monitor.c -o $@ $(LDFLAGS) $(GLIB_LIBS) $(FILTER_LIBS) -lm $(LDLIBS)

package: all
	cp third_party/rbdvbt/LICENSE $(BUILD_DIR)/rbdvbt-LICENSE.txt
	@echo "Start: ./build/linrad_dvb (config: linrad-dvb.ini)"

$(BUILD_DIR)/gui_smoke: tests/gui_smoke.c src/linrad_dvb.c $(GUI_SOURCES) $(GUI_HEADERS) $(DVBT_OBJECTS) src/iq_file.h src/sdriq.h src/gui_theme.h src/iq_view.h src/iq_monitor.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DVBT_CFLAGS) $(GTK_CFLAGS) $(JSON_CFLAGS) $< $(GUI_SOURCES) $(DVBT_OBJECTS) -o $@ $(LDFLAGS) $(GTK_LIBS) $(JSON_LIBS) $(FILTER_LIBS) -pthread -lm $(LDLIBS)

test: all $(BUILD_DIR)/gui_smoke
	python3 tests/test_linrad.py

clean:
	rm -rf $(BUILD_DIR)

$(BUILD_DIR)/sdr_to_stdout: src/sdr_to_stdout.c src/iq_monitor.c src/iq_monitor.h src/iq_bandfilter.c src/iq_bandfilter.h src/iq_output.c src/iq_output.h src/iq_convert.h src/sdriq.h src/tune_control.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(FILTER_CFLAGS) $(shell pkg-config --cflags SoapySDR uhd) src/sdr_to_stdout.c src/iq_output.c src/iq_bandfilter.c src/iq_monitor.c -pthread -o $@ $(LDFLAGS) $(shell pkg-config --libs SoapySDR uhd) $(FILTER_LIBS) -lm $(LDLIBS)

$(BUILD_DIR)/test_iq_output: tests/test_iq_output.c src/iq_output.c src/iq_output.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_iq_output.c src/iq_output.c -o $@ $(LDFLAGS) -pthread $(LDLIBS)

.PHONY: test-iq-output
test-iq-output: $(BUILD_DIR)/test_iq_output
	./$(BUILD_DIR)/test_iq_output

test: test-iq-output

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

$(BUILD_DIR)/bandfilter_probe: tests/bandfilter_probe.c src/iq_bandfilter.c src/iq_bandfilter.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(FILTER_CFLAGS) tests/bandfilter_probe.c src/iq_bandfilter.c src/iq_monitor.c -o $@ $(LDFLAGS) $(FILTER_LIBS) -lm $(LDLIBS)

.PHONY: test-bandfilter
test-bandfilter: $(BUILD_DIR)/bandfilter_probe
	python3 tests/test_bandfilter.py

test: test-bandfilter

$(BUILD_DIR)/monitor_probe: tests/monitor_probe.c src/iq_monitor.c src/iq_monitor.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(FILTER_CFLAGS) tests/monitor_probe.c src/iq_monitor.c -o $@ $(LDFLAGS) $(FILTER_LIBS) -lm $(LDLIBS)

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
test-dvbt: $(BUILD_DIR)/test_dvbt_stream
	./$(BUILD_DIR)/test_dvbt_stream

test: test-dvbt
