// Transmitter-only test tools do not publish receiver telemetry.
extern "C" void linrad_s2_pls(int,int,int) {}
extern "C" void linrad_fec_result(int,int) {}
