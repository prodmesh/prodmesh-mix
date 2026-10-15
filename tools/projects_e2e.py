#!/usr/bin/env python3
# Projects end to end against a sim mixd (PM_MIX_HOME keeps it away from
# the user's Music folder and recent list): first run offers Start once;
# new project from a template records into its own Takes/; opening
# another project brings its mix and scenes back; neither project's file
# picks up the other's scenes; the recent list follows.
#   tools/projects_e2e.py [--build build]
import argparse, atexit, json, os, socket, subprocess, sys, tempfile, time

ap = argparse.ArgumentParser(); ap.add_argument("--build", default="build")
build = ap.parse_args().build
home = tempfile.mkdtemp(prefix="pm-projects-")
import atexit, shutil
atexit.register(shutil.rmtree, home, True)  # recordings add up
sock = "pm-projects-%d" % os.getpid()
sockdir = os.environ.get("TMPDIR", "/tmp/")
env = dict(os.environ, PM_MIX_HOME=home)
daemons = []
atexit.register(lambda: [d.kill() for d in daemons if d.poll() is None])

def start():
    d = subprocess.Popen([build + "/mixd", "--driver", "sim", "--socket", sock],
                         env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    daemons.append(d)
    while "running on" not in d.stderr.readline(): pass
    return d

class C:
    def __init__(s, client="mix-ui"):
        s.s = socket.socket(socket.AF_UNIX); s.s.connect(os.path.join(sockdir, sock)); s.buf = b""
        s.send({"op": "hello", "client": client, "proto": 1})
    def send(s, o): s.s.sendall((json.dumps(o) + "\n").encode())
    def wait(s, typ, pred=lambda m: True, timeout=8):
        end = time.time() + timeout; s.s.settimeout(0.2)
        while time.time() < end:
            while b"\n" in s.buf:
                l, s.buf = s.buf.split(b"\n", 1); m = json.loads(l)
                if m.get("type") == "error": print("  error:", m.get("message"))
                if m.get("type") == typ and pred(m): return m
            try: s.buf += s.s.recv(1 << 20)
            except socket.timeout: pass
        raise SystemExit("timeout waiting for " + typ)

ok = True
def check(c, msg):
    global ok; print(("ok   " if c else "FAIL ") + msg); ok &= bool(c)
def strips(snap): return [n["name"] for n in snap["graph"]["nodes"] if n["kind"] == "strip"]

d = start()
a = C(); p = a.wait("project")
check(p["offerStart"] and p["name"] == "Untitled", "first run: Untitled, Start offered")
b = C(); check(not b.wait("project")["offerStart"], "... offered once (a reconnecting UI never gets it)")

a.send({"op": "storeScene", "name": "A scene"}); a.wait("scenes", lambda m: m["scenes"])
a.send({"op": "newProject", "name": "Sunday", "template": "livestream", "record": True})
p = a.wait("project", lambda m: m["name"] == "Sunday")
check(a.wait("scenes")["scenes"] == [], "a new project starts without scenes")
snap = a.wait("snapshot", lambda m: "Host" in strips(m))
check(p["record"] and os.path.isdir(home + "/Projects/Sunday/Takes"), "new project records into its Takes/")
time.sleep(0.5)
a.send({"op": "storeScene", "name": "Sunday scene"}); a.wait("scenes", lambda m: m["scenes"])

a.send({"op": "openProject", "path": home + "/Projects/Untitled"})
a.wait("project", lambda m: m["name"] == "Untitled")
m = a.wait("scenes", lambda m: m["scenes"] and m["scenes"][0]["name"] == "A scene")
check(len(m["scenes"]) == 1, "opening a project brings its own scenes back")
time.sleep(0.8)
for name, want in (("Untitled", "A scene"), ("Sunday", "Sunday scene")):
    doc = json.load(open(home + "/Projects/%s/session.json" % name))
    check([x["name"] for x in doc["scenes"]] == [want], name + "'s file holds only its own scenes")

a.send({"op": "projects"}); pr = a.wait("projects")
check([r["name"] for r in pr["recent"]][:2] == ["Untitled", "Sunday"], "recent list, newest first")
a.send({"op": "newProject", "name": "Sunday", "template": "blank"})
a.wait("error"); check(True, "an existing name is refused")
a.send({"op": "openProject", "path": home}); a.wait("error"); check(True, "a non-project folder is refused")

d.terminate(); d.wait()
d = start(); p = C().wait("project")
check(p["name"] == "Untitled" and not p["offerStart"], "restart reopens the last project, no Start")
print("PASS" if ok else "FAILED"); sys.exit(0 if ok else 1)
