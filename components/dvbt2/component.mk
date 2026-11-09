# DVB-T2 broadcast receiver. Keep generators out of the production executable.
DVBT2_DIR := components/dvbt2
CARGO ?= cargo
DVBT2_SOURCES := $(filter-out $(DVBT2_DIR)/src/t2/t2gen.cpp,$(wildcard $(DVBT2_DIR)/src/t2/*.cpp)) $(wildcard $(DVBT2_DIR)/src/dsp/*.cpp $(DVBT2_DIR)/src/common/*.cpp $(DVBT2_DIR)/src/fec/*.cpp $(DVBT2_DIR)/src/tv/*.cpp)
DVBT2_OBJECTS := $(patsubst $(DVBT2_DIR)/src/%.cpp,$(BUILD_DIR)/components/dvbt2/%.o,$(DVBT2_SOURCES))
DVBT2_RUST := $(BUILD_DIR)/components/dvbt2/rust/release/liblinrad_t2_fec.a
DVBT2_FLAGS := -std=gnu++20 -I$(DVBT2_DIR)/include -Isrc -pthread
DVBT2_LINK := $(DVBT2_OBJECTS) $(BUILD_DIR)/components/dvbt2/backend.o $(DVBT2_RUST)
$(BUILD_DIR)/components/dvbt2/%.o: $(DVBT2_DIR)/src/%.cpp
	mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DVBT2_FLAGS) -MMD -MP -c $< -o $@
$(BUILD_DIR)/components/dvbt2/backend.o: src/dvbt2_backend.cc src/dvbt2_backend.h src/decoder_monitor.h
	mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DVBT2_FLAGS) -MMD -MP -c $< -o $@
$(DVBT2_RUST): $(shell find $(DVBT2_DIR)/rust -type f)
	CARGO_HOME=$(abspath $(BUILD_DIR)/cargo-home) $(CARGO) build --offline --locked --release --manifest-path $(DVBT2_DIR)/rust/Cargo.toml --target-dir $(abspath $(BUILD_DIR)/components/dvbt2/rust)
-include $(DVBT2_OBJECTS:.o=.d) $(BUILD_DIR)/components/dvbt2/backend.d
-include $(BUILD_DIR)/components/dvbt2/t2/t2gen.d

$(BUILD_DIR)/dvbt2_fixture: tests/dvbt2_fixture.cpp $(BUILD_DIR)/components/dvbt2/t2/t2gen.o $(DVBT2_OBJECTS) $(DVBT2_RUST)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DVBT2_FLAGS) $^ -o $@ $(LDFLAGS) -ldl -lm $(LDLIBS)
$(BUILD_DIR)/test_dvbt2_gap_clock: $(DVBT2_DIR)/tests/test_gap_clock.cpp $(BUILD_DIR)/components/dvbt2/t2/t2gen.o $(DVBT2_OBJECTS) $(DVBT2_RUST)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DVBT2_FLAGS) $^ -o $@ $(LDFLAGS) -ldl -lm $(LDLIBS)
$(BUILD_DIR)/test_dvbt2_%: $(DVBT2_DIR)/tests/test_%.cpp $(DVBT2_OBJECTS) $(DVBT2_RUST)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DVBT2_FLAGS) $^ -o $@ $(LDFLAGS) -ldl -lm $(LDLIBS)
.PHONY: test-dvbt2
$(BUILD_DIR)/test_dvbt2_gpu_fallback: tests/dvbt2_gpu_fallback.cpp $(filter-out $(BUILD_DIR)/components/dvbt2/dsp/gpu_ldpc_stub.o $(BUILD_DIR)/components/dvbt2/dsp/gpu_ldpc_d3d11.o $(BUILD_DIR)/components/dvbt2/dsp/gpu_ldpc_vulkan.o,$(DVBT2_OBJECTS)) $(DVBT2_RUST)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DVBT2_FLAGS) $^ -o $@ $(LDFLAGS) -ldl -lm $(LDLIBS)
test-dvbt2: $(BUILD_DIR)/test_dvbt2_gpu_fallback
$(BUILD_DIR)/dvbt2_worker: tests/dvbt2_worker_main.cpp $(DVBT2_LINK)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DVBT2_FLAGS) $^ -o $@ $(LDFLAGS) -ldl -lm $(LDLIBS)
$(BUILD_DIR)/test_dvbt2_ldpc: tests/dvbt2_ldpc_equivalence.cpp $(DVBT2_OBJECTS) $(DVBT2_RUST)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DVBT2_FLAGS) $^ -o $@ $(LDFLAGS) -ldl -lm $(LDLIBS)
$(BUILD_DIR)/components/dvbt2/t2/t2ldpc_generic.o: $(DVBT2_DIR)/src/t2/t2ldpc.cpp
	mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DVBT2_FLAGS) -DDECT2_NO_SIMD -MMD -MP -c $< -o $@
-include $(BUILD_DIR)/components/dvbt2/t2/t2ldpc_generic.d
$(BUILD_DIR)/test_dvbt2_ldpc_generic: tests/dvbt2_ldpc_equivalence.cpp $(filter-out $(BUILD_DIR)/components/dvbt2/t2/t2ldpc.o,$(DVBT2_OBJECTS)) $(BUILD_DIR)/components/dvbt2/t2/t2ldpc_generic.o $(DVBT2_RUST)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DVBT2_FLAGS) $^ -o $@ $(LDFLAGS) -ldl -lm $(LDLIBS)
test-dvbt2: $(BUILD_DIR)/test_dvbt2_clock_correct $(BUILD_DIR)/test_dvbt2_stage_prof $(BUILD_DIR)/test_dvbt2_ldpc $(BUILD_DIR)/test_dvbt2_ldpc_generic $(BUILD_DIR)/test_dvbt2_pipeline $(BUILD_DIR)/test_dvbt2_gap_clock
test-dvbt2: $(BUILD_DIR)/linrad_dvb $(BUILD_DIR)/dvbt2_fixture $(BUILD_DIR)/test_dvbt2_fec $(BUILD_DIR)/test_dvbt2_l1
	./$(BUILD_DIR)/test_dvbt2_ldpc
	./$(BUILD_DIR)/test_dvbt2_gpu_fallback
	./$(BUILD_DIR)/test_dvbt2_ldpc_generic
	python3 tests/test_dvbt2_clock_correct.py
	./$(BUILD_DIR)/test_dvbt2_stage_prof
	./$(BUILD_DIR)/test_dvbt2_pipeline
	./$(BUILD_DIR)/test_dvbt2_gap_clock
	./$(BUILD_DIR)/test_dvbt2_fec
	./$(BUILD_DIR)/test_dvbt2_l1
	python3 tests/test_dvbt2.py --build-dir $(BUILD_DIR)
test: test-dvbt2
