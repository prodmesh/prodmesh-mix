// Third-party plugin hosting. A loaded plugin is a Processor in a node's
// insert chain; formats differ only in the adapter.
//
// Threading (CLAP's rules, which VST3 can be fitted to):
//   load, activate, deactivate, destroy  -> control thread
//   start/stop processing, process       -> audio thread
// A plugin that fails to load must never take the show down: the engine
// passes audio straight through that insert and reports a warning.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "engine/node.h"

namespace pm::plugin {

struct PluginInfo {
    std::string format;  // "clap" | "vst3"
    std::string path;    // bundle path
    std::string id;      // plugin id within the bundle
    std::string name, vendor, version;
    bool instrument = false;  // generates audio; not offered as an insert
};

// Lists the plugins a bundle contains. Loads code: run it in the scanner
// process, never in the engine (a broken plugin may crash on load).
bool describeBundle(const std::string& path, std::vector<PluginInfo>& out,
                    std::string* error);

struct PluginRef {
    std::string format, path, id;
};

// Runs consecutive plugins as one chain in a single mix-pluginhost process:
// one block of extra latency for the whole chain, not per plugin. Any load
// failure fails the chain (the engine passes it through).
std::unique_ptr<Processor> loadSandboxedChain(const std::vector<PluginRef>& chain,
                                              double sampleRate, int maxBlock,
                                              int channels, std::string* error);

// Instantiates and activates a plugin for `channels`-wide processing.
// Returns null (and sets `error`) on any failure. `sandboxed` runs it in a
// mix-pluginhost process (one block of extra latency; a crash bypasses the
// insert and the host is restarted) instead of in the engine.
std::unique_ptr<Processor> load(const std::string& format,
                                const std::string& path,
                                const std::string& id, double sampleRate,
                                int maxBlock, int channels,
                                std::string* error, bool sandboxed = false);

}  // namespace pm::plugin
