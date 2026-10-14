#!/usr/bin/env python3
# Scenes end to end against a sim mixd: store, "modified" detection, recall
# with a fade, undo recall, edit, and persistence across a restart.
#   tools/scenes_e2e.py [--build build]
import argparse, socket, json, os, sys, tempfile, time, subprocess
ap = argparse.ArgumentParser(); ap.add_argument("--build", default="build")
args = ap.parse_args(); build = args.build
tmp = tempfile.mkdtemp(prefix="pm-scenes-")
sock = "pm-scenes-%d" % os.getpid(); sess = tmp + "/session.json"
sockdir = os.environ.get("TMPDIR", "/tmp/")
daemons=[]
import atexit
atexit.register(lambda: [d.kill() for d in daemons if d.poll() is None])
def start(fresh):
    args=[build+"/mixd","--driver","sim","--show","demo","--socket",sock,"--session",sess]+(["--fresh"] if fresh else [])
    p=subprocess.Popen(args,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,text=True)
    daemons.append(p)
    while "running on" not in p.stderr.readline(): pass
    return p
class C:
    def __init__(s):
        s.s=socket.socket(socket.AF_UNIX); s.s.connect(os.path.join(sockdir, sock)); s.buf=b""
        s.send({"op":"hello","client":"e2e","proto":1})
    def send(s,o): s.s.sendall((json.dumps(o)+"\n").encode())
    def wait(s,typ,pred=lambda m:True,timeout=5):
        end=time.time()+timeout; s.s.settimeout(0.2)
        while time.time()<end:
            while b"\n" in s.buf:
                l,s.buf=s.buf.split(b"\n",1); m=json.loads(l)
                if m.get("type")=="error": print("ERROR",m)
                if m.get("type")==typ and pred(m): return m
            try: s.buf+=s.s.recv(1<<20)
            except socket.timeout: pass
        raise SystemExit("timeout waiting for "+typ)
def node(snap,i): return [n for n in snap["graph"]["nodes"] if n["id"]==i][0]
def edge(snap,a,b): return [e for e in snap["graph"]["edges"] if e["from"]==a and e["to"]==b and not e.get("derived")][0]
ok=True
def check(c,msg):
    global ok; print(("ok   " if c else "FAIL ")+msg); ok&=bool(c)
d=start(True); c=C(); snap=c.wait("snapshot"); c.wait("scenes")
c.send({"op":"storeScene","name":"Pregame"}); m=c.wait("scenes",lambda m:m["scenes"])
check(m["current"]==1 and not m["modified"],"stored, current, unmodified")
g=snap["graph"]; [e for e in g["edges"] if e["from"]==2000 and e["to"]==3002][0]["pan"]=0.9
c.send({"op":"setGraph","graph":g}); c.wait("snapshot")
c.send({"op":"setLevel","id":2000,"faderDb":-12.0,"mute":False})
m=c.wait("scenes",lambda m:m["modified"]); check(True,"modified after a fader move")
c.send({"op":"recallScene","id":1,"fadeMs":300})
m=c.wait("scenes",lambda m:not m["modified"],timeout=3); check(m["canUndo"],"recall -> unmodified, undo available")
snap=c.wait("snapshot",lambda m:node(m,2000)["faderDb"]==0)
check(edge(snap,2000,3002)["pan"]!=0.9,"pan restored")
c.send({"op":"undoRecall"}); snap=c.wait("snapshot",lambda m:node(m,2000)["faderDb"]==-12)
check(abs(edge(snap,2000,3002)["pan"]-0.9)<1e-6,"undo brings the pre-recall mix back")
# Undo also puts back which scene was live.
c.send({"op":"storeScene","name":"Game"}); c.wait("scenes",lambda m:m["current"]==2)
c.send({"op":"recallScene","id":1}); c.wait("scenes",lambda m:m["current"]==1 and m["canUndo"])
c.send({"op":"undoRecall"})
m=c.wait("scenes",lambda m:not m["canUndo"])
check(m["current"]==2,"undo makes the previous scene live again")
m=c.wait("scenes",lambda m:not m["modified"]) if m["modified"] else m
check(not m["modified"],"... and unmodified")
c.send({"op":"editScene","id":1,"name":"Pre-game","fadeMs":1500}); c.wait("scenes",lambda m:m["scenes"][0]["name"]=="Pre-game")
time.sleep(0.8); d.terminate(); d.wait()
d=start(False); c=C(); m=c.wait("scenes")
check(m["scenes"][0]=={"id":1,"name":"Pre-game","fadeMs":1500} and len(m["scenes"])==2,"scenes survive a restart")
d.terminate(); d.wait()
print("PASS" if ok else "FAILED")
