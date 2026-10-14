#!/usr/bin/env python3
"""Runs real installed plugins through the engine as live inserts (via
`mix-plugin bench`, one process each) and prints a table. A crash, hang,
non-finite output or load warning fails the run.

    tools/plugin_smoke.py [--build build] [--catalog scan.jsonl] [NAME...]

Without --catalog it scans first. NAMEs are substrings of plugin names;
without them, a default set of widely used effects is tried (whatever of it
is installed).
"""
import argparse, json, re, subprocess, sys, tempfile

# FabFilter (licensed on the dev machine) and MiniMeters (free). Loading
# other vendors' plugins can pop up licence dialogs (iLok etc.) on the
# owner's screen — name them explicitly when you mean to.
DEFAULT = ["Pro-Q 3", "Pro-Q 4", "Pro-C 2", "Pro-L 2", "Pro-MB", "Pro-DS",
           "Pro-G", "Pro-R 2", "Saturn 2", "Timeless 3", "MiniMeters - Loudness"]

ap = argparse.ArgumentParser()
ap.add_argument("--build", default="build")
ap.add_argument("--catalog")
ap.add_argument("names", nargs="*")
args = ap.parse_args()

if args.catalog:
    lines = open(args.catalog).read().splitlines()
else:
    lines = subprocess.run([f"{args.build}/mix-plugin", "scan"],
                           capture_output=True, text=True).stdout.splitlines()
rows = [json.loads(l) for l in lines if l.startswith("{") and "quarantined" not in l]

picks = []
for want in args.names or DEFAULT:
    for fmt in ("clap", "vst3"):
        hit = next((r for r in rows if r["format"] == fmt and want in r["name"]), None)
        if hit:
            picks.append(hit)

failed = 0
print(f"{'plugin':<26}{'fmt':<6}{'result':<10}{'latency':>8}{'cpu/inst':>10}")
for r in picks:
    try:
        p = subprocess.run([f"{args.build}/mix-plugin", "bench", r["path"], r["id"]],
                           capture_output=True, text=True, timeout=120)
        out, rc = p.stdout + p.stderr, p.returncode
    except subprocess.TimeoutExpired:
        out, rc = "", "timeout"
    lat = re.search(r"latency (\d+)", out)
    cpu = re.search(r"\(([\d.]+)% of a core\)", out)
    ok = rc == 0
    failed += not ok
    result = "ok" if ok else ("crash" if isinstance(rc, int) and rc < 0 else str(rc))
    print(f"{r['name'][:25]:<26}{r['format']:<6}{result:<10}"
          f"{lat.group(1) if lat else '-':>8}{(cpu.group(1) + '%') if cpu else '-':>10}")
print(f"{len(picks) - failed}/{len(picks)} ok")
sys.exit(1 if failed else 0)
