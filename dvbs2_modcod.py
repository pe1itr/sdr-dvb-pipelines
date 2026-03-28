#!/usr/bin/env python3
"""
dvbs2_modcod.py

Zoek DVB-S2 MODCOD(s) op basis van constellatie + FEC of FEC-range.

Voorbeelden:
  python3 dvbs2_modcod.py QPSK 1/2
  python3 dvbs2_modcod.py QPSK 1/4-3/4
  python3 dvbs2_modcod.py QPSK 1/2-9/10
  python3 dvbs2_modcod.py 8PSK 3/5-9/10
  python3 dvbs2_modcod.py --list
"""

from __future__ import annotations

import argparse
import sys
from fractions import Fraction
from typing import Dict, List, Tuple


MODCOD_TABLE: Dict[int, Tuple[str, str]] = {
    1:  ("QPSK",   "1/4"),
    2:  ("QPSK",   "1/3"),
    3:  ("QPSK",   "2/5"),
    4:  ("QPSK",   "1/2"),
    5:  ("QPSK",   "3/5"),
    6:  ("QPSK",   "2/3"),
    7:  ("QPSK",   "3/4"),
    8:  ("QPSK",   "4/5"),
    9:  ("QPSK",   "5/6"),
    10: ("QPSK",   "8/9"),
    11: ("QPSK",   "9/10"),
    12: ("8PSK",   "3/5"),
    13: ("8PSK",   "2/3"),
    14: ("8PSK",   "3/4"),
    15: ("8PSK",   "5/6"),
    16: ("8PSK",   "8/9"),
    17: ("8PSK",   "9/10"),
    18: ("16APSK", "2/3"),
    19: ("16APSK", "3/4"),
    20: ("16APSK", "4/5"),
    21: ("16APSK", "5/6"),
    22: ("16APSK", "8/9"),
    23: ("16APSK", "9/10"),
    24: ("32APSK", "3/4"),
    25: ("32APSK", "4/5"),
    26: ("32APSK", "5/6"),
    27: ("32APSK", "8/9"),
    28: ("32APSK", "9/10"),
}

CONSTELLATION_ALIASES = {
    "QPSK": "QPSK",
    "4PSK": "QPSK",
    "8PSK": "8PSK",
    "PSK8": "8PSK",
    "16APSK": "16APSK",
    "APSK16": "16APSK",
    "32APSK": "32APSK",
    "APSK32": "32APSK",
}


def normalize_constellation(value: str) -> str:
    s = value.strip().upper().replace("-", "").replace("_", "").replace(" ", "")
    if s not in CONSTELLATION_ALIASES:
        raise ValueError(f"Onbekende constellatie: {value}")
    return CONSTELLATION_ALIASES[s]


def normalize_fec(value: str) -> str:
    s = value.strip().replace(" ", "")
    parts = s.split("/")
    if len(parts) != 2:
        raise ValueError(f"FEC moet NUM/DEN zijn, bijvoorbeeld 3/4: {value}")
    try:
        num = int(parts[0])
        den = int(parts[1])
    except ValueError as exc:
        raise ValueError(f"FEC bevat geen geldige getallen: {value}") from exc

    if num <= 0 or den <= 0:
        raise ValueError(f"FEC moet positief zijn: {value}")

    return f"{num}/{den}"


def fec_fraction(fec: str) -> Fraction:
    n, d = fec.split("/")
    return Fraction(int(n), int(d))


def mask_for_modcod(modcod: int) -> int:
    return 1 << modcod


def get_modcods_for_constellation(constellation: str) -> List[Tuple[int, str]]:
    rows = []
    for modcod in sorted(MODCOD_TABLE):
        const, fec = MODCOD_TABLE[modcod]
        if const == constellation:
            rows.append((modcod, fec))
    return rows


def lookup_single(constellation: str, fec: str) -> List[int]:
    result = []
    for modcod, (const, f) in MODCOD_TABLE.items():
        if const == constellation and f == fec:
            result.append(modcod)
    return sorted(result)


def lookup_range(constellation: str, fec_start: str, fec_end: str) -> List[int]:
    start = fec_fraction(fec_start)
    end = fec_fraction(fec_end)
    low = min(start, end)
    high = max(start, end)

    result = []
    for modcod, fec in get_modcods_for_constellation(constellation):
        f = fec_fraction(fec)
        if low <= f <= high:
            result.append(modcod)
    return sorted(result)


def combined_mask(modcods: List[int]) -> int:
    m = 0
    for modcod in modcods:
        m |= mask_for_modcod(modcod)
    return m


def parse_fec_spec(constellation: str, fec_spec: str) -> List[int]:
    spec = fec_spec.strip().replace(" ", "")

    if "-" in spec:
        a, b = spec.split("-", 1)
        a = normalize_fec(a)
        b = normalize_fec(b)
        return lookup_range(constellation, a, b)

    fec = normalize_fec(spec)
    return lookup_single(constellation, fec)


def print_result(constellation: str, modcods: List[int]) -> None:
    if not modcods:
        print("Geen matches gevonden.")
        return

    total_mask = combined_mask(modcods)

    print(f"Constellation : {constellation}")
    print("Matches:")
    for modcod in modcods:
        const, fec = MODCOD_TABLE[modcod]
        mask = mask_for_modcod(modcod)
        print(f"  MODCOD {modcod:2d}  {const:7s} {fec:4s}  mask=0x{mask:X}")

    print()
    print(f"Combined mask dec : {total_mask}")
    print(f"Combined mask hex : 0x{total_mask:X}")
    print(f"LeanDVB           : --modcods 0x{total_mask:X}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("constellation", nargs="?")
    parser.add_argument("fec", nargs="?")
    parser.add_argument("--list", action="store_true")
    args = parser.parse_args()

    if args.list:
        print("MODCOD  CONST    FEC    MASK")
        for modcod in sorted(MODCOD_TABLE):
            const, fec = MODCOD_TABLE[modcod]
            print(f"{modcod:2d}      {const:7s}  {fec:4s}   0x{mask_for_modcod(modcod):X}")
        return 0

    if not args.constellation or not args.fec:
        parser.print_help()
        return 1

    try:
        constellation = normalize_constellation(args.constellation)
        modcods = parse_fec_spec(constellation, args.fec)
    except Exception as e:
        print(f"Fout: {e}", file=sys.stderr)
        return 1

    print_result(constellation, modcods)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
