#include "engine/engine.h"

#include <algorithm>
#include <cstring>
#include <set>

#include "core/base64.h"
#include "engine/node.h"
#include "engine/plan.h"
#include "engine/worker_pool.h"

namespace pm {
namespace {

// Steps are keyed by node id; a node being replaced (kind, channels or
// device patch changed) keeps its old self as a second, dying step under
// the shadow key while the new one fades in.
using Key = std::uint64_t;
constexpr Key kShadow = Key(1) << 32;

constexpr int kMaxBlockLimit = 8192;

bool sameShape(const NodeDesc& a, const NodeDesc& b) {
    return a.kind == b.kind && a.channels == b.channels &&
           a.ioChannel == b.ioChannel;
}

}  // namespace

const PlanStep* RenderPlan::stepFor(NodeId id) const {
    auto it = std::lower_bound(
        index.begin(), index.end(), id,
        [](const std::pair<NodeId, int>& e, NodeId v) { return e.first < v; });
    if (it == index.end() || it->first != id) return nullptr;
    return &steps[it->second];
}

Engine::Engine(const EngineConfig& cfg) : cfg_(cfg) {
    cfg_.maxBlock = std::clamp(cfg_.maxBlock, 1, kMaxBlockLimit);
    // At least one sample, or every structural edit becomes a hard cut.
    transitionSamples_ =
        std::max(1, int(cfg_.sampleRate * cfg_.transitionMs / 1000));
    levelRampSamples_ =
        std::max(1, int(cfg_.sampleRate * cfg_.levelRampMs / 1000));
    if (cfg_.workerThreads > 0)
        pool_ = std::make_unique<WorkerPool>(*this, cfg_.workerThreads,
                                             cfg_.sampleRate, cfg_.maxBlock,
                                             cfg_.workgroup);
}

Engine::~Engine() {
    pool_.reset();
    // The driver has stopped (a documented precondition), so this thread
    // stands in for the audio thread: stop plugins before they are
    // deactivated and destroyed.
    if (active_)
        for (const auto& n : active_->nodes) n->stopProcessing();
    while (auto p = retired_.pop()) delete *p;
    while (auto p = pending_.pop()) delete *p;
    delete active_;
}

// --- control thread ------------------------------------------------------

bool Engine::setGraph(const GraphDesc& in, std::string* error,
                      double glideMs) {
    GraphDesc g = in;
    resolveMixMinus(g);
    carryPluginStates(g);
    // Validate by compiling against nothing; the result is discarded.
    // A dry run (no Live): structure only, no instances, no plugin loads.
    if (!compile(g, nullptr, nullptr, error, nullptr)) return false;
    desired_ = std::move(g);
    // Coalesced edits glide if any of them asked to.
    glideSamples_ = std::max(glideSamples_,
                             int(std::max(0.0, glideMs) * cfg_.sampleRate / 1000));
    dirty_ = true;
    pump();
    return true;
}

void Engine::setLevel(NodeId id, float faderDb, bool mute) {
    for (auto* g : {&desired_, &published_})
        if (auto* n = g->find(id)) {
            n->faderDb = faderDb;
            n->mute = mute;
        }
    levels_.push({id, faderDb, mute, ++levelSeq_});
    // A plan compiled before this move may still be on its way to the
    // audio thread, or may hold a node this command cannot reach yet.
    // Once it lands, every level is re-sent with a newer sequence number.
    if (activeSerial_.load(std::memory_order_acquire) != publishedSerial_)
        resyncLevels_ = true;
}

bool Engine::canPublish() const {
    if (publishedSerial_ == 0) return true;
    return activeSerial_.load(std::memory_order_acquire) == publishedSerial_ &&
           transitionDoneSerial_.load(std::memory_order_acquire) ==
               publishedSerial_;
}

void Engine::pump() {
    while (auto p = retired_.pop()) delete *p;
    // Sandboxed plugin hosts: restart any that died.
    for (const auto& [id, inst] : live_.nodes)
        for (auto& e : inst->maintain()) events_.push_back(std::move(e));

    if (resyncLevels_ &&
        activeSerial_.load(std::memory_order_acquire) == publishedSerial_) {
        resyncLevels_ = false;
        for (const auto& n : published_.nodes)
            levels_.push({n.id, n.faderDb, n.mute, ++levelSeq_});
    }

    if (!canPublish()) return;
    if (dirty_) {
        Live live;
        Fail why = Fail::None;
        std::string err;
        if (auto plan = compile(desired_, &published_, &live, &err, &why)) {
            publish(std::move(plan), std::move(live), desired_);
            dirty_ = false;
            lastError_.clear();
            return;
        }
        if (why == Fail::FadingCycle) {
            // Fade out what the edit removes first; the additions follow
            // once that transition is done.
            GraphDesc step = published_;
            std::erase_if(step.nodes, [&](const NodeDesc& n) {
                return !desired_.find(n.id);
            });
            std::erase_if(step.edges, [&](const EdgeDesc& e) {
                for (const auto& d : desired_.edges)
                    if (d.from == e.from && d.to == e.to) return false;
                return true;
            });
            Live stepLive;
            const bool progress =
                step.nodes.size() != published_.nodes.size() ||
                step.edges.size() != published_.edges.size();
            if (progress)
            if (auto plan =
                    compile(step, &published_, &stepLive, &err, nullptr)) {
                publish(std::move(plan), std::move(stepLive), step);
                lastError_.clear();
                return;
            }
        }
        lastError_ = err;
    } else if (publishedNeedsCleanup_) {
        // Drop dying steps and faded-out instances. Nothing changes, so
        // the output is bit-identical across this swap.
        Live live;
        GraphDesc same = published_;
        if (auto plan = compile(same, &published_, &live, nullptr, nullptr))
            publish(std::move(plan), std::move(live), same);
    }
}

void Engine::setRecordTaps(
    std::vector<std::pair<NodeId, std::shared_ptr<RecordTap>>> taps) {
    taps_ = std::move(taps);
    dirty_ = true;
    pump();
}

std::vector<Engine::PluginRtIssue> Engine::pluginRtIssues() const {
    std::vector<PluginRtIssue> out;
    for (const auto& [id, inst] : live_.nodes)
        for (const auto& r : inst->pluginRtReport())
            out.push_back({id, r.plugin, r.allocations, r.missed});
    return out;
}

Engine::DelayInfo Engine::delayInfo(NodeId id) const {
    DelayInfo d;
    if (auto it = live_.chain.find(id); it != live_.chain.end()) d.chain = it->second;
    if (auto it = live_.latency.find(id); it != live_.latency.end())
        d.arrival = it->second;
    if (auto it = live_.comp.find(id); it != live_.comp.end())
        d.compensation = it->second;
    return d;
}

void Engine::carryPluginStates(GraphDesc& g) {
    auto same = [](const InsertDesc& a, const InsertDesc& b) {
        return a.type == InsertDesc::Type::Plugin && b.type == a.type &&
               a.format == b.format && a.path == b.path &&
               a.pluginId == b.pluginId;  // isolated or not: same state
    };
    for (auto& n : g.nodes) {
        const NodeDesc* old = published_.find(n.id);
        auto it = live_.nodes.find(n.id);
        if (!old || it == live_.nodes.end() ||
            sameStructure(old->processing, n.processing))
            continue;
        const auto& was = old->processing.inserts;
        bool plugins = false;
        for (const auto& i : was) plugins |= i.type == InsertDesc::Type::Plugin;
        if (!plugins) continue;
        // Isolated hosts report on request; in-engine plugins answer now.
        it->second->refreshStates(150);
        // Match each plugin to the first unused one of the same kind:
        // a reorder or an added neighbour keeps every plugin's settings.
        std::vector<bool> used(was.size(), false);
        for (auto& ins : n.processing.inserts)
            for (std::size_t j = 0; j < was.size(); ++j)
                if (!used[j] && same(ins, was[j])) {
                    used[j] = true;
                    const auto raw = it->second->insertState(int(j));
                    if (!raw.empty()) ins.state = base64Encode(raw);
                    break;
                }
    }
}

void Engine::collectPluginStates(int waitMs) {
    if (waitMs > 0)
        for (const auto& [id, inst] : live_.nodes) inst->refreshStates(waitMs);
    for (const auto& [id, inst] : live_.nodes) {
        const NodeDesc* d = published_.find(id);
        if (!d) continue;
        for (int i = 0; i < int(d->processing.inserts.size()); ++i) {
            if (d->processing.inserts[std::size_t(i)].type != InsertDesc::Type::Plugin)
                continue;
            const auto raw = inst->insertState(i);
            if (raw.empty()) continue;
            const auto b64 = base64Encode(raw);
            for (auto* g : {&desired_, &published_})
                if (auto* n = g->find(id); n && i < int(n->processing.inserts.size()) &&
                                           n->processing.inserts[std::size_t(i)] ==
                                               d->processing.inserts[std::size_t(i)])
                    n->processing.inserts[std::size_t(i)].state = b64;
        }
    }
}

bool Engine::openEditor(NodeId id, int insertIndex, std::string* error) {
    auto it = live_.nodes.find(id);
    if (it == live_.nodes.end()) {
        if (error) *error = "no such channel";
        return false;
    }
    return it->second->requestEditor(insertIndex, error);
}

int Engine::latencyOf(NodeId id) const {
    auto it = live_.latency.find(id);
    return it == live_.latency.end() ? 0 : it->second;
}

bool Engine::takeMeter(NodeId id, MeterReading& out) {
    auto it = live_.meters.find(id);
    if (it == live_.meters.end()) return false;
    const auto r = it->second->take();
    out.channels = r.channels;
    for (int c = 0; c < r.channels && c < 2; ++c) {
        out.peak[c] = r.peak[c];
        out.rms[c] = r.rms[c];
    }
    return true;
}

bool Engine::settled() const {
    return !dirty_ && !publishedNeedsCleanup_ && canPublish() &&
           publishedSerial_ != 0;
}

void Engine::publish(std::unique_ptr<RenderPlan> plan, Live live,
                     const GraphDesc& g) {
    plan->serial = nextSerial_++;
    plan->levelSeq = ++levelSeq_;
    if (glideSamples_ > 0) {
        plan->levelRamp = glideSamples_;
        for (auto& t : plan->edgeTargets)
            if (t.samples < 0) t.samples = glideSamples_;  // surviving edges
        glideSamples_ = 0;
    }
    publishedSerial_ = plan->serial;
    publishedNeedsCleanup_ = plan->needsCleanup;
    published_ = g;
    live_ = std::move(live);
    publishedNodes_.clear();
    for (const auto& n : plan->nodes) publishedNodes_.push_back(n.get());
    pending_.push(plan.release());  // gated by canPublish(); never full
}

std::unique_ptr<RenderPlan> Engine::compile(const GraphDesc& next,
                                            const GraphDesc* prev, Live* live,
                                            std::string* error, Fail* why) {
    auto fail = [&](std::string msg, Fail f = Fail::Invalid)
        -> std::unique_ptr<RenderPlan> {
        if (error) *error = std::move(msg);
        if (why) *why = f;
        return nullptr;
    };
    const double fs = cfg_.sampleRate;
    auto plan = std::make_unique<RenderPlan>();
    const bool first = !prev || publishedSerial_ == 0;
    plan->xfadeLen = first ? 0 : transitionSamples_;
    const bool dry = !live;
    Live scratch;
    if (!live) live = &scratch;

    auto findLive = [&](auto& map, NodeId id) {
        auto it = map.find(id);
        return it == map.end() ? nullptr : it->second;
    };

    // 1. Steps: every desired node, plus removed and replaced nodes that
    //    fade out.
    std::vector<PlanStep> protos;
    std::vector<Key> keys;
    std::set<NodeId> ids;
    std::set<NodeId> shadowed;

    auto stepFrom = [](const NodeDesc& d) {
        PlanStep s;
        s.id = d.id;
        s.kind = d.kind;
        s.nch = d.channels;
        s.ioChannel = d.ioChannel;
        s.faderDb = d.faderDb;
        s.mute = d.mute;
        return s;
    };
    auto addDying = [&](const NodeDesc& d, Key key) {
        auto inst = findLive(live_.nodes, d.id);
        auto lvl = findLive(live_.levels, d.id);
        if (!inst || !lvl) return false;
        PlanStep s = stepFrom(d);
        s.node = inst.get();
        s.level = lvl.get();
        s.dying = true;
        plan->nodes.push_back(inst);
        plan->levelStates.push_back(lvl);
        plan->needsCleanup = true;
        protos.push_back(s);
        keys.push_back(key);
        return true;
    };

    for (const auto& d : next.nodes) {
        if (!ids.insert(d.id).second)
            return fail("duplicate node id " + std::to_string(d.id));
        if (d.channels < 1 || d.channels > kMaxChannels)
            return fail("node " + std::to_string(d.id) + ": bad channels");
        if (d.ioChannel < 0 || d.ioChannel + d.channels > kMaxDeviceChannels)
            return fail("node " + std::to_string(d.id) + ": bad ioChannel");

        PlanStep s = stepFrom(d);
        const NodeDesc* old = prev && !first ? prev->find(d.id) : nullptr;
        auto inst = old ? findLive(live_.nodes, d.id) : nullptr;
        std::shared_ptr<LevelState> lvl;
        if (old && inst && sameShape(*old, d)) {
            lvl = findLive(live_.levels, d.id);
            if (old->generator == d.generator &&
                sameStructure(old->processing, d.processing)) {
                s.node = inst.get();
                plan->nodes.push_back(inst);
            } else {
                s.fadeOut = inst.get();
                plan->nodes.push_back(inst);
                plan->needsCleanup = true;
            }
        } else if (old && inst) {
            if (addDying(*old, Key(d.id) | kShadow)) shadowed.insert(d.id);
        }
        if (!s.node && !dry) {
            auto fresh = std::make_shared<NodeInstance>(d, fs, cfg_.maxBlock);
            for (const auto& w : fresh->warnings())
                warnings_.push_back(
                    (d.name.empty() ? "node " + std::to_string(d.id) : d.name) +
                    ": " + w);
            s.node = fresh.get();
            plan->nodes.push_back(std::move(fresh));
        }
        if (!lvl) lvl = std::make_shared<LevelState>(d.faderDb, d.mute);
        s.level = lvl.get();
        plan->levelStates.push_back(lvl);
        std::shared_ptr<MeterState> meter;
        if (old && sameShape(*old, d)) meter = findLive(live_.meters, d.id);
        if (!meter) meter = std::make_shared<MeterState>(d.channels);
        s.meter = meter.get();
        plan->meters.push_back(meter);
        live->meters[d.id] = meter;
        std::size_t ntaps = 0;
        for (const auto& [id, tap] : taps_)
            if (id == d.id && tap->channels() == d.channels &&
                ntaps < s.taps.size()) {
                s.taps[ntaps++] = tap.get();
                plan->taps.push_back(tap);
            }
        // The instance that renders is always the last one pushed.
        if (!dry) live->nodes[d.id] = plan->nodes.back();
        live->levels[d.id] = lvl;
        protos.push_back(s);
        keys.push_back(Key(d.id));
    }

    if (prev && !first)
        for (const auto& d : prev->nodes)
            if (!ids.count(d.id)) addDying(d, Key(d.id));

    // 2. Edges: desired ones, plus removed ones ramping to silence.
    struct EdgeProto {
        Key from, to;
        NodeId fromId, toId;
        const EdgeDesc* desc;  // null = dying
    };
    std::vector<EdgeProto> edges;
    std::set<std::pair<NodeId, NodeId>> wanted;
    for (const auto& e : next.edges) {
        if (!next.find(e.from) || !next.find(e.to))
            return fail("edge to unknown node");
        if (e.from == e.to) return fail("self edge");
        if (!wanted.insert({e.from, e.to}).second)
            return fail("duplicate edge");
        edges.push_back({Key(e.from), Key(e.to), e.from, e.to, &e});
    }
    std::set<Key> stepKeys(keys.begin(), keys.end());
    bool hasDyingEdges = false;
    if (prev && !first) {
        for (const auto& e : prev->edges) {
            if (!live_.edges.count({e.from, e.to})) continue;
            const bool touchesShadow =
                shadowed.count(e.from) || shadowed.count(e.to);
            if (wanted.count({e.from, e.to}) && !touchesShadow) continue;
            const Key f = shadowed.count(e.from) ? Key(e.from) | kShadow
                                                 : Key(e.from);
            const Key t =
                shadowed.count(e.to) ? Key(e.to) | kShadow : Key(e.to);
            if (!stepKeys.count(f) || !stepKeys.count(t)) continue;
            edges.push_back({f, t, e.from, e.to, nullptr});
            plan->needsCleanup = true;
            hasDyingEdges = true;
        }
    }

    // 3. Levels by longest path from a source; reject cycles.
    std::map<Key, int> protoOf;
    for (int i = 0; i < int(protos.size()); ++i) protoOf[keys[i]] = i;
    std::vector<int> level(protos.size(), 0), indeg(protos.size(), 0);
    std::vector<std::vector<int>> out(protos.size());
    for (const auto& e : edges) {
        out[protoOf[e.from]].push_back(protoOf[e.to]);
        ++indeg[protoOf[e.to]];
    }
    std::vector<int> queue;
    for (int i = 0; i < int(protos.size()); ++i)
        if (indeg[i] == 0) queue.push_back(i);
    for (std::size_t q = 0; q < queue.size(); ++q)
        for (int v : out[queue[q]]) {
            level[v] = std::max(level[v], level[queue[q]] + 1);
            if (--indeg[v] == 0) queue.push_back(v);
        }
    if (queue.size() != protos.size())
        return fail("graph has a cycle",
                    hasDyingEdges ? Fail::FadingCycle : Fail::Invalid);

    // 4. Order by (level, key) — deterministic regardless of edit history.
    std::vector<int> order(protos.size());
    for (int i = 0; i < int(order.size()); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        if (level[a] != level[b]) return level[a] < level[b];
        return keys[a] < keys[b];
    });
    std::map<Key, int> stepOf;
    std::vector<Key> stepKey;
    int maxLevel = -1;
    for (int i : order) {
        stepOf[keys[i]] = int(plan->steps.size());
        stepKey.push_back(keys[i]);
        while (maxLevel < level[i]) {
            plan->levelStart.push_back(int(plan->steps.size()));
            ++maxLevel;
        }
        plan->steps.push_back(protos[i]);
    }
    plan->levelStart.push_back(int(plan->steps.size()));

    // 5. Edge instances and per-step contributions, sorted by source key
    //    so summation order never depends on edit history. Steps are in
    //    topological order, so each step's arrival latency is known before
    //    its consumers: every input is delayed to line up with the latest
    //    one (plugin delay compensation).
    std::sort(edges.begin(), edges.end(), [](const auto& a, const auto& b) {
        return a.to != b.to ? a.to < b.to : a.from < b.from;
    });
    std::vector<int> arrival(plan->steps.size(), 0);
    for (int si = 0; si < int(plan->steps.size()); ++si) {
        auto& s = plan->steps[si];
        s.firstContrib = int(plan->contribs.size());
        const Key me = stepKey[si];
        int inArrival = 0;
        bool anyLive = false;
        for (const auto& e : edges)
            if (e.to == me && e.desc) anyLive = true;
        for (const auto& e : edges)
            if (e.to == me && (e.desc || !anyLive))
                inArrival = std::max(inArrival, arrival[stepOf[e.from]]);
        arrival[si] = inArrival + (s.node ? s.node->latencySamples() : 0);
        if (!s.dying) {
            live->latency[s.id] = arrival[si];
            live->chain[s.id] = s.node ? s.node->latencySamples() : 0;
        }

        for (const auto& e : edges) {
            if (e.to != me) continue;
            const int srcStep = stepOf[e.from];
            const auto& src = plan->steps[srcStep];
            const int delay = inArrival - arrival[srcStep];
            const bool oldSide = !e.desc;
            const bool newOfShadowed =
                e.desc && (shadowed.count(e.fromId) || shadowed.count(e.toId));
            std::shared_ptr<EdgeInstance> inst, retiring;
            if (!newOfShadowed) {
                auto it = live_.edges.find({e.fromId, e.toId});
                if (it != live_.edges.end() &&
                    it->second->srcChannels() == src.nch &&
                    it->second->dstChannels() == s.nch) {
                    if (oldSide || (it->second->delay() == std::max(0, delay) &&
                                    it->second->preFader() == e.desc->preFader))
                        inst = it->second;
                    else
                        retiring = it->second;  // compensation or tap changed
                }
            }
            if (!inst && oldSide) continue;  // nothing to fade
            if (retiring) {
                EdgeTarget t{retiring.get(), {}};
                EdgeInstance::targetsFor(nullptr, src.nch, s.nch, t.m);
                plan->edgeTargets.push_back(t);
                plan->contribs.push_back({retiring.get(), srcStep});
                plan->edges.push_back(std::move(retiring));
                plan->needsCleanup = true;
            }
            if (!inst)
                inst = std::make_shared<EdgeInstance>(
                    e.fromId, e.toId, src.nch, s.nch, std::max(0, delay),
                    cfg_.maxBlock, e.desc->preFader);
            const bool survives = e.desc && !retiring && live_.edges.count({e.fromId, e.toId}) &&
                                  live_.edges.at({e.fromId, e.toId}) == inst;
            EdgeTarget t{inst.get(), {}, survives ? -1 : 0};  // -1: may glide
            EdgeInstance::targetsFor(e.desc, src.nch, s.nch, t.m);
            plan->edgeTargets.push_back(t);
            plan->contribs.push_back({inst.get(), srcStep});
            if (e.desc) {
                live->edges[{e.fromId, e.toId}] = inst;
                live->comp[e.fromId].push_back({e.toId, inst->delay()});
            }
            plan->edges.push_back(std::move(inst));
        }
        s.numContrib = int(plan->contribs.size()) - s.firstContrib;
    }

    // 6. Buffers: one per channel, plus scratch for each crossfade, plus
    //    a pre-fader copy for sources of pre-fader edges.
    std::vector<bool> needsPre(plan->steps.size(), false);
    for (const auto& c : plan->contribs)
        if (c.edge->preFader()) needsPre[std::size_t(c.srcStep)] = true;
    int nbuf = 0;
    for (std::size_t i = 0; i < plan->steps.size(); ++i) {
        auto& s = plan->steps[i];
        s.buf = nbuf;
        nbuf += s.nch;
        if (s.fadeOut) {
            s.fadeBuf = nbuf;
            nbuf += s.nch;
        }
        if (needsPre[i]) {
            s.preBuf = nbuf;
            nbuf += s.nch;
        }
    }
    const std::size_t stride = (std::size_t(cfg_.maxBlock) + 15) & ~15ull;
    const std::size_t total = stride * std::size_t(std::max(nbuf, 1));
    plan->storage = std::make_unique<float[]>(total);
    std::fill_n(plan->storage.get(), total, 0.0f);
    for (int b = 0; b < nbuf; ++b)
        plan->chan.push_back(plan->storage.get() + stride * std::size_t(b));

    for (int i = 0; i < int(plan->steps.size()); ++i)
        if (!plan->steps[i].dying)
            plan->index.push_back({plan->steps[i].id, i});
    std::sort(plan->index.begin(), plan->index.end());

    // Instances the running plan has and this one drops: the audio thread
    // stops them (CLAP stop_processing) when it switches plans.
    if (!dry)
        for (NodeInstance* n : publishedNodes_) {
            bool kept = false;
            for (const auto& m : plan->nodes) kept = kept || m.get() == n;
            if (!kept) plan->stopping.push_back(n);
        }
    return plan;
}

// --- audio thread --------------------------------------------------------

void Engine::activate(RenderPlan* plan) {
    if (active_ && !retired_.push(active_)) {
        // Unreachable while publication is gated; leaking beats freeing
        // on the audio thread.
    }
    for (NodeInstance* n : plan->stopping) n->stopProcessing();
    active_ = plan;
    for (const auto& t : plan->edgeTargets)
        t.edge->setTargets(t.m, t.samples > 0 ? t.samples : plan->xfadeLen);
    const int faderRamp = plan->levelRamp > 0 ? plan->levelRamp : levelRampSamples_;
    for (const auto& s : plan->steps)
        if (!s.dying)
            s.level->set(s.faderDb, s.mute, faderRamp, levelRampSamples_,
                         plan->levelSeq);
    plan->xfadePos = 0;
    activeSerial_.store(plan->serial, std::memory_order_release);
    if (plan->xfadeLen == 0)
        transitionDoneSerial_.store(plan->serial, std::memory_order_release);
}

void Engine::renderStep(RenderPlan& p, int index, const float* const* in,
                        int numIn, int frames) {
    const PlanStep& s = p.steps[index];
    if (s.dying && !p.fading()) return;
    float* const* buf = &p.chan[s.buf];

    switch (s.kind) {
    case NodeKind::Input:
        for (int c = 0; c < s.nch; ++c) {
            const int dev = s.ioChannel + c;
            if (in && dev >= 0 && dev < numIn && in[dev])
                std::memcpy(buf[c], in[dev], sizeof(float) * frames);
            else
                std::memset(buf[c], 0, sizeof(float) * frames);
        }
        break;
    case NodeKind::Generator:
        break;  // the instance writes its own output
    default:
        for (int c = 0; c < s.nch; ++c)
            std::memset(buf[c], 0, sizeof(float) * frames);
        for (int k = 0; k < s.numContrib; ++k) {
            const auto& con = p.contribs[s.firstContrib + k];
            // A delayed edge keeps its line moving even while silent.
            if (con.edge->silent() && con.edge->delay() == 0) continue;
            const PlanStep& src = p.steps[con.srcStep];
            con.edge->mixInto(
                buf, &p.chan[con.edge->preFader() ? src.preBuf : src.buf],
                frames);
        }
        break;
    }

    if (s.fadeOut && p.fading()) {
        float* const* fade = &p.chan[s.fadeBuf];
        for (int c = 0; c < s.nch; ++c)
            std::memcpy(fade[c], buf[c], sizeof(float) * frames);
        s.node->process(buf, frames);
        s.fadeOut->process(fade, frames);
        const float inv = 1.0f / float(p.xfadeLen);
        for (int i = 0; i < frames; ++i) {
            const int pos = p.xfadePos + i;
            const float g = pos >= p.xfadeLen ? 1.0f : float(pos) * inv;
            for (int c = 0; c < s.nch; ++c)
                buf[c][i] = buf[c][i] * g + fade[c][i] * (1.0f - g);
        }
    } else {
        s.node->process(buf, frames);
    }

    // Dying steps keep their level; their edges carry the fade.
    s.level->apply(buf, s.nch, frames,
                   s.preBuf >= 0 ? &p.chan[s.preBuf] : nullptr);
    if (s.meter && !s.dying) s.meter->accumulate(buf, frames);
    if (!s.dying)
        for (RecordTap* t : s.taps)
            if (t) t->push(buf, frames, sliceTime_);
}

void Engine::process(const float* const* in, int numIn, float* const* out,
                     int numOut, int frames) PM_NONBLOCKING {
    // Newest pending plan wins; any skipped one is retired untouched.
    while (auto p = pending_.pop()) activate(*p);

    for (int c = 0; c < numOut; ++c)
        std::memset(out[c], 0, sizeof(float) * frames);
    if (!active_) return;
    RenderPlan& p = *active_;

    while (auto cmd = levels_.pop())
        if (const PlanStep* s = p.stepFor(cmd->id))
            s->level->set(cmd->faderDb, cmd->mute, levelRampSamples_,
                          levelRampSamples_,
                          cmd->seq);

    // Blocks larger than the plan's buffers are rendered in slices.
    for (int off = 0; off < frames;) {
        const int n = std::min(frames - off, cfg_.maxBlock);
        const float* slice[kMaxDeviceChannels];
        const int ins = std::min(numIn, kMaxDeviceChannels);
        for (int c = 0; c < ins; ++c)
            slice[c] = in && in[c] ? in[c] + off : nullptr;

        sliceTime_ = sampleTime_;  // read by workers after the gen bump
        if (pool_)
            pool_->run(p, slice, ins, n);
        else
            for (int i = 0; i < int(p.steps.size()); ++i)
                renderStep(p, i, slice, ins, n);

        // A dying Output keeps writing while it fades: that is how a
        // re-patched output fades out of its old channels.
        for (const auto& s : p.steps) {
            if (s.kind != NodeKind::Output || (s.dying && !p.fading()))
                continue;
            for (int c = 0; c < s.nch; ++c) {
                const int dev = s.ioChannel + c;
                if (dev < 0 || dev >= numOut) continue;
                const float* src = p.chan[s.buf + c];
                float* dst = out[dev] + off;
                for (int i = 0; i < n; ++i) dst[i] += src[i];
            }
        }

        sampleTime_ += std::uint64_t(n);
        if (p.fading()) {
            p.xfadePos += n;
            if (!p.fading())
                transitionDoneSerial_.store(p.serial,
                                            std::memory_order_release);
        }
        off += n;
    }
}

}  // namespace pm
