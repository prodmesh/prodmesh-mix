#!/usr/bin/env python3
"""T0-5: a UI crash must not touch program audio, and a restarted UI must be
back with full state in under 2 s.

Starts mixd on the simulated clock, repeatedly SIGKILLs the UI while
another client watches engine health, and times each UI restart to its
first snapshot. Exits non-zero on any xrun or a reconnect over 2 s.

    tools/t05_ui_isolation.py [--build build] [--cycles 10]
"""
import argparse, json, os, re, signal, socket, subprocess, sys, tempfile, time
import tempfile as _tf
# Never touch the user's projects or recent list.
os.environ.setdefault("PM_MIX_HOME", _tf.mkdtemp(prefix="pm-gate-"))

ap = argparse.ArgumentParser()
ap.add_argument("--build", default="build")
ap.add_argument("--cycles", type=int, default=10)
args = ap.parse_args()

name = f"pm-t05-{os.getpid()}"
mixd = subprocess.Popen([f"{args.build}/mixd", "--driver", "sim", "--socket", name],
                        stderr=subprocess.PIPE, text=True)
line = ""
while "running on" not in line:  # other lines (e.g. "recording take") first
    line = mixd.stderr.readline()
    if not line:
        raise SystemExit("mixd exited before it was running")
path = re.search(r"socket (\S+)", line).group(1)

def health():
    s = socket.socket(socket.AF_UNIX)
    s.connect(path)
    s.sendall(b'{"op":"hello","client":"t05","proto":1}\n')
    buf, deadline = b"", time.time() + 2
    while time.time() < deadline:
        buf += s.recv(65536)
        for l in buf.split(b"\n")[:-1]:
            d = json.loads(l)
            if d["type"] == "health":
                s.close()
                return d
    raise SystemExit("no health from mixd")

ui = f"{args.build}/ProdMeshMix.app/Contents/MacOS/ProdMeshMix"
times, failed = [], False
try:
    before = health()
    for i in range(args.cycles):
        t0 = time.time()
        p = subprocess.Popen([ui, "--socket", name], stderr=subprocess.PIPE,
                             text=True)
        ms = None
        while ms is None and time.time() - t0 < 5:
            l = p.stderr.readline()
            m = re.search(r"snapshot (\d+) ms", l)
            if m:
                ms = (time.time() - t0) * 1000
        times.append(ms)
        time.sleep(0.5)  # let it draw meters, send moves, etc.
        p.send_signal(signal.SIGKILL)
        p.wait()
    after = health()
finally:
    mixd.send_signal(signal.SIGTERM)
    mixd.wait(timeout=5)

xruns = after["xruns"] - before["xruns"]
worst = max(t for t in times if t is not None) if all(times) else None
print(f"cycles {args.cycles}  xruns during test {xruns:.0f}  "
      f"rt violations {after['rtViolations']:.0f}")
print("restart-to-snapshot ms: " +
      " ".join("timeout" if t is None else f"{t:.0f}" for t in times))
ok = xruns == 0 and after["rtViolations"] == 0 and worst is not None and worst < 2000
print("T0-5", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
