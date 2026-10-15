#!/usr/bin/env python3
# Audio Setup against a sim mixd: applying settings saves audio.json and
# restarts the engine in place (same process, same arguments) with them;
# clients are told first; the take is finalised and the show comes back;
# settings given on the command line cannot be changed from the app.
#   tools/audio_setup_e2e.py [--build build]
import argparse, atexit, glob, json, os, shutil, socket, struct, subprocess, sys, tempfile, time

ap = argparse.ArgumentParser(); ap.add_argument("--build", default="build")
build = ap.parse_args().build
home = tempfile.mkdtemp(prefix="pm-audiosetup-")
atexit.register(shutil.rmtree, home, True)
sock = "pm-audiosetup-%d" % os.getpid()
sockdir = os.environ.get("TMPDIR", "/tmp/")
env = dict(os.environ, PM_MIX_HOME=home)
args = [build + "/mixd", "--driver", "sim", "--socket", sock, "--project", home + "/Projects/Show"]
eng = subprocess.Popen(args, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
atexit.register(lambda: eng.poll() is None and eng.kill())

ok = True
def check(c, msg):
    global ok; print(("ok   " if c else "FAIL ") + msg); ok &= bool(c)

class Conn:
    def __init__(self, timeout=10):
        end = time.time() + timeout
        while True:
            try:
                self.s = socket.socket(socket.AF_UNIX)
                self.s.connect(os.path.join(sockdir, sock)); break
            except OSError:
                if time.time() > end: raise
                time.sleep(0.1)
        self.buf = b""
        self.send({"op": "hello", "client": "e2e", "proto": 1})
    def send(self, o): self.s.sendall((json.dumps(o) + "\n").encode())
    def wait(self, kind, timeout=5):
        end = time.time() + timeout
        self.s.settimeout(0.2)
        while time.time() < end:
            lines = self.buf.split(b"\n")
            self.buf = lines[-1]
            for l in lines[:-1]:
                m = json.loads(l)
                if m.get("type") == kind: return m
            try:
                d = self.s.recv(1 << 20)
                if not d: return None
                self.buf += d
            except socket.timeout: pass
        return None

c = Conn()
snap = c.wait("snapshot")
check(snap and snap["engine"]["block"] == 128 and snap["engine"]["sampleRate"] == 48000,
      "starts at 48 kHz / 128")
c.send({"op": "audioSetup"})
a = c.wait("audioSetup")
check(a and a["driver"] == "sim" and a["locked"] == [] and a["running"]["block"] == 128,
      "audioSetup reports the driver, nothing locked, what is running")

c.send({"op": "applyAudio", "rate": 44100, "block": 100000})
e = c.wait("error")
check(e and "out of range" in e["message"], "a bad buffer size is refused")

c.send({"op": "applyAudio", "rate": 96000, "block": 256})
check(c.wait("restarting") is not None, "clients are told before the restart")
time.sleep(0.3)
saved = json.load(open(home + "/audio.json"))
check(saved.get("rate") == 96000 and saved.get("block") == 256,
      "audio.json saved in the app data folder (PM_MIX_HOME)")
c2 = Conn()
snap = c2.wait("snapshot")
check(snap and snap["engine"]["sampleRate"] == 96000 and snap["engine"]["block"] == 256,
      "the engine comes back at 96 kHz / 256")
check(eng.poll() is None, "... in the same process (pid %d)" % eng.pid)
takes = sorted(glob.glob(home + "/Projects/Show/Takes/*"))
def complete(p):
    b = open(p, "rb").read()
    i = b.find(b"data"); n = struct.unpack("<I", b[i + 4:i + 8])[0]
    return n not in (0, 0xFFFFFFFF) and i + 8 + n == len(b)
first = glob.glob(takes[0] + "/*.wav") if takes else []
check(len(takes) == 2 and first and all(complete(w) for w in first),
      "the first take was finalised and a new one started (%d takes)" % len(takes))

# Stop it; start it with --block on the command line: locked.
c2.send({"op": "shutdown", "client": "the test"}); eng.wait(timeout=5)
eng = subprocess.Popen(args + ["--block", "64"], env=env,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
c3 = Conn()
snap = c3.wait("snapshot")
check(snap and snap["engine"]["block"] == 64 and snap["engine"]["sampleRate"] == 96000,
      "the command line wins over audio.json (64), the saved rate still applies")
c3.send({"op": "audioSetup"})
a = c3.wait("audioSetup")
check(a and a["locked"] == ["block"], "audioSetup says the buffer is locked")
c3.send({"op": "applyAudio", "block": 512})
e = c3.wait("error")
check(e and "--block" in e["message"], "changing it from the app is refused")
c3.send({"op": "shutdown", "client": "the test"}); eng.wait(timeout=5)
print("PASS" if ok else "FAILED"); sys.exit(0 if ok else 1)
