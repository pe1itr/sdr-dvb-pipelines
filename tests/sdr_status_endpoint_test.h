/* Included after the application source by both native GUI test executables. */
static void exercise_sdr_status(void) {
    App a={.running=TRUE,.standard=3,.source=2};
    const char *line="OO[SDR] RX timing: calls=100 samples=6553600 overflows=7 timeouts=2 output_dropped=0 gaps_over_5ms=4 spectrum_dropped=9 rate=9142857.000 interval_ms=1001.000 recv_mean_ms=6.100 recv_max_ms=9.200 gap_max_ms=8.100 call_interval_max_ms=15.000 overflow_gap_max_ms=8.100 monitor_max_ms=0.100 output_max_ms=0.200 spectrum_async=1\n";
    record_log(&a,line,25);g_assert_cmpint(a.sdr_status_time,==,0);
    record_log(&a,line+25,strlen(line)-25);g_assert_cmpint(a.sdr_status_time,>,0);
    a.decoder_view.status_time=g_get_monotonic_time();
    a.decoder_view.status.input_dropped=123;
    a.decoder_view.status.ldpc_gpu_requested=1;
    a.decoder_view.status.ldpc_backend=2;
    a.decoder_view.status.ldpc_gpu_frames=42;
    char *text=network_status(&a);json_object *root=json_tokener_parse(text);g_free(text);
    json_object *s=json_object_object_get(root,"sdr");g_assert_nonnull(s);
    g_assert_cmpuint(json_object_get_uint64(json_object_object_get(s,"hardware_overflows")),==,7);
    g_assert_true(json_object_get_boolean(json_object_object_get(s,"telemetry_fresh")));
    g_assert_true(json_object_get_boolean(json_object_object_get(s,"spectrum_async")));
    g_assert_cmpfloat(json_object_get_double(json_object_object_get(s,"service_gap_max_ms")),==,8.1);
    g_assert_cmpuint(json_object_get_uint64(json_object_object_get(json_object_object_get(root,"decoder"),"input_dropped")),==,123);
    json_object *decoder=json_object_object_get(root,"decoder");
    g_assert_true(json_object_get_boolean(json_object_object_get(decoder,"ldpc_gpu_requested")));
    g_assert_cmpstr(json_object_get_string(json_object_object_get(decoder,"ldpc_backend")),==,"gpu");
    g_assert_cmpuint(json_object_get_uint64(json_object_object_get(decoder,"ldpc_gpu_frames")),==,42);
    json_object_put(root);
    const char *bad="[SDR] RX timing: calls=broken\n";
    record_log(&a,bad,strlen(bad));g_assert_cmpuint(a.sdr_status.overflows,==,7);
    a.sdr_status_time-=4000000;
    text=network_status(&a);root=json_tokener_parse(text);g_free(text);
    s=json_object_object_get(root,"sdr");
    g_assert_false(json_object_get_boolean(json_object_object_get(s,"telemetry_fresh")));
    json_object_put(root);
    a.sdr_status_time=g_get_monotonic_time();a.running=FALSE;
    text=network_status(&a);root=json_tokener_parse(text);g_free(text);
    g_assert_false(json_object_get_boolean(json_object_object_get(json_object_object_get(root,"sdr"),"telemetry_fresh")));
    json_object_put(root);
    puts("SDR endpoint: fragmented logs, separate losses, stale/stopped freshness passed");
}
