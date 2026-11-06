"""Exercise real CSV reading and ADI export with an independent tag parser."""
import base64
import csv
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HEADER = ["timestamp_utc", "frequency_hz", "service_name", "provider_name", "mode", "tii", "source",
          "symbol_rate_baud", "bandwidth_hz", "fec", "atv_points", "atv_direction"]


def read_adif(path):
    text = Path(path).read_text(encoding="ascii")
    pos = text.index("<")
    records, record = [], {}
    while pos < len(text):
        if text[pos].isspace():
            pos += 1
            continue
        match = re.match(r"<([A-Z0-9_]+)(?::(\d+))?>", text[pos:])
        if not match:
            raise AssertionError(f"Malformed ADI at {pos}: {text[pos:pos+50]!r}")
        name, length = match.groups()
        pos += len(match.group())
        if name == "EOH":
            assert record["PROGRAMID"] == "LINRADDVB"
            record = {}
        elif name == "EOR":
            records.append(record)
            record = {}
        else:
            assert length is not None
            length = int(length)
            record[name] = text[pos:pos+length]
            assert len(record[name]) == length
            pos += length
    assert not record
    return records


class ReceptionLogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        cls.probe = Path(cls.build.name, "probe")
        flags = shlex.split(subprocess.check_output(
            ["pkg-config", "--cflags", "--libs", "gio-2.0"], text=True))
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + ["-std=c11",
            str(ROOT / "tests/reception_log_probe.c"), "-o", str(cls.probe)] + flags, check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.log = Path(self.tmp.name, "log.csv")
        self.out = Path(self.tmp.name, "export.adi")

    def write(self, rows):
        with self.log.open("w", newline="", encoding="utf-8") as f:
            writer = csv.writer(f)
            writer.writerow(HEADER)
            writer.writerows(row + [""] * (len(HEADER) - len(row)) for row in rows)

    def run_export(self, ok=True):
        result = subprocess.run([self.probe, self.log, self.out], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0 if ok else 1, result.stderr)
        return result

    def test_utc_units_names_and_lengths(self):
        service = 'Télé, "TV" <EOR> 日本'
        provider = "Provider\nTwo"
        self.write([["2026-11-06T23:59:58.123456+01:00", "436125000", service,
                     provider, "DVB-T2", "", "file", "", "8000000", "3/5"]])
        self.run_export()
        row, = read_adif(self.out)
        self.assertEqual(row["QSO_DATE"], "20261106")
        self.assertEqual(row["TIME_ON"], "225958")
        self.assertEqual(row["FREQ"], "436.125000")
        self.assertEqual(row["FREQ_RX"], "436.125000")
        self.assertEqual(row["MODE"], "ATV")
        self.assertEqual(row["SWL"], "Y")
        self.assertEqual(row["APP_LINRADDVB_MODE"], "DVB-T2")
        self.assertEqual(row["APP_LINRADDVB_SOURCE"], "file")
        self.assertNotIn("CALL", row)
        self.assertNotIn("SUBMODE", row)
        self.assertNotIn("APP_LINRADDVB_TII", row)
        self.assertIn("BW (Hz): 8000000; FEC: 3/5", row["COMMENT"])
        self.assertNotIn("APP_LINRADDVB_FEC", row)
        self.assertNotIn("APP_LINRADDVB_BANDWIDTH", row)
        self.assertNotIn("APP_VERON_ATV_POINTS", row)
        self.assertNotIn("APP_VERON_ATV_DIRECTION", row)
        for field, original in [("SERVICE", service), ("PROVIDER", provider)]:
            self.assertEqual(base64.b64decode(row[f"APP_LINRADDVB_{field}_UTF8_B64"]).decode(), original)

    def test_tii_preserved_without_inventing_a_standard_mode(self):
        self.write([["2026-11-06T12:00:00Z", "227360000", "Service", "Provider", "DAB+", "12:03; 21:07", "rtlsdr"]])
        self.run_export()
        row, = read_adif(self.out)
        self.assertEqual(row["APP_LINRADDVB_TII"], "12:03; 21:07")
        self.assertEqual(row["APP_LINRADDVB_MODE"], "DAB+")
        self.assertNotIn("MODE", row)

    def test_contestrobot_extra_fields_and_symbol_rate_comment(self):
        self.write([["2026-11-06T12:00:00Z", "437000000", "S", "P", "DVB-S2", "", "b210",
                     "125000", "", "1/2", "1234", "ONE-WAY"]])
        self.run_export()
        row, = read_adif(self.out)
        self.assertEqual(row["APP_VERON_ATV_POINTS"], "1234")
        self.assertEqual(row["APP_VERON_ATV_DIRECTION"], "ONE-WAY")
        self.assertIn("SR (baud): 125000; BW (Hz): ; FEC: 1/2", row["COMMENT"])
        self.assertNotIn("APP_LINRADDVB_SYMBOL_RATE_BAUD", row)

    def test_invalid_csv_or_values_do_not_replace_export(self):
        self.out.write_text("previous export")
        for frequency, timestamp in [("NaN", "2026-11-06T12:00:00Z"),
                                     ("0", "2026-11-06T12:00:00Z"),
                                     ("123", "not a time")]:
            self.write([[timestamp, frequency, "S", "P", "DVB-S", "", "b210"]])
            self.run_export(False)
            self.assertEqual(self.out.read_text(), "previous export")
        for broken in [b'"unterminated', b'1,2,3\r\n', b'"a"x,2,3,4,5,6,7\r\n']:
            self.log.write_bytes((",".join(HEADER)+"\r\n").encode()+broken)
            self.run_export(False)
            self.assertEqual(self.out.read_text(), "previous export")

    def test_export_cannot_overwrite_log_or_its_alias(self):
        self.write([["2026-11-06T12:00:00Z", "437000000", "S", "P", "DVB-S", "", "b210"]])
        original = self.log.read_bytes()
        for alias in ["same", "symlink", "hardlink"]:
            self.out = self.log if alias == "same" else Path(self.tmp.name, alias)
            if alias == "symlink":
                self.out.symlink_to(self.log)
            elif alias == "hardlink":
                os.link(self.log, self.out)
            self.run_export(False)
            self.assertEqual(self.log.read_bytes(), original)

    def test_edit_replaces_one_record_and_rejects_stale_data(self):
        rows = [["2026-11-06T12:00:00Z", "437000000", "First", "P", "DVB-S", "", "b210"],
                ["2026-11-06T13:00:00Z", "570000000", "Second", "Q", "DVB-T2", "", "b210"]]
        self.write(rows)
        original = self.log.read_bytes()
        replacement = Path(self.tmp.name, "replacement.csv")
        with replacement.open("w", newline="") as f:
            writer = csv.writer(f)
            writer.writerow(HEADER)
            writer.writerow(rows[1][:2] + ['Updated, "TV" é', "Provider\nTwo"] + rows[1][4:] +
                            ["", "8000000", "2/3", "321", "ONE-WAY"])
        failed = subprocess.run([self.probe, "--stale-edit", self.log, "1", replacement], capture_output=True)
        self.assertEqual(failed.returncode, 1)
        self.assertEqual(self.log.read_bytes(), original)
        subprocess.run([self.probe, "--edit", self.log, "1", replacement], check=True)
        with self.log.open(newline="") as f:
            edited = list(csv.DictReader(f))
        self.assertEqual(len(edited), 2)
        self.assertEqual(edited[0]["service_name"], "First")
        self.assertEqual(edited[1]["service_name"], 'Updated, "TV" é')
        self.assertEqual(edited[1]["provider_name"], "Provider\nTwo")
        self.assertEqual(edited[1]["atv_points"], "321")
        self.run_export()
        self.assertEqual(read_adif(self.out)[1]["APP_VERON_ATV_POINTS"], "321")


if __name__ == "__main__":
    unittest.main(verbosity=2)
