#include "proto/protocol.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <cmath>

namespace pm::proto {
namespace {

const char* kindName(NodeKind k) {
    switch (k) {
    case NodeKind::Input: return "input";
    case NodeKind::Generator: return "generator";
    case NodeKind::Strip: return "strip";
    case NodeKind::Bus: return "bus";
    case NodeKind::Output: return "output";
    }
    return "strip";
}

bool kindFrom(const QString& s, NodeKind& k) {
    static const std::pair<const char*, NodeKind> table[] = {
        {"input", NodeKind::Input},   {"generator", NodeKind::Generator},
        {"strip", NodeKind::Strip},   {"bus", NodeKind::Bus},
        {"output", NodeKind::Output}};
    for (const auto& [name, kind] : table)
        if (s == QLatin1String(name)) {
            k = kind;
            return true;
        }
    return false;
}

const char* bandName(EqBand::Type t) {
    switch (t) {
    case EqBand::Type::LowShelf: return "lowShelf";
    case EqBand::Type::HighShelf: return "highShelf";
    case EqBand::Type::Peak: break;
    }
    return "peak";
}

EqBand::Type bandFrom(const QString& s) {
    if (s == "lowShelf") return EqBand::Type::LowShelf;
    if (s == "highShelf") return EqBand::Type::HighShelf;
    return EqBand::Type::Peak;
}

QJsonObject processingToJson(const ProcessingDesc& p) {
    QJsonArray bands;
    for (const auto& b : p.bands)
        bands.append(QJsonObject{{"type", bandName(b.type)},
                                 {"freqHz", b.freqHz},
                                 {"q", b.q},
                                 {"gainDb", b.gainDb}});
    QJsonArray inserts;
    for (const auto& i : p.inserts) {
        switch (i.type) {
        case InsertDesc::Type::Gain:
            inserts.append(QJsonObject{{"type", "gain"}, {"value", i.value}});
            break;
        case InsertDesc::Type::CpuLoad:
            inserts.append(QJsonObject{{"type", "cpuLoad"}, {"value", i.value}});
            break;
        case InsertDesc::Type::Plugin:
            inserts.append(QJsonObject{
                {"type", "plugin"},
                {"format", QString::fromStdString(i.format)},
                {"path", QString::fromStdString(i.path)},
                {"id", QString::fromStdString(i.pluginId)},
                {"sandboxed", i.sandboxed},
                {"state", QString::fromStdString(i.state)}});
            break;
        }
    }
    const auto& g = p.gateParams;
    const auto& c = p.compParams;
    return QJsonObject{
        {"hpf", QJsonObject{{"on", p.hpf}, {"freqHz", p.hpfHz}}},
        {"gate", QJsonObject{{"on", p.gate},
                             {"thresholdDb", g.thresholdDb},
                             {"rangeDb", g.rangeDb},
                             {"attackMs", g.attackMs},
                             {"holdMs", g.holdMs},
                             {"releaseMs", g.releaseMs}}},
        {"eq", QJsonObject{{"on", p.eq}, {"bands", bands}}},
        {"comp", QJsonObject{{"on", p.comp},
                             {"thresholdDb", c.thresholdDb},
                             {"ratio", c.ratio},
                             {"kneeDb", c.kneeDb},
                             {"attackMs", c.attackMs},
                             {"releaseMs", c.releaseMs},
                             {"makeupDb", c.makeupDb}}},
        {"limiter",
         QJsonObject{{"on", p.limiter}, {"ceilingDb", p.limiterCeilingDb}}},
        {"inserts", inserts}};
}

float num(const QJsonObject& o, const char* key, float fallback) {
    const auto v = o.value(QLatin1String(key));
    return v.isDouble() ? float(v.toDouble()) : fallback;
}

void processingFromJson(const QJsonObject& o, ProcessingDesc& p) {
    const auto hpf = o["hpf"].toObject();
    p.hpf = hpf["on"].toBool(p.hpf);
    p.hpfHz = num(hpf, "freqHz", p.hpfHz);

    const auto gate = o["gate"].toObject();
    auto& g = p.gateParams;
    p.gate = gate["on"].toBool(p.gate);
    g.thresholdDb = num(gate, "thresholdDb", g.thresholdDb);
    g.rangeDb = num(gate, "rangeDb", g.rangeDb);
    g.attackMs = num(gate, "attackMs", g.attackMs);
    g.holdMs = num(gate, "holdMs", g.holdMs);
    g.releaseMs = num(gate, "releaseMs", g.releaseMs);

    const auto eq = o["eq"].toObject();
    p.eq = eq["on"].toBool(p.eq);
    const auto bands = eq["bands"].toArray();
    for (int i = 0; i < int(p.bands.size()) && i < bands.size(); ++i) {
        const auto b = bands[i].toObject();
        auto& band = p.bands[std::size_t(i)];
        band.type = bandFrom(b["type"].toString());
        band.freqHz = num(b, "freqHz", band.freqHz);
        band.q = num(b, "q", band.q);
        band.gainDb = num(b, "gainDb", band.gainDb);
    }

    const auto comp = o["comp"].toObject();
    auto& c = p.compParams;
    p.comp = comp["on"].toBool(p.comp);
    c.thresholdDb = num(comp, "thresholdDb", c.thresholdDb);
    c.ratio = num(comp, "ratio", c.ratio);
    c.kneeDb = num(comp, "kneeDb", c.kneeDb);
    c.attackMs = num(comp, "attackMs", c.attackMs);
    c.releaseMs = num(comp, "releaseMs", c.releaseMs);
    c.makeupDb = num(comp, "makeupDb", c.makeupDb);

    const auto lim = o["limiter"].toObject();
    p.limiter = lim["on"].toBool(p.limiter);
    p.limiterCeilingDb = num(lim, "ceilingDb", p.limiterCeilingDb);

    p.inserts.clear();
    for (const auto& v : o["inserts"].toArray()) {
        const auto i = v.toObject();
        const auto type = i["type"].toString();
        InsertDesc d;
        d.type = type == "cpuLoad"  ? InsertDesc::Type::CpuLoad
                 : type == "plugin" ? InsertDesc::Type::Plugin
                                    : InsertDesc::Type::Gain;
        d.value = num(i, "value", 0);
        d.format = i["format"].toString().toStdString();
        d.path = i["path"].toString().toStdString();
        d.pluginId = i["id"].toString().toStdString();
        d.sandboxed = i["sandboxed"].toBool(false);
        d.state = i["state"].toString().toStdString();
        p.inserts.push_back(std::move(d));
    }
}

}  // namespace

QJsonObject graphToJson(const GraphDesc& g) {
    QJsonArray nodes, edges;
    for (const auto& n : g.nodes) {
        QJsonObject o{{"id", double(n.id)},
                      {"kind", kindName(n.kind)},
                      {"channels", n.channels},
                      {"name", QString::fromStdString(n.name)},
                      {"faderDb", n.faderDb},
                      {"mute", n.mute},
                      {"processing", processingToJson(n.processing)}};
        if (n.kind == NodeKind::Input || n.kind == NodeKind::Output)
            o["ioChannel"] = n.ioChannel;
        if (n.kind == NodeKind::Generator)
            o["generator"] = QJsonObject{{"seed", double(n.generator.seed)},
                                         {"freqHz", n.generator.freqHz},
                                         {"sineDb", n.generator.sineDb},
                                         {"noiseDb", n.generator.noiseDb},
                                         {"bursts", n.generator.bursts}};
        if (n.mixMinus.follows) {
            QJsonArray ex;
            for (auto id : n.mixMinus.exclude) ex.append(double(id));
            o["mixMinus"] = QJsonObject{
                {"follows", double(n.mixMinus.follows)}, {"exclude", ex}};
        }
        nodes.append(o);
    }
    for (const auto& e : g.edges) {
        QJsonObject o{{"from", double(e.from)},
                      {"to", double(e.to)},
                      {"gainDb", e.gainDb},
                      {"pan", e.pan}};
        if (e.derived) o["derived"] = true;
        edges.append(o);
    }
    return QJsonObject{{"nodes", nodes}, {"edges", edges}};
}

bool graphFromJson(const QJsonObject& o, GraphDesc& g, QString* error) {
    auto fail = [&](const QString& why) {
        if (error) *error = why;
        return false;
    };
    GraphDesc out;
    for (const auto& v : o["nodes"].toArray()) {
        const auto n = v.toObject();
        NodeDesc d;
        if (!n["id"].isDouble()) return fail("node without id");
        d.id = NodeId(n["id"].toDouble());
        if (!kindFrom(n["kind"].toString(), d.kind))
            return fail("node " + QString::number(d.id) + ": unknown kind");
        d.channels = n["channels"].toInt(1);
        d.name = n["name"].toString().toStdString();
        d.ioChannel = n["ioChannel"].toInt(0);
        d.faderDb = num(n, "faderDb", 0);
        d.mute = n["mute"].toBool(false);
        processingFromJson(n["processing"].toObject(), d.processing);
        const auto gen = n["generator"].toObject();
        d.generator.seed = std::uint32_t(gen["seed"].toDouble(1));
        d.generator.freqHz = num(gen, "freqHz", d.generator.freqHz);
        d.generator.sineDb = num(gen, "sineDb", d.generator.sineDb);
        d.generator.noiseDb = num(gen, "noiseDb", d.generator.noiseDb);
        d.generator.bursts = gen["bursts"].toBool(d.generator.bursts);
        const auto mm = n["mixMinus"].toObject();
        d.mixMinus.follows = NodeId(mm["follows"].toDouble(0));
        for (const auto& x : mm["exclude"].toArray())
            d.mixMinus.exclude.push_back(NodeId(x.toDouble()));
        out.nodes.push_back(std::move(d));
    }
    for (const auto& v : o["edges"].toArray()) {
        const auto e = v.toObject();
        if (!e["from"].isDouble() || !e["to"].isDouble())
            return fail("edge without endpoints");
        out.edges.push_back({NodeId(e["from"].toDouble()),
                             NodeId(e["to"].toDouble()), num(e, "gainDb", 0),
                             num(e, "pan", 0), e["derived"].toBool(false)});
    }
    g = std::move(out);
    return true;
}

float toDbfs(float linear) {
    return linear > 1e-6f ? 20.0f * std::log10(linear) : -120.0f;
}

QByteArray frame(const QJsonObject& o) {
    return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
}

}  // namespace pm::proto
