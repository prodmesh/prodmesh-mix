#!/usr/bin/env python3
# Builds a project from a folder of multitrack WAVs, to try the console on
# real material without a band: the files become a finished take (as if
# recorded from that many device inputs) and the show gets one channel per
# file, a bus per section, and Program -> Out 1-2. Open it, then Editor >
# "Play this take" (virtual soundcheck): everything goes through the mix.
#
# Stereo files arrive as two mono inputs (a stereo channel on a pair, as
# on a real console). Names like "Song _ STR _ Violas" (stem exports) give
# the section (STR -> Strings) and the channel name (Violas); others use
# the file name and no sections. Recording is off in the new project, so
# opening it never fills the disk with silent inputs.
#
#   tools/import_multitracks.py SRC_DIR PROJECT_DIR [--build build]
import argparse, datetime, json, os, re, subprocess, sys, wave

ap = argparse.ArgumentParser()
ap.add_argument("src")
ap.add_argument("project")
ap.add_argument("--build", default=os.path.join(os.path.dirname(__file__), "..", "build"))
a = ap.parse_args()

SECTIONS = {"WW": "Woodwinds", "BRS": "Brass", "PRC": "Percussion", "CHR": "Choir",
            "STR": "Strings", "KEY": "Keys", "DRM": "Drums", "VOX": "Vocals"}
# Score order, then anything else alphabetically.
ORDER = ["piccolo", "flutes", "oboes", "english horn", "clarinets", "bass clarinet",
         "bassoon", "contrabassoon", "horns", "trumpets", "trombones", "tuba",
         "timpani", "glockenspiel", "celeste", "harp", "piano", "women", "men",
         "1st violins", "2nd violins", "violas", "cellos", "basses"]
SECTION_ORDER = ["WW", "BRS", "PRC", "KEY", "CHR", "STR"]

if os.path.exists(a.project):
    sys.exit("refusing to overwrite " + a.project)
tool = os.path.join(a.build, "mix-import")
if not os.path.exists(tool):
    sys.exit("build mix-import first (cmake --build build)")

stems = []
for f in sorted(os.listdir(a.src)):
    if not f.lower().endswith(".wav"):
        continue
    parts = [p.strip() for p in os.path.splitext(f)[0].split(" _ ")]
    code, name = (parts[-2], parts[-1]) if len(parts) >= 3 else ("", parts[-1])
    try:
        with wave.open(os.path.join(a.src, f)) as w:
            ch, rate = w.getnchannels(), w.getframerate()
    except wave.Error:  # float WAVs: read the fmt chunk ourselves
        b = open(os.path.join(a.src, f), "rb").read(4096)
        i = b.find(b"fmt ")
        ch, rate = int.from_bytes(b[i + 10:i + 12], "little"), int.from_bytes(b[i + 12:i + 16], "little")
    stems.append({"file": f, "code": code, "name": name, "channels": ch, "rate": rate})
    if ch > 2:
        sys.exit(f + ": %d channels; mono and stereo files only" % ch)
if not stems:
    sys.exit("no WAV files in " + a.src)
rates = {s["rate"] for s in stems}
if len(rates) != 1:
    sys.exit("files at different sample rates: %s" % sorted(rates))

def key(s):
    sec = SECTION_ORDER.index(s["code"]) if s["code"] in SECTION_ORDER else len(SECTION_ORDER)
    n = s["name"].lower()
    return (sec, s["code"], ORDER.index(n) if n in ORDER else len(ORDER), n)
stems.sort(key=key)

PROGRAM, OUT = 3000, 4000
nodes = [{"id": PROGRAM, "kind": "bus", "channels": 2, "name": "Program",
          "processing": {"limiter": {"on": True, "ceilingDb": -1}}},
         {"id": OUT, "kind": "output", "channels": 2, "ioChannel": 0, "name": "Program out"}]
edges = [{"from": PROGRAM, "to": OUT, "gainDb": 0, "pan": 0}]
buses = {}
for s in stems:
    if s["code"] and s["code"] not in buses:
        bid = 3100 + len(buses)
        buses[s["code"]] = bid
        nodes.append({"id": bid, "kind": "bus", "channels": 2,
                      "name": SECTIONS.get(s["code"], s["code"])})
        edges.append({"from": bid, "to": PROGRAM, "gainDb": 0, "pan": 0})

name = os.path.basename(os.path.normpath(a.project))
take = name + " (imported)"
takedir = os.path.join(a.project, "Takes", take)
os.makedirs(takedir)
tracks, ch = [], 0
for k, s in enumerate(stems):
    strip = 2000 + k
    nodes.append({"id": strip, "kind": "strip", "channels": s["channels"], "name": s["name"]})
    edges.append({"from": strip, "to": buses.get(s["code"], PROGRAM), "gainDb": 0, "pan": 0})
    sides = ["L", "R"] if s["channels"] == 2 else [""]
    outs = []
    for i, side in enumerate(sides):
        node = 1000 + ch
        nodes.append({"id": node, "kind": "input", "channels": 1, "ioChannel": ch,
                      "name": "In %d" % (ch + 1)})
        edges.append({"from": node, "to": strip, "gainDb": 0,
                      "pan": (-1 if i == 0 else 1) if len(sides) == 2 else 0})
        label = (s["name"] + " " + side).strip()
        fname = "%02d %s.wav" % (ch + 1, label.replace("/", "-"))
        outs.append(os.path.join(takedir, fname))
        tracks.append({"channels": 1, "file": fname, "name": label, "node": node,
                       "role": "source"})
        ch += 1
    src = os.path.join(a.src, s["file"])
    subprocess.run([tool, src] + outs, check=True, stdout=subprocess.DEVNULL)

rate = rates.pop()
json.dump({"format": "prodmesh-mix-take", "take": take,
           "started": datetime.datetime.now().isoformat(timespec="milliseconds"),
           "sampleRate": rate, "tracks": tracks},
          open(os.path.join(takedir, "take.json"), "w"), indent=4)
json.dump({"format": "prodmesh-mix-session", "version": 1, "graph": {"nodes": nodes, "edges": edges},
           "scenes": [], "currentScene": 0, "loudness": {"target": -24, "truePeak": -2},
           "record": False},
          open(os.path.join(a.project, "session.json"), "w"), indent=4)
print("%s: %d channels from %d inputs, %d section buses, take \"%s\" at %d Hz"
      % (a.project, len(stems), ch, len(buses), take, rate))
