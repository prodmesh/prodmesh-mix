// CoreAudio HAL driver. One IOProc on one device; the HAL calls us on its
// real-time I/O thread with both input and output buffers.
//
// Input and output on different devices (the built-in mic and speakers, a
// USB mic and an interface...) run on different clocks. Like other pro
// apps we then build a private aggregate device: the output is the clock
// master and the input is drift-corrected into it. It is private (no other
// app sees it) and destroyed when we stop.
#include <CoreAudio/CoreAudio.h>

#include <algorithm>
#include <cstring>
#include <thread>
#include <vector>

#include "core/rt_guard.h"
#include "driver/driver.h"

namespace pm {
namespace {

constexpr int kMaxFrames = 4096;

AudioObjectPropertyAddress addr(AudioObjectPropertySelector sel,
                                AudioObjectPropertyScope scope =
                                    kAudioObjectPropertyScopeGlobal) {
    return {sel, scope, kAudioObjectPropertyElementMain};
}

std::string cfString(CFStringRef s) {
    if (!s) return {};
    char buf[512];
    if (!CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8)) return {};
    return buf;
}

std::string stringProp(AudioObjectID id, AudioObjectPropertySelector sel) {
    CFStringRef s = nullptr;
    UInt32 size = sizeof s;
    auto a = addr(sel);
    if (AudioObjectGetPropertyData(id, &a, 0, nullptr, &size, &s) || !s) return {};
    auto out = cfString(s);
    CFRelease(s);
    return out;
}

int channelCount(AudioObjectID id, AudioObjectPropertyScope scope) {
    auto a = addr(kAudioDevicePropertyStreamConfiguration, scope);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(id, &a, 0, nullptr, &size)) return 0;
    std::vector<char> buf(size);
    auto* list = reinterpret_cast<AudioBufferList*>(buf.data());
    if (AudioObjectGetPropertyData(id, &a, 0, nullptr, &size, list)) return 0;
    int n = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i)
        n += int(list->mBuffers[i].mNumberChannels);
    return n;
}

std::vector<AudioObjectID> allDevices() {
    auto a = addr(kAudioHardwarePropertyDevices);
    UInt32 size = 0;
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &size);
    std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size,
                               ids.data());
    return ids;
}

AudioObjectID defaultDevice(bool input) {
    AudioObjectID id = kAudioObjectUnknown;
    UInt32 size = sizeof id;
    auto a = addr(input ? kAudioHardwarePropertyDefaultInputDevice
                        : kAudioHardwarePropertyDefaultOutputDevice);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size, &id);
    return id;
}

// By UID, exact name, or (case-insensitive) name prefix; "" = default.
AudioObjectID findDevice(const std::string& want, bool input) {
    if (want.empty()) return defaultDevice(input);
    const auto scope = input ? kAudioObjectPropertyScopeInput
                             : kAudioObjectPropertyScopeOutput;
    AudioObjectID prefix = kAudioObjectUnknown;
    for (auto id : allDevices()) {
        if (channelCount(id, scope) == 0) continue;
        const auto name = stringProp(id, kAudioObjectPropertyName);
        if (stringProp(id, kAudioDevicePropertyDeviceUID) == want || name == want)
            return id;
        if (prefix == kAudioObjectUnknown && name.size() >= want.size() &&
            strncasecmp(name.c_str(), want.c_str(), want.size()) == 0)
            prefix = id;
    }
    return prefix;
}

class CoreAudioDriver final : public Driver {
public:
    explicit CoreAudioDriver(const DriverConfig& cfg) : cfg_(cfg) {}
    ~CoreAudioDriver() override {
        stop();
        if (workgroup_) os_release(workgroup_);
        if (aggregate_ != kAudioObjectUnknown)
            AudioHardwareDestroyAggregateDevice(aggregate_);
    }

    const DriverConfig& config() const override { return cfg_; }
    rt::Workgroup workgroup() const override { return workgroup_; }

    // Finds and configures the device(s); the workgroup is known after this.
    bool open(std::string* error) {
        auto fail = [&](const std::string& what, OSStatus s = 0) {
            if (error)
                *error = what + (s ? " (OSStatus " + std::to_string(int(s)) + ")" : "");
            return false;
        };
        const AudioObjectID out = findDevice(cfg_.outputDevice, false);
        if (out == kAudioObjectUnknown)
            return fail("no output device" +
                        (cfg_.outputDevice.empty() ? "" : " \"" + cfg_.outputDevice + "\""));
        AudioObjectID in = kAudioObjectUnknown;
        if (cfg_.inputDevice != "none") {
            in = findDevice(cfg_.inputDevice, true);
            if (in == kAudioObjectUnknown && !cfg_.inputDevice.empty())
                return fail("no input device \"" + cfg_.inputDevice + "\"");
        }
        device_ = out;
        if (in == kAudioObjectUnknown) {
            useInputs_ = false;
        } else if (in != out) {
            std::string err;
            if (!makeAggregate(in, out, &err)) return fail(err);
            device_ = aggregate_;
        }
        // What the operator recognises: the real devices, not our aggregate.
        deviceName_ = device_ == aggregate_
                          ? stringProp(in, kAudioObjectPropertyName) + " \u2192 " +
                                stringProp(out, kAudioObjectPropertyName)
                          : stringProp(device_, kAudioObjectPropertyName);

        Float64 rate = cfg_.sampleRate;
        auto a = addr(kAudioDevicePropertyNominalSampleRate);
        AudioObjectSetPropertyData(device_, &a, 0, nullptr, sizeof(rate), &rate);
        UInt32 size = sizeof(rate);
        AudioObjectGetPropertyData(device_, &a, 0, nullptr, &size, &rate);
        cfg_.sampleRate = rate;

        UInt32 frames = UInt32(cfg_.blockSize);
        a = addr(kAudioDevicePropertyBufferFrameSize);
        AudioObjectSetPropertyData(device_, &a, 0, nullptr, sizeof(frames), &frames);
        size = sizeof(frames);
        AudioObjectGetPropertyData(device_, &a, 0, nullptr, &size, &frames);
        cfg_.blockSize = int(frames);

        cfg_.numInputs = useInputs_ ? channelCount(device_, kAudioObjectPropertyScopeInput) : 0;
        cfg_.numOutputs = channelCount(device_, kAudioObjectPropertyScopeOutput);
        outPtrs_.assign(std::size_t(cfg_.numOutputs), nullptr);
        inPtrs_.assign(std::size_t(cfg_.numInputs), nullptr);
        outScratch_.assign(std::size_t(cfg_.numOutputs) * kMaxFrames, 0.0f);
        inScratch_.assign(std::size_t(cfg_.numInputs) * kMaxFrames, 0.0f);

        size = sizeof(workgroup_);
        a = addr(kAudioDevicePropertyIOThreadOSWorkgroup);
        if (AudioObjectGetPropertyData(device_, &a, 0, nullptr, &size, &workgroup_))
            workgroup_ = nullptr;
        return true;
    }

    bool start(RenderFn fn, void* ctx, std::string* error) override {
        stop();
        fn_ = fn;
        ctx_ = ctx;
        auto a = addr(kAudioDeviceProcessorOverload);
        AudioObjectAddPropertyListener(device_, &a, &CoreAudioDriver::onOverload, this);
        OSStatus s = AudioDeviceCreateIOProcID(device_, &CoreAudioDriver::ioProc,
                                               this, &proc_);
        if (!s) s = AudioDeviceStart(device_, proc_);
        if (s) {
            if (error) *error = "cannot start " + deviceName_ + " (OSStatus " +
                                std::to_string(int(s)) + ")";
            return false;
        }
        return true;
    }

    void stop() override {
        if (!proc_) return;
        AudioDeviceStop(device_, proc_);
        AudioDeviceDestroyIOProcID(device_, proc_);
        auto a = addr(kAudioDeviceProcessorOverload);
        AudioObjectRemovePropertyListener(device_, &a, &CoreAudioDriver::onOverload,
                                          this);
        proc_ = nullptr;
    }

    std::string deviceName() const override { return deviceName_; }

private:
    bool makeAggregate(AudioObjectID in, AudioObjectID out, std::string* error) {
        const auto inUid = stringProp(in, kAudioDevicePropertyDeviceUID);
        const auto outUid = stringProp(out, kAudioDevicePropertyDeviceUID);
        auto str = [](const std::string& s) {
            return CFStringCreateWithCString(nullptr, s.c_str(), kCFStringEncodingUTF8);
        };
        auto sub = [&](const std::string& uid, bool drift) {
            CFMutableDictionaryRef d = CFDictionaryCreateMutable(
                nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFStringRef u = str(uid);
            CFDictionarySetValue(d, CFSTR(kAudioSubDeviceUIDKey), u);
            CFRelease(u);
            int one = drift ? 1 : 0;
            CFNumberRef n = CFNumberCreate(nullptr, kCFNumberIntType, &one);
            CFDictionarySetValue(d, CFSTR(kAudioSubDeviceDriftCompensationKey), n);
            CFRelease(n);
            return d;
        };
        CFMutableArrayRef subs = CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks);
        // Output first: it is the clock master; the input is drift-corrected.
        CFMutableDictionaryRef so = sub(outUid, false), si = sub(inUid, true);
        CFArrayAppendValue(subs, so);
        CFArrayAppendValue(subs, si);
        CFRelease(so);
        CFRelease(si);

        CFMutableDictionaryRef desc = CFDictionaryCreateMutable(
            nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFStringRef name = str("ProdMesh Mix I/O");
        CFStringRef uid = str("com.prodmesh.mix.aggregate." + std::to_string(getpid()));
        CFStringRef master = str(outUid);
        int one = 1;
        CFNumberRef yes = CFNumberCreate(nullptr, kCFNumberIntType, &one);
        CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceNameKey), name);
        CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceUIDKey), uid);
        CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceIsPrivateKey), yes);
        CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceSubDeviceListKey), subs);
        CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceMainSubDeviceKey), master);
        const OSStatus s = AudioHardwareCreateAggregateDevice(desc, &aggregate_);
        for (CFTypeRef r : {CFTypeRef(name), CFTypeRef(uid), CFTypeRef(master),
                            CFTypeRef(yes), CFTypeRef(subs), CFTypeRef(desc)})
            CFRelease(r);
        if (s || aggregate_ == kAudioObjectUnknown) {
            if (error) *error = "cannot combine \"" + stringProp(in, kAudioObjectPropertyName) +
                                "\" and \"" + stringProp(out, kAudioObjectPropertyName) +
                                "\" (OSStatus " + std::to_string(int(s)) + ")";
            aggregate_ = kAudioObjectUnknown;
            return false;
        }
        // The HAL publishes the new device's streams asynchronously.
        for (int i = 0; i < 100 && channelCount(aggregate_, kAudioObjectPropertyScopeOutput) == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return true;
    }

    static OSStatus onOverload(AudioObjectID, UInt32, const AudioObjectPropertyAddress*,
                               void* me) {
        static_cast<CoreAudioDriver*>(me)->stats_.xruns.fetch_add(1);
        return noErr;
    }

    static OSStatus ioProc(AudioObjectID, const AudioTimeStamp*, const AudioBufferList* in,
                           const AudioTimeStamp*, AudioBufferList* out,
                           const AudioTimeStamp*, void* me) {
        static_cast<CoreAudioDriver*>(me)->render(in, out);
        return noErr;
    }

    // HAL buffers are interleaved per stream; the engine is planar.
    void render(const AudioBufferList* in, AudioBufferList* out) {
        rt::ScopedRealtime realtime;
        if (!out || out->mNumberBuffers == 0) return;
        const auto& b0 = out->mBuffers[0];
        const int frames = int(b0.mDataByteSize /
                               (sizeof(float) * std::max<UInt32>(b0.mNumberChannels, 1)));
        if (frames > kMaxFrames) return;
        for (int c = 0; c < cfg_.numOutputs; ++c)
            outPtrs_[std::size_t(c)] = &outScratch_[std::size_t(c) * kMaxFrames];

        int base = 0;
        if (in && cfg_.numInputs > 0)
            for (UInt32 b = 0; b < in->mNumberBuffers; ++b) {
                const auto& buf = in->mBuffers[b];
                const auto* src = static_cast<const float*>(buf.mData);
                const int nch = int(buf.mNumberChannels);
                for (int c = 0; c < nch && base + c < cfg_.numInputs; ++c) {
                    float* dst = &inScratch_[std::size_t(base + c) * kMaxFrames];
                    inPtrs_[std::size_t(base + c)] = dst;
                    for (int i = 0; i < frames; ++i)
                        dst[i] = src ? src[i * nch + c] : 0.0f;
                }
                base += nch;
            }

        const auto t0 = rt::nowNs();
        fn_(ctx_, cfg_.numInputs ? inPtrs_.data() : nullptr, cfg_.numInputs,
            outPtrs_.data(), cfg_.numOutputs, frames);
        const auto t1 = rt::nowNs();

        base = 0;
        for (UInt32 b = 0; b < out->mNumberBuffers; ++b) {
            auto& buf = out->mBuffers[b];
            auto* dst = static_cast<float*>(buf.mData);
            const int nch = int(buf.mNumberChannels);
            for (int i = 0; i < frames; ++i)
                for (int c = 0; c < nch; ++c)
                    dst[i * nch + c] =
                        base + c < cfg_.numOutputs ? outPtrs_[std::size_t(base + c)][i] : 0.0f;
            base += nch;
        }

        stats_.callbacks.fetch_add(1, std::memory_order_relaxed);
        if (!stats_.samples.push({std::uint32_t(t1 - t0), std::uint32_t(frames)}))
            stats_.dropped.fetch_add(1, std::memory_order_relaxed);
    }

    DriverConfig cfg_;
    AudioObjectID device_ = kAudioObjectUnknown;
    AudioObjectID aggregate_ = kAudioObjectUnknown;
    AudioDeviceIOProcID proc_ = nullptr;
    os_workgroup_t workgroup_ = nullptr;
    std::string deviceName_;
    bool useInputs_ = true;
    RenderFn fn_ = nullptr;
    void* ctx_ = nullptr;
    std::vector<float*> outPtrs_;
    std::vector<const float*> inPtrs_;
    std::vector<float> outScratch_, inScratch_;
};

}  // namespace

std::unique_ptr<Driver> makeCoreAudioDriver(const DriverConfig& cfg,
                                            std::string* error) {
    auto d = std::make_unique<CoreAudioDriver>(cfg);
    if (!d->open(error)) return nullptr;
    return d;
}

std::vector<AudioDeviceInfo> listAudioDevices() {
    std::vector<AudioDeviceInfo> out;
    const auto defIn = defaultDevice(true), defOut = defaultDevice(false);
    for (auto id : allDevices()) {
        Float64 rate = 0;
        UInt32 size = sizeof rate;
        auto a = addr(kAudioDevicePropertyNominalSampleRate);
        AudioObjectGetPropertyData(id, &a, 0, nullptr, &size, &rate);
        out.push_back({stringProp(id, kAudioObjectPropertyName),
                       stringProp(id, kAudioDevicePropertyDeviceUID),
                       channelCount(id, kAudioObjectPropertyScopeInput),
                       channelCount(id, kAudioObjectPropertyScopeOutput), rate,
                       id == defIn, id == defOut});
    }
    return out;
}

}  // namespace pm
