#include "engine/node.h"

#include "core/base64.h"
#include "plugin/plugin.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace pm {
namespace {

class GainInsert final : public Processor {
public:
    explicit GainInsert(float dB) : g_(dsp::dbToGain(dB)) {}
    void process(float* const* ch, int nch, int n) override {
        for (int c = 0; c < nch; ++c)
            for (int i = 0; i < n; ++i) ch[c][i] *= g_;
    }

private:
    float g_;
};

// Models a heavy plugin: a dependent chain of multiply-adds the compiler
// can't vectorise or elide, then a gain of exactly 1.
class CpuLoadInsert final : public Processor {
public:
    explicit CpuLoadInsert(float opsPerSample)
        : ops_(int(opsPerSample)) {}
    void process(float* const* ch, int nch, int n) override {
        for (int c = 0; c < nch; ++c) {
            float acc = state_;
            for (int i = 0; i < n; ++i)
                for (int k = 0; k < ops_; ++k) acc = acc * 0.999f + ch[c][i];
            state_ = acc;
        }
        sink_ = state_;
    }

private:
    int ops_;
    float state_ = 0;
    volatile float sink_ = 0;
};

dsp::BiquadCoeffs bandCoeffs(double fs, const EqBand& b) {
    switch (b.type) {
    case EqBand::Type::LowShelf:
        return dsp::BiquadCoeffs::lowShelf(fs, b.freqHz, b.gainDb);
    case EqBand::Type::HighShelf:
        return dsp::BiquadCoeffs::highShelf(fs, b.freqHz, b.gainDb);
    case EqBand::Type::Peak:
        break;
    }
    return dsp::BiquadCoeffs::peaking(fs, b.freqHz, b.q, b.gainDb);
}

float levelGain(float faderDb, bool mute) {
    return mute ? 0.0f : dsp::dbToGain(faderDb);
}

}  // namespace

bool sameStructure(const ProcessingDesc& a, const ProcessingDesc& b) {
    return a.hpf == b.hpf && a.hpfHz == b.hpfHz && a.gate == b.gate &&
           a.gateParams == b.gateParams && a.eq == b.eq &&
           a.bands == b.bands && a.comp == b.comp &&
           a.compParams == b.compParams && a.limiter == b.limiter &&
           a.limiterCeilingDb == b.limiterCeilingDb &&
           a.inserts == b.inserts;
}

const NodeDesc* GraphDesc::find(NodeId id) const {
    for (const auto& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}

NodeDesc* GraphDesc::find(NodeId id) {
    for (auto& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}

bool GraphDesc::reaches(NodeId from, NodeId to) const {
    std::vector<NodeId> stack{from}, seen;
    while (!stack.empty()) {
        const NodeId n = stack.back();
        stack.pop_back();
        if (n == to) return true;
        if (std::find(seen.begin(), seen.end(), n) != seen.end()) continue;
        seen.push_back(n);
        for (const auto& e : edges)
            if (e.from == n) stack.push_back(e.to);
    }
    return false;
}

void resolveDerived(GraphDesc& g) {
    std::erase_if(g.edges, [](const EdgeDesc& e) { return e.derived; });
    std::vector<EdgeDesc> add;
    for (const auto& m : g.nodes) {
        const auto& mm = m.mixMinus;
        if (m.kind != NodeKind::Bus || !mm.follows || mm.follows == m.id ||
            !g.find(mm.follows))
            continue;
        for (const auto& s : g.nodes) {
            if (s.kind != NodeKind::Strip) continue;
            if (std::find(mm.exclude.begin(), mm.exclude.end(), s.id) !=
                mm.exclude.end())
                continue;
            bool explicitSend = false;
            for (const auto& e : g.edges)
                explicitSend |= e.from == s.id && e.to == m.id;
            // Never feed a strip that listens to this bus (a cycle).
            if (explicitSend || !g.reaches(s.id, mm.follows) ||
                g.reaches(m.id, s.id))
                continue;
            // Same image as in the program: the pan of its assignment.
            float pan = 0;
            for (const auto& e : g.edges)
                if (e.from == s.id && g.reaches(e.to, mm.follows)) {
                    pan = e.pan;
                    break;
                }
            add.push_back({s.id, m.id, 0, pan, true, true});
        }
    }
    // Monitor buses: the followed bus, or whatever is soloed.
    for (const auto& m : g.nodes) {
        if (m.kind != NodeKind::Bus || !m.monitor.on) continue;
        std::vector<NodeId> soloed;
        for (const auto& n : g.nodes)
            if (n.solo && n.id != m.id && !n.monitor.on &&
                (n.kind == NodeKind::Strip || n.kind == NodeKind::Bus) &&
                !g.reaches(m.id, n.id))  // never feed what listens to it
                soloed.push_back(n.id);
        if (soloed.empty()) {
            if (m.monitor.follows && g.find(m.monitor.follows) &&
                !g.reaches(m.id, m.monitor.follows))
                add.push_back({m.monitor.follows, m.id, 0, 0, true, true});
            continue;
        }
        for (NodeId s : soloed) {
            float pan = 0;  // the same image as in the mix
            for (const auto& e : g.edges)
                if (e.from == s && !e.derived && !e.send) {
                    pan = e.pan;
                    break;
                }
            EdgeDesc e{s, m.id, 0, pan, true, true};
            e.preFader = m.monitor.pfl;
            add.push_back(e);
        }
    }
    g.edges.insert(g.edges.end(), add.begin(), add.end());
}

NodeInstance::NodeInstance(const NodeDesc& d, double fs, int maxBlock)
    : id_(d.id),
      kind_(d.kind),
      channels_(d.channels < 1 ? 1 : d.channels > 2 ? 2 : d.channels),
      processing_(d.processing) {
    const auto& p = processing_;
    generator_.prepare(fs, d.generator);
    for (int c = 0; c < channels_; ++c) {
        hpf_[c].setCoeffs(dsp::BiquadCoeffs::highpass(fs, p.hpfHz, 0.7071));
        for (int b = 0; b < 4; ++b) eq_[b][c].setCoeffs(bandCoeffs(fs, p.bands[b]));
        gate_[c].prepare(fs, p.gateParams);
        gate_[c].reset();
    }
    comp_.prepare(fs, p.compParams);
    limiter_.prepare(fs, p.limiterCeilingDb, 50);
    // Consecutive isolated plugins share one host process, so a chain adds
    // one block of latency, not one per plugin.
    std::vector<plugin::PluginRef> isolated;
    std::vector<std::string> isolatedStates;  // raw, per chain position
    std::vector<std::size_t> isolatedSlots;   // their InsertDesc indices
    auto rawState = [](const InsertDesc& ins) {
        std::string raw;
        if (!ins.state.empty()) base64Decode(ins.state, raw);
        return raw;
    };
    insertMap_.assign(p.inserts.size(), {-1, 0});
    auto flushIsolated = [&] {
        if (isolated.empty()) return;
        std::string err;
        if (auto proc = plugin::loadSandboxedChain(isolated, isolatedStates, fs,
                                                   maxBlock, channels_, &err)) {
            for (std::size_t k = 0; k < isolatedSlots.size(); ++k)
                insertMap_[isolatedSlots[k]] = {int(inserts_.size()), int(k)};
            inserts_.push_back(std::move(proc));
        } else {
            warnings_.push_back(err);  // the chain is passed through
        }
        isolated.clear();
        isolatedStates.clear();
        isolatedSlots.clear();
    };
    for (std::size_t slot = 0; slot < p.inserts.size(); ++slot) {
        const auto& ins = p.inserts[slot];
        if (ins.type == InsertDesc::Type::Plugin && ins.sandboxed) {
            isolated.push_back({ins.format, ins.path, ins.pluginId});
            isolatedStates.push_back(rawState(ins));
            isolatedSlots.push_back(slot);
            continue;
        }
        flushIsolated();
        insertMap_[slot] = {int(inserts_.size()), 0};
        switch (ins.type) {
        case InsertDesc::Type::Gain:
            inserts_.push_back(std::make_unique<GainInsert>(ins.value));
            break;
        case InsertDesc::Type::CpuLoad:
            inserts_.push_back(std::make_unique<CpuLoadInsert>(ins.value));
            break;
        case InsertDesc::Type::Plugin: {
            std::string err;
            if (auto proc = plugin::load(ins.format, ins.path, ins.pluginId,
                                         fs, maxBlock, channels_, &err)) {
                if (const auto raw = rawState(ins); !raw.empty())
                    proc->loadState(raw);
                inserts_.push_back(std::move(proc));
            } else {
                insertMap_[slot] = {-1, 0};
                warnings_.push_back(err);  // the insert is passed through
            }
            break;
        }
        }
    }
    flushIsolated();
}

std::vector<NodeInstance::PluginRt> NodeInstance::pluginRtReport() const {
    std::vector<PluginRt> out;
    for (const auto& ins : inserts_)
        if (ins->rtAllocations() || ins->missedBlocks())
            out.push_back({ins->name(), ins->rtAllocations(), ins->missedBlocks()});
    return out;
}

bool NodeInstance::requestEditor(int insertIndex, std::string* error) {
    if (insertIndex < 0 || insertIndex >= int(insertMap_.size()) ||
        insertMap_[std::size_t(insertIndex)].first < 0) {
        if (error) *error = "that insert is not running";
        return false;
    }
    const auto [proc, pos] = insertMap_[std::size_t(insertIndex)];
    return inserts_[std::size_t(proc)]->requestEditor(pos, error);
}

std::string NodeInstance::insertState(int insertIndex) {
    if (insertIndex < 0 || insertIndex >= int(insertMap_.size()) ||
        insertMap_[std::size_t(insertIndex)].first < 0)
        return {};
    const auto [proc, pos] = insertMap_[std::size_t(insertIndex)];
    return inserts_[std::size_t(proc)]->stateFor(pos);
}

std::vector<std::string> NodeInstance::maintain() {
    std::vector<std::string> events;
    for (auto& ins : inserts_)
        if (auto e = ins->maintain(); !e.empty()) events.push_back(std::move(e));
    return events;
}

void NodeInstance::stopProcessing() {
    for (auto& ins : inserts_) ins->stopProcessing();
}

int NodeInstance::latencySamples() const {
    int total = 0;
    for (const auto& ins : inserts_) total += ins->latencySamples();
    return total;
}

void NodeInstance::process(float* const* ch, int n) {
    const auto& p = processing_;
    if (kind_ == NodeKind::Generator) {
        generator_.process(ch[0], n);
        for (int c = 1; c < channels_; ++c)
            for (int i = 0; i < n; ++i) ch[c][i] = ch[0][i];
    }
    for (int c = 0; c < channels_; ++c) {
        if (p.hpf) hpf_[c].process(ch[c], n);
        if (p.gate) gate_[c].process(ch[c], n);
        if (p.eq)
            for (int b = 0; b < 4; ++b) eq_[b][c].process(ch[c], n);
    }
    if (p.comp) comp_.process(ch, channels_, n);
    for (auto& ins : inserts_) ins->process(ch, channels_, n);
    if (p.limiter) limiter_.process(ch, channels_, n);
}

LevelState::LevelState(float faderDb, bool mute)
    : fader_(levelGain(faderDb, false)), on_(mute ? 0.0f : 1.0f) {}

void LevelState::set(float faderDb, bool mute, int faderRamp, int muteRamp,
                     std::uint64_t seq) {
    if (seq <= seq_) return;
    seq_ = seq;
    fader_.setTarget(levelGain(faderDb, false), faderRamp);
    on_.setTarget(mute ? 0.0f : 1.0f, muteRamp);
}

void LevelState::apply(float* const* ch, int nch, int n, float* const* pre) {
    if (!fader_.ramping() && !on_.ramping()) {  // steady: plain multiplies
        const float m = on_.current();
        const float g = fader_.current() * m;
        for (int c = 0; c < nch; ++c) {
            float* x = ch[c];
            if (pre) {
                if (m == 1.0f) std::memcpy(pre[c], x, sizeof(float) * std::size_t(n));
                else
                    for (int i = 0; i < n; ++i) pre[c][i] = x[i] * m;
            }
            if (g != 1.0f)
                for (int i = 0; i < n; ++i) x[i] *= g;
        }
        return;
    }
    // The ramps must advance identically on every channel.
    for (int i = 0; i < n; ++i) {
        const float m = on_.next();
        const float g = fader_.next() * m;
        for (int c = 0; c < nch; ++c) {
            if (pre) pre[c][i] = ch[c][i] * m;
            ch[c][i] *= g;
        }
    }
}

void MeterState::accumulate(const float* const* ch, int n) {
    for (int c = 0; c < channels_; ++c) {
        float pk = 0;
        double e = 0;
        for (int i = 0; i < n; ++i) {
            const float v = ch[c][i];
            pk = std::max(pk, std::fabs(v));
            e += double(v) * v;
        }
        float cur = peak_[c].load(std::memory_order_relaxed);
        while (pk > cur && !peak_[c].compare_exchange_weak(
                               cur, pk, std::memory_order_relaxed)) {
        }
        energy_[c].fetch_add(e, std::memory_order_relaxed);
    }
    samples_.fetch_add(std::uint32_t(n), std::memory_order_release);
}

MeterState::Reading MeterState::take() {
    Reading r;
    r.channels = channels_;
    const auto n = samples_.exchange(0, std::memory_order_acquire);
    for (int c = 0; c < channels_; ++c) {
        r.peak[c] = peak_[c].exchange(0, std::memory_order_relaxed);
        const double e = energy_[c].exchange(0, std::memory_order_relaxed);
        r.rms[c] = n ? float(std::sqrt(e / n)) : 0.0f;
    }
    return r;
}

EdgeInstance::EdgeInstance(NodeId from, NodeId to, int srcCh, int dstCh,
                           int delaySamples, int maxBlock, bool preFader)
    : from_(from), to_(to), srcCh_(srcCh), dstCh_(dstCh),
      delay_(std::max(0, delaySamples)), pre_(preFader) {
    if (delay_ > 0)
        for (int c = 0; c < srcCh_; ++c) {
            line_[c].assign(std::size_t(delay_), 0.0f);
            delayed_[c].assign(std::size_t(std::max(maxBlock, 1)), 0.0f);
        }
}

void EdgeInstance::targetsFor(const EdgeDesc* e, int srcCh, int dstCh,
                              float out[kMaxChannels][kMaxChannels]) {
    for (int d = 0; d < kMaxChannels; ++d)
        for (int s = 0; s < kMaxChannels; ++s) out[d][s] = 0;
    if (!e || e->mute) return;
    const float g = dsp::dbToGain(e->gainDb);
    if (srcCh == 1 && dstCh == 2) {
        // -3 dB constant-power pan law.
        const double theta = (std::clamp(e->pan, -1.0f, 1.0f) + 1) *
                             std::numbers::pi / 4;
        out[0][0] = g * float(std::cos(theta));
        out[1][0] = g * float(std::sin(theta));
    } else if (srcCh == 2 && dstCh == 1) {
        out[0][0] = out[0][1] = g * 0.5f;
    } else if (srcCh == 2 && dstCh == 2) {
        // Balance: the far side dips, the near side stays at unity.
        const float p = std::clamp(e->pan, -1.0f, 1.0f);
        out[0][0] = g * (p > 0 ? 1 - p : 1.0f);
        out[1][1] = g * (p < 0 ? 1 + p : 1.0f);
    } else {
        for (int c = 0; c < srcCh; ++c) out[c][c] = g;
    }
}

void EdgeInstance::setTargets(const float t[kMaxChannels][kMaxChannels],
                              int samples) {
    for (int d = 0; d < dstCh_; ++d)
        for (int s = 0; s < srcCh_; ++s) m_[d][s].setTarget(t[d][s], samples);
}

bool EdgeInstance::silent() const {
    for (int d = 0; d < dstCh_; ++d)
        for (int s = 0; s < srcCh_; ++s)
            if (!m_[d][s].silent()) return false;
    return true;
}

void EdgeInstance::mixInto(float* const* dst, const float* const* src,
                           int n) {
    const float* in[kMaxChannels] = {src[0], srcCh_ > 1 ? src[1] : nullptr};
    if (delay_ > 0) {
        // The delay line advances every block, even while the edge is
        // silent, so it is primed when it fades in.
        const std::size_t len = std::size_t(delay_);
        for (int c = 0; c < srcCh_; ++c) {
            float* line = line_[c].data();
            float* out = delayed_[c].data();
            std::size_t p = pos_;
            for (int i = 0; i < n; ++i) {
                out[i] = line[p];
                line[p] = src[c][i];
                if (++p == len) p = 0;
            }
            in[c] = out;
        }
        pos_ = (pos_ + std::size_t(n)) % len;
    }
    for (int d = 0; d < dstCh_; ++d)
        for (int s = 0; s < srcCh_; ++s)
            if (!m_[d][s].silent()) m_[d][s].mixInto(dst[d], in[s], n);
}

}  // namespace pm
