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
//   {"op":"ping","t":123}
//
// daemon -> client
//   {"type":"snapshot","proto":1,"graph":{...},"engine":{...}}
//   {"type":"level","id":2001,"faderDb":-3.5,"mute":false}
//   {"type":"meters","m":[[id,peakL,peakR,rmsL,rmsR],...]}   dBFS, ~30 Hz
//   {"type":"health","xruns":0,"callbacks":N,"meanLoad":0.14,...}  ~2 Hz
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
