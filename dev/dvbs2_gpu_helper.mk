# Explicit experimental target only. Never part of all/package/GUI selection.
# make -f Makefile -f dev/dvbs2_gpu_helper.mk build/dvbs2_gpu_helper
$(BUILD_DIR)/dvbs2_gpu_helper: dev/dvbs2_gpu_helper.cpp src/dvbs2_gpu_helper.cpp src/ldpc_pool.h $(DVBT2_OBJECTS) $(DVBT2_RUST) $(BUILD_DIR)/leansdr/ldpc_pool.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DVBT2_FLAGS) $< $(DVBT2_OBJECTS) $(DVBT2_RUST) $(BUILD_DIR)/leansdr/ldpc_pool.o -o $@ $(LDFLAGS) -ldl -lm $(LDLIBS)
