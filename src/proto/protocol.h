// Control protocol between the engine daemon (mixd) and its clients.
//
// Transport: a QLocalSocket (Unix domain socket / named pipe). Framing: one
// compact JSON object per line. The daemon is the single source of truth;
// a client that (re)connects sends `hello` and receives a full `snapshot`.
//
// client -> daemon
//   {"op":"hello","client":"mix-ui","proto":1}
//   {"op":"setLevel","id":2001,"faderDb":-3.5,"mute":false}
//   {"op":"setGraph","graph":{...}}
//   {"op":"openEditor","id":2000,"insert":0}   isolated plugins only
//   {"op":"storeScene","name":"Pregame","fadeMs":0}   current mix -> new scene
//   {"op":"updateScene","id":2}         current mix -> scene 2
//   {"op":"recallScene","id":2}         (optional "fadeMs" overrides the scene's)
//   {"op":"undoRecall"}                 back to the mix before the last recall
//   {"op":"editScene","id":2,"name":"Game","fadeMs":2000}
//   {"op":"moveScene","id":2,"to":0}   {"op":"deleteScene","id":2}
//   {"op":"resetLoudness"}   {"op":"setLoudnessTarget","lufs":-24,"truePeak":-2}
//   {"op":"addMarker","label":"fumble"}   (label optional)
//   {"op":"ping","t":123}
//
// daemon -> client
//   {"type":"snapshot","proto":1,"graph":{...},"engine":{...}}
//       "glideMs": present after a scene recall with a fade: show the
//       levels moving there over that time (linear in gain, like the engine)
//   {"type":"level","id":2001,"faderDb":-3.5,"mute":false}
//   {"type":"meters","m":[[id,peakL,peakR,rmsL,rmsR],...]}   dBFS, ~30 Hz
//   {"type":"health","xruns":0,"callbacks":N,"meanLoad":0.14,...}  ~2 Hz
//   {"type":"scenes","scenes":[{"id":1,"name":"Pregame","fadeMs":0},...],
//    "current":1,"modified":false,"canUndo":true}
//   {"type":"loudness","m":-23.1,"s":-23.4,"i":-23.0,"mMax":-18,"tp":-3.2,
//    "tpMax":-1.9,"seconds":812.4,"target":-24,"tpLimit":-2}   ~10 Hz, LUFS/dBTP
//   {"type":"markers","markers":[{"t":4521.3,"wall":"19:42:10.120","label":"..."}]}
//   {"type":"error","op":"setGraph","message":"graph has a cycle"}
//   {"type":"pong","t":123}
#pragma once

#include <QJsonObject>
#include <QString>

#include "engine/graph.h"

namespace pm::proto {

inline constexpr int kVersion = 1;
inline constexpr const char* kDefaultSocket = "prodmesh-mix";

QJsonObject graphToJson(const GraphDesc& g);
bool graphFromJson(const QJsonObject& o, GraphDesc& g, QString* error);

// Meter values on the wire: dBFS, floored at -120.
float toDbfs(float linear);

QByteArray frame(const QJsonObject& o);  // compact JSON + '\n'

}  // namespace pm::proto
