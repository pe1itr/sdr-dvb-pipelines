#!/usr/bin/env python3
import sys
import re
import time
from datetime import datetime, timezone
from pathlib import Path

BASE_DIR = Path.cwd()
LATEST_FILE = BASE_DIR / "tsp_latest.txt"
LOG_FILE = BASE_DIR / "service_provider.log"
SUPPRESS_SECONDS = 3600

SERVICE_RE = re.compile(r"Service name:\s*(.*?),\s*provider:\s*(.*)\s*$")
last_logged = {}

def utc_now_str():
    return datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")

def debug(msg):
    print(msg, file=sys.stderr, flush=True)

def log_service_provider(service, provider):
    key = (service, provider)
    now = time.time()
    prev = last_logged.get(key)

    if prev is None or (now - prev) >= SUPPRESS_SECONDS:
        with open(LOG_FILE, "a", encoding="utf-8") as f:
            f.write(f"{utc_now_str()} service=\"{service}\" provider=\"{provider}\"\n")
        last_logged[key] = now
        debug(f"logged service/provider: {service} / {provider}")

def process_block(block_lines):
    if not block_lines:
        return

    block_text = "".join(block_lines).strip()
    if not block_text:
        return

    with open(LATEST_FILE, "w", encoding="utf-8") as f:
        f.write(block_text + "\n")

    debug(f"updated {LATEST_FILE}")

    for line in block_text.splitlines():
        m = SERVICE_RE.search(line)
        if m:
            service = m.group(1).strip()
            provider = m.group(2).strip()
            if service and provider:
                log_service_provider(service, provider)
            break

def main():
    debug(f"tsp_monitor started in {BASE_DIR}")

    block_lines = []
    in_block = False

    for line in sys.stdin:
        # begin van een nieuw analyze-blok
        if line.startswith("==============================================================================="):
            if block_lines:
                process_block(block_lines)
                block_lines = []
            in_block = True

        if in_block:
            block_lines.append(line)

    # rest bij EOF
    if block_lines:
        process_block(block_lines)

if __name__ == "__main__":
    main()
