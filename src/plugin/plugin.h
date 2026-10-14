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
};

// Lists the plugins a bundle contains. Loads code: run it in the scanner
// process, never in the engine (a broken plugin may crash on load).
bool describeBundle(const std::string& path, std::vector<PluginInfo>& out,
                    std::string* error);

// Instantiates and activates a plugin for `channels`-wide processing.
// Returns null (and sets `error`) on any failure.
std::unique_ptr<Processor> load(const std::string& format,
                                const std::string& path,
                                const std::string& id, double sampleRate,
                                int maxBlock, int channels,
                                std::string* error);

}  // namespace pm::plugin
