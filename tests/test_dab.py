"""DAB pipeline contracts and optional receive-only IQ regressions."""
import configparser
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import time
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = Path(os.environ.get("DAB_FIXTURE_DIR", ROOT / "tests/fixtures/dab"))


class DabTests(unittest.TestCase):
    def preview(self, source="b210", rate=4096000, **settings):
        with tempfile.TemporaryDirectory() as tmp:
            cfg = configparser.ConfigParser(interpolation=None)
            cfg.read(ROOT / "linrad-dvb.example.ini")
            cfg[source]["sample_rate"] = str(rate)
            cfg[source]["receiver_offset"] = "0"
            cfg[source]["lo_offset"] = "0"
            cfg[source]["filter_enabled"] = "false"
            cfg["dab"] = {"channel": "12D", "profile": "weak", **settings}
            path = Path(tmp) / "config.ini"
            with path.open("w") as f:
                cfg.write(f)
            return subprocess.run([ROOT / "build/linrad_dvb", "--config", path,
                "--source", source, "--standard", "DAB", "--print-pipeline"],
                text=True, capture_output=True, timeout=5)

    def test_direct_and_mixer_paths(self):
        p = self.preview()
        self.assertEqual(p.returncode, 0, p.stderr)
        stages = [shlex.split(line) for line in p.stdout.splitlines() if line.startswith("'")]
        self.assertEqual(len(stages), 2)
        self.assertIn("229072000", stages[0])
        self.assertNotIn("--dab-lo-output", stages[0])
        self.assertIn("dab_worker", stages[1][0])
        self.assertIn("12D", stages[1])
        self.assertIn("weak", stages[1])
        p = self.preview(mixer_enabled="true", lo_output_enabled="true", rx_input="RX2", lo_port="TRX1")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("'--dab-if' '53000000'", p.stdout)
        self.assertIn("'--dab-lo-output'", p.stdout)
        p = self.preview(mixer_enabled="true", lo_output_enabled="false")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertNotIn("--dab-lo-output", p.stdout)

    def test_invalid_settings_fail_before_hardware(self):
        for values in ({"channel": "4Z"}, {"profile": "unknown"},
                       {"mixer_enabled": "true", "if_hz": "nan"},
                       {"mixer_enabled": "true", "lo_output_enabled": "true", "rx_input": "TRX1", "lo_port": "TRX1"}):
            with self.subTest(values=values):
                p = self.preview(**values)
                self.assertNotEqual(p.returncode, 0)
                self.assertNotIn("Creating a usrp", p.stderr)
        for options in (["--source", "rtlsdr", "--dab-lo-output"],
                        ["--source", "b210", "--dab-lo-output"],
                        ["--source", "b210", "--frequency", "229072000", "--dab-if", "53000000",
                         "--b210-input", "TRX1", "--dab-lo-output"]):
            p = subprocess.run([ROOT / "build/sdr_to_stdout", *options], capture_output=True, timeout=3)
            self.assertEqual(p.returncode, 2, p.stderr)

    def test_sample_rates_follow_hardware_limits(self):
        for source,rate in (("b210",4000000),("b210",8000000),("b210",10000000),("b210",12345678),
                            ("b210",20000000),("b210",61440000),("hackrf",20000000),
                            ("rtlsdr",2048000),("airspyr2",2500000),("airspyr2",10000000)):
            with self.subTest(source=source,rate=rate):
                result=self.preview(source=source,rate=rate)
                self.assertEqual(result.returncode,0,result.stderr)
                self.assertEqual(result.stdout.count(f"'--sample-rate' '{rate}'"),2)
        for source,rate in (("b210",2048000),("b210",61440001),("hackrf",20000001),
                            ("rtlsdr",4000000),("airspyr2",8000000),("rsp1a",20000000)):
            with self.subTest(source=source,rate=rate):
                self.assertNotEqual(self.preview(source=source,rate=rate).returncode,0)

    def test_retune_discards_queued_iq_at_acknowledged_byte_boundary(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory=Path(tmp)
            p=subprocess.Popen([ROOT/"build/dab_worker","--state-dir",tmp,"--channel","9C","--no-audio"],
                               stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
            def wait_state(predicate):
                deadline=time.monotonic()+5
                while time.monotonic()<deadline:
                    try:
                        state=json.loads((directory/"state.json").read_text())
                        if predicate(state): return state
                    except (FileNotFoundError,json.JSONDecodeError): pass
                    time.sleep(.01)
                self.fail("DAB worker did not acknowledge channel request")
            try:
                wait_state(lambda s:s["generation"]==0)
                (directory/"channel").write_text("1 10A\n")
                wait_state(lambda s:s["generation"]==1 and s["tuning"])
                old=struct.pack("<hh",16384,-16384)*196608
                # An odd partial read while waiting must not corrupt I/Q alignment.
                p.stdin.write(old[:12345]);p.stdin.flush()
                (directory/"tuned").write_text(f"1 209936000 {len(old)}\n")
                p.stdin.write(old[12345:]+bytes(3));p.stdin.flush()
                fresh_samples=196608*3
                p.stdin.write(bytes(4*fresh_samples-3));p.stdin.close();p.stdin=None
                _,errors=p.communicate(timeout=15)
                self.assertEqual(p.returncode,0,errors.decode())
                state=json.loads((directory/"state.json").read_text())
                self.assertEqual(state["generation"],1)
                self.assertEqual(state["channel"],"10A")
                self.assertEqual(state["input_samples"],fresh_samples)
                self.assertEqual(state["ueid"],0)
                self.assertEqual(state["tii"],[])
                self.assertEqual(state["constellation"],[])
            finally:
                if p.poll() is None: p.kill();p.communicate()

    def worker(self, fixture, channel, *extra):
        with tempfile.TemporaryDirectory() as tmp, fixture.open("rb") as iq:
            p = subprocess.run([ROOT / "build/dab_worker", "--state-dir", tmp, "--channel", channel,
                                "--no-audio", *extra], stdin=iq, capture_output=True, timeout=90)
            self.assertEqual(p.returncode, 0, p.stderr.decode())
            return json.loads((Path(tmp) / "state.json").read_text())

    def test_silence_does_not_create_identity(self):
        with tempfile.TemporaryDirectory() as tmp:
            fixture = Path(tmp) / "silence.iq"
            fixture.write_bytes(bytes(196608 * 4 * 3))
            state = self.worker(fixture, "9C")
            self.assertEqual(state["ueid"], 0)
            self.assertEqual(state["tii"], [])
            self.assertEqual(state.get("services", []), [])

    def test_iq_file_pipeline_drains_and_stops(self):
        with tempfile.TemporaryDirectory() as tmp:
            fixture = Path(tmp) / "silence.iq"
            fixture.write_bytes(bytes(196608 * 4 * 3))
            cfg = configparser.ConfigParser(interpolation=None)
            cfg.read(ROOT / "linrad-dvb.example.ini")
            cfg["file"] = {"path": str(fixture), "input_format": "s16", "format": "s16",
                           "sample_rate": "2048000", "header_bytes": "0", "frequency": "206352000"}
            cfg["paths"]["iq_file"] = str(ROOT / "build/iq_file_to_stdout")
            cfg["dab"] = {"channel": "9C"}
            path = Path(tmp) / "config.ini"
            with path.open("w") as f:
                cfg.write(f)
            p = subprocess.run([ROOT / "build/linrad_dvb", "--config", path,
                "--source", "file", "--standard", "DAB", "--run"], capture_output=True, timeout=15)
            self.assertEqual(p.returncode, 0, p.stderr.decode())
            self.assertIn(b"IQ file playback complete", p.stderr)

    @unittest.skipUnless((FIXTURES / "dab-9c-fic-positive-1s.s16le.iq").is_file(), "set DAB_FIXTURE_DIR for recorded IQ")
    def test_known_ensemble_and_tii(self):
        state = self.worker(FIXTURES / "dab-9c-fic-positive-1s.s16le.iq", "9C")
        self.assertEqual(state["ueid"], 0xE3809C)
        self.assertEqual(state["ensemble"], "9C")
        codes = {(int(t["main"]), int(t["sub"])) for t in state["tii"] if t["confirmed"] and not t["ambiguous"]}
        self.assertTrue({(19, 4), (19, 16)} <= codes)

    @unittest.skipUnless((FIXTURES / "dab-9c-fic-positive-1s.s16le.iq").is_file(), "set DAB_FIXTURE_DIR for recorded IQ")
    def test_recorded_ensemble_at_fractional_10mhz_rate(self):
        try:
            import numpy as np
            from scipy.signal import resample_poly
        except ImportError:
            self.skipTest("numpy/scipy needed to independently upsample the recorded fixture")
        original=np.fromfile(FIXTURES/"dab-9c-fic-positive-1s.s16le.iq",dtype="<i2").reshape(-1,2)
        wide=resample_poly(original.astype(np.float32),625,128,axis=0)
        with tempfile.TemporaryDirectory() as tmp:
            fixture=Path(tmp)/"10mhz.iq"
            np.clip(wide,-32768,32767).astype("<i2").tofile(fixture)
            state=self.worker(fixture,"9C","--sample-rate","10000000")
        self.assertEqual(state["ueid"],0xE3809C)
        self.assertEqual(state["ensemble"],"9C")
        self.assertGreater(state["scope_sequence"],0)
        self.assertGreater(len(state["constellation"]),100)

    @unittest.skipUnless(os.environ.get("DAB_REALTIME_TEST") == "1" and
                         (FIXTURES / "live-tii-phase-11c-4s.s16le.iq").is_file(),
                         "set DAB_REALTIME_TEST=1 and DAB_FIXTURE_DIR for paced reception")
    def test_paced_wideband_reception_keeps_up_after_first_lock(self):
        import numpy as np
        from scipy.signal import resample_poly
        original=np.fromfile(FIXTURES/"live-tii-phase-11c-4s.s16le.iq",dtype="<i2").reshape(-1,2)
        for rate,up in ((10000000,625),(20000000,625)):
            with self.subTest(rate=rate), tempfile.TemporaryDirectory() as tmp:
                wide=resample_poly(original.astype(np.float32),up,128 if rate==10000000 else 64,axis=0)
                data=np.clip(wide,-32768,32767).astype("<i2").tobytes();del wide
                directory=Path(tmp)
                p=subprocess.Popen([ROOT/"build/dab_worker","--state-dir",tmp,"--channel","11C",
                    "--sample-rate",str(rate),"--no-audio"],stdin=subprocess.PIPE,
                    stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
                try:
                    started=time.monotonic();sent=0;max_lag=0;locks=0;last_check=0;auto_audio=0;manual_sid=None;audio_frames=0
                    # Three fixture passes exercise initial acquisition, sustained audio
                    # and the first asynchronous 8-second DX job. Ignore the short
                    # lock reacquisition gaps at the repeated fixture boundaries.
                    for repeat in range(3):
                        for offset in range(0,len(data),262144):
                            block=data[offset:offset+262144]
                            p.stdin.write(block);p.stdin.flush();sent+=len(block)
                            due=started+sent/(4*rate);now=time.monotonic()
                            max_lag=max(max_lag,now-due)
                            if now-last_check>.25:
                                last_check=now
                                try:
                                    state=json.loads((directory/"state.json").read_text())
                                    if not manual_sid:
                                        auto_audio=max(auto_audio,state.get("audio_frames",0))
                                    if repeat>=1 and len(state.get("services",[]))>1 and not manual_sid:
                                        self.assertGreater(auto_audio,0,"First station did not start automatically")
                                        manual_sid=hex(int(state["services"][1]["id"]))
                                        (directory/"station").write_text(manual_sid)
                                    if repeat>=1 and state.get("synced"):locks+=1
                                    audio_frames=max(audio_frames,state.get("audio_frames",0))
                                except (FileNotFoundError,json.JSONDecodeError):pass
                            if due>now:time.sleep(due-now)
                    p.stdin.close();p.stdin=None
                    _,errors=p.communicate(timeout=45)
                    self.assertEqual(p.returncode,0,errors.decode())
                    state=json.loads((directory/"state.json").read_text())
                    self.assertLess(max_lag,.5,"DAB input is falling behind the live SDR clock")
                    self.assertGreater(locks,12,"FIC lock did not persist after acquisition")
                    self.assertGreater(audio_frames,50,"Audio stopped after initial lock")
                    self.assertGreater(auto_audio,0)
                    self.assertEqual(state["selection"],manual_sid,"Automatic selection overrode the chosen station")
                    self.assertGreater(state["audio_frames"],auto_audio)
                    self.assertEqual(state["dropped_iq"],0)
                    self.assertGreaterEqual(state["captures"],1)
                    print(f"\n{rate/1e6:g} MS/s, 12 seconds: max input lag {max_lag:.3f}s, "
                          f"{locks} later FIC locks, {int(audio_frames)} audio frames, zero IQ drops")
                finally:
                    if p.poll() is None:p.kill();p.communicate()

    @unittest.skipUnless((FIXTURES / "live-tii-false-main-7c-8s.s16le.iq").is_file(), "set DAB_FIXTURE_DIR for recorded IQ")
    def test_false_main_is_never_a_loggable_detection(self):
        state = self.worker(FIXTURES / "live-tii-false-main-7c-8s.s16le.iq", "7C", "--profile", "weak")
        codes = {(int(t["main"]), int(t["sub"])) for t in state["tii"] if t["confirmed"] and not t["ambiguous"]}
        self.assertTrue({(7, 1), (7, 2), (7, 14)} <= codes)
        self.assertNotIn((46, 15), codes)


if __name__ == "__main__":
    unittest.main(verbosity=2)
