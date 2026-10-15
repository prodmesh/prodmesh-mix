#!/usr/bin/env python3
"""T0-7: a killed engine loses at most ~1 s of header bookkeeping, no audio
data, and the next start repairs every file.

Records with mixd on the simulated clock, SIGKILLs it, measures how far each
WAV header lags its data, restarts mixd, and checks the headers now cover
all the data. Exits non-zero on failure.

    tools/t07_crash_recording.py [--build build] [--seconds 4]
"""
import argparse, glob, os, re, shutil, signal, struct, subprocess, sys, tempfile, time
import tempfile as _tf
# Never touch the user's projects or recent list.
os.environ.setdefault("PM_MIX_HOME", _tf.mkdtemp(prefix="pm-gate-"))

ap = argparse.ArgumentParser()
ap.add_argument("--build", default="build")
ap.add_argument("--seconds", type=float, default=4)
args = ap.parse_args()

work = tempfile.mkdtemp(prefix="pm-t07-")
rec = os.path.join(work, "Recordings")
sess = os.path.join(work, "session.json")
DATA = 690  # data offset of our BWF layout (src/record/wav_writer.h)

def mixd():
    return subprocess.Popen([f"{args.build}/mixd", "--driver", "sim",
                             "--socket", f"pm-t07-{os.getpid()}", "--fresh",
                             "--session", sess, "--record", rec],
                            stderr=subprocess.PIPE, text=True)

def header(path):
    with open(path, "rb") as f:
        h = f.read(DATA)
    ch = struct.unpack_from("<H", h, 58)[0]
    rate = struct.unpack_from("<I", h, 60)[0]
    frames = struct.unpack_from("<I", h, 686)[0] // (4 * ch)
    actual = (os.path.getsize(path) - DATA) // (4 * ch)
    return rate, frames, actual

p = mixd()
time.sleep(args.seconds)
p.send_signal(signal.SIGKILL)
p.wait()
files = sorted(glob.glob(os.path.join(rec, "*", "*.wav")))
lags = []
for f in files:
    rate, frames, actual = header(f)
    lags.append((actual - frames) / rate)
print(f"after kill -9: {len(files)} files, data {actual / rate:.2f} s, "
      f"header lag max {max(lags):.2f} s")

p = mixd()
log = ""
deadline = time.time() + 5
while "running" not in log and time.time() < deadline:
    log += p.stderr.readline()
p.send_signal(signal.SIGTERM)
p.wait(timeout=10)
repaired = re.search(r"repaired (\d+)", log)
after = [header(f) for f in files]
complete = all(fr == ac for _, fr, ac in after)
print(f"restart: {log.strip().splitlines()[0] if log else 'no output'}")
print(f"headers complete after restart: {complete}")

ok = (len(files) > 0 and max(lags) <= 1.1 and complete and repaired
      and int(repaired.group(1)) == len(files))
print("T0-7", "PASS" if ok else "FAIL")
shutil.rmtree(work)
sys.exit(0 if ok else 1)
