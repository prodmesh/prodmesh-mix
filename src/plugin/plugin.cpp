#include "plugin/plugin.h"

namespace pm::plugin {

bool describeClap(const std::string& path, std::vector<PluginInfo>& out,
                  std::string* error);
bool describeVst3(const std::string& path, std::vector<PluginInfo>& out,
                  std::string* error);
std::unique_ptr<Processor> loadVst3(const std::string& path,
                                    const std::string& id, double fs,
                                    int maxBlock, int channels,
                                    std::string* error);
std::unique_ptr<Processor> loadClap(const std::string& path,
                                    const std::string& id, double fs,
                                    int maxBlock, int channels,
                                    std::string* error);

static bool endsWith(const std::string& s, const char* suffix) {
    const std::string x(suffix);
    return s.size() >= x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0;
}

bool describeBundle(const std::string& path, std::vector<PluginInfo>& out,
                    std::string* error) {
    if (endsWith(path, ".clap")) return describeClap(path, out, error);
    if (endsWith(path, ".vst3")) return describeVst3(path, out, error);
    if (error) *error = path + ": unsupported plugin format";
    return false;
}

std::unique_ptr<Processor> load(const std::string& format,
                                const std::string& path,
                                const std::string& id, double sampleRate,
                                int maxBlock, int channels,
                                std::string* error, bool sandboxed) {
    if (sandboxed)
        return loadSandboxedChain({{format, path, id}}, sampleRate, maxBlock,
                                  channels, error);
    if (format == "clap")
        return loadClap(path, id, sampleRate, maxBlock, channels, error);
    if (format == "vst3")
        return loadVst3(path, id, sampleRate, maxBlock, channels, error);
    if (error) *error = format + ": unsupported plugin format";
    return nullptr;
}

}  // namespace pm::plugin
