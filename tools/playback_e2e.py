#!/usr/bin/env python3
# Playback (virtual soundcheck) end to end against a sim mixd: record a take,
# play it back through the mix, locate, stop, and go back to live (a new
# take starts). PM_MIX_HOME keeps it away from the user's projects.
#   tools/playback_e2e.py [--build build]
import argparse, atexit, json, os, socket, subprocess, sys, tempfile, time

ap = argparse.ArgumentParser(); ap.add_argument("--build", default="build")
build = ap.parse_args().build
home = tempfile.mkdtemp(prefix="pm-playback-")
import atexit, shutil
atexit.register(shutil.rmtree, home, True)  # recordings add up
sock = "pm-playback-%d" % os.getpid()
sockdir = os.environ.get("TMPDIR", "/tmp/")
env = dict(os.environ, PM_MIX_HOME=home)
d = subprocess.Popen([build + "/mixd", "--driver", "sim", "--socket", sock, "--show", "demo",
                      "--project", home + "/Projects/Show"],
                     env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
atexit.register(lambda: d.poll() is None and d.kill())
while "running on" not in d.stderr.readline(): pass

s = socket.socket(socket.AF_UNIX); s.connect(os.path.join(sockdir, sock)); buf = b""
def send(o): s.sendall((json.dumps(o) + "\n").encode())
def wait(typ, pred=lambda m: True, timeout=8):
    global buf
    end = time.time() + timeout; s.settimeout(0.2)
    while time.time() < end:
        while b"\n" in buf:
            l, buf = buf.split(b"\n", 1); m = json.loads(l)
            if m.get("type") == "error": print("  error:", m.get("message"))
            if m.get("type") == typ and pred(m): return m
        try: buf += s.recv(1 << 20)
        except socket.timeout: pass
    raise SystemExit("timeout waiting for " + typ)
def latest(typ):
    """The newest message of a type (the 15 Hz stream queues up while we sleep)."""
    global buf
    s.settimeout(0.02)
    end = time.time() + 0.2  # the streams never pause: drain for a moment
    while time.time() < end:
        try: buf += s.recv(1 << 20)
        except socket.timeout: pass
    last = None
    keep = b""
    for l in buf.split(b"\n")[:-1]:
        m = json.loads(l)
        if m.get("type") == typ: last = m
    buf = buf[buf.rfind(b"\n") + 1:]
    return last or wait(typ)
ok = True
def check(c, msg):
    global ok; print(("ok   " if c else "FAIL ") + msg); ok &= bool(c)

send({"op": "hello", "client": "e2e", "proto": 1})
h = wait("health", lambda m: m["recording"].get("on"))
take = h["recording"]["take"]
time.sleep(4)  # record a few seconds

send({"op": "enterPlayback", "take": take})
t = wait("transport", lambda m: m["mode"] == "playback")
check(t["take"] == take and t["length"] > 3.5 and t["tracks"] >= 8, "playback of the take (length %.1f s)" % t["length"])
check(not wait("health")["recording"]["on"], "recording stopped")

send({"op": "transport", "action": "play"})
wait("transport", lambda m: m["playing"])
if os.environ.get("PM_DEBUG"):
    for _ in range(12):
        m = wait("transport"); print("   debug", {k: m[k] for k in ("playing","position","underruns")})
time.sleep(1.5)
t = latest("transport")
check(1.0 < t["position"] < 2.5 and t["underruns"] == 0, "plays: position %.2f s, %d underruns" % (t["position"], t["underruns"]))
mt = latest("meters")
prog = [r for r in mt["m"] if r[0] == 3000][0]
check(max(prog[1], prog[2]) > -40, "Program carries the replay (%.0f dBFS)" % max(prog[1], prog[2]))

send({"op": "transport", "action": "locate", "seconds": 0.5})
time.sleep(0.5)
t = latest("transport")
check(0.5 <= t["position"] < 1.6, "locate while playing resumes there (%.2f s)" % t["position"])

send({"op": "transport", "action": "stop"})
time.sleep(0.3)
a = latest("transport")["position"]; time.sleep(0.5); b = latest("transport")["position"]
check(a == b, "stop holds the position")

send({"op": "transport", "action": "locate", "seconds": t["length"] - 0.3})
send({"op": "transport", "action": "play"})
time.sleep(1.2)
t = latest("transport")
check(not t["playing"] and t["length"] - 0.1 <= t["position"] <= t["length"] + 0.2,
      "play right after a locate plays, and stops at the end (%.2f of %.2f s)" % (t["position"], t["length"]))

send({"op": "exitPlayback"})
wait("transport", lambda m: m["mode"] == "live")
h = wait("health", lambda m: m["recording"].get("on"))
check(h["recording"]["take"] != take, "back to live: a new take is recording")
time.sleep(0.6)
h = latest("health")
# Shared CI machines (CI=true) have no real-time guarantees: dropouts are
# reported there, not judged. RT violations are code bugs and always fail.
strict = not os.environ.get("CI")
check(h["rtViolations"] == 0 and (h["xruns"] == 0 or not strict),
      "Tier 0: no dropouts through it all (%d xruns%s, %d RT violations)"
      % (h["xruns"], "" if strict else ", not judged on CI", h["rtViolations"]))
print("PASS" if ok else "FAILED"); sys.exit(0 if ok else 1)
