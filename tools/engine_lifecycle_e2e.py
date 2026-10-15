#!/usr/bin/env python3
# The engine's lifecycle rules, headless (the app relies on them): one
# engine per socket — a second refuses and leaves the first untouched — and
# a deliberate shutdown warns every client, then exits with the take's
# files finalised.
#   tools/engine_lifecycle_e2e.py [--build build]
import argparse, atexit, glob, json, os, shutil, socket, struct, subprocess, sys, tempfile, time

ap = argparse.ArgumentParser(); ap.add_argument("--build", default="build")
build = ap.parse_args().build
home = tempfile.mkdtemp(prefix="pm-lifecycle-")
atexit.register(shutil.rmtree, home, True)
sock = "pm-lifecycle-%d" % os.getpid()
sockdir = os.environ.get("TMPDIR", "/tmp/")
env = dict(os.environ, PM_MIX_HOME=home)
args = [build + "/mixd", "--driver", "sim", "--socket", sock, "--project", home + "/Projects/Show"]
first = subprocess.Popen(args, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
atexit.register(lambda: first.poll() is None and first.kill())
while "running on" not in first.stderr.readline(): pass

ok = True
def check(c, msg):
    global ok; print(("ok   " if c else "FAIL ") + msg); ok &= bool(c)

second = subprocess.run(args, env=env, capture_output=True, text=True, timeout=20)
check(second.returncode == 3 and "already running" in second.stderr,
      "a second engine refuses to start (exit %d)" % second.returncode)
check(first.poll() is None, "... and the first keeps running")

s = socket.socket(socket.AF_UNIX); s.connect(os.path.join(sockdir, sock)); buf = b""
s.sendall(b'{"op":"hello","client":"e2e","proto":1}\n')
time.sleep(1.5)  # record a little
s.sendall(b'{"op":"shutdown","client":"the test"}\n')
s.settimeout(3); got = None; end = time.time() + 3
while time.time() < end and got is None:
    try: buf += s.recv(1 << 20)
    except socket.timeout: break
    for l in buf.split(b"\n")[:-1]:
        m = json.loads(l)
        if m.get("type") == "shutdown": got = m
check(got is not None, "clients are told before it stops")
try: first.wait(timeout=2); exited = True
except subprocess.TimeoutExpired: exited = False
check(exited and first.returncode == 0, "it exits cleanly within 2 s")
wavs = glob.glob(home + "/Projects/Show/Takes/*/*.wav")
def complete(p):  # header covers the data: the take was finalised
    b = open(p, "rb").read()
    i = b.find(b"data"); n = struct.unpack("<I", b[i + 4:i + 8])[0]
    return n not in (0, 0xFFFFFFFF) and i + 8 + n == len(b)
check(wavs and all(complete(w) for w in wavs), "the take's %d files are finalised" % len(wavs))
print("PASS" if ok else "FAILED"); sys.exit(0 if ok else 1)
