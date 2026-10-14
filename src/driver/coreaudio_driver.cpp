// CoreAudio HAL driver: an IOProc on the default output device. The HAL
// calls us on its own real-time I/O thread; we only time the render and
// listen for the device's processor-overload notification (its xrun).
#include <CoreAudio/CoreAudio.h>

#include <vector>

#include "core/rt_guard.h"
#include "driver/driver.h"

namespace pm {
namespace {

AudioObjectPropertyAddress addr(AudioObjectPropertySelector sel,
                                AudioObjectPropertyScope scope =
                                    kAudioObjectPropertyScopeGlobal) {
    return {sel, scope, kAudioObjectPropertyElementMain};
}

class CoreAudioDriver final : public Driver {
public:
    explicit CoreAudioDriver(const DriverConfig& cfg) : cfg_(cfg) {}
    ~CoreAudioDriver() override {
        stop();
        if (workgroup_) os_release(workgroup_);
    }

    const DriverConfig& config() const override { return cfg_; }
    rt::Workgroup workgroup() const override { return workgroup_; }

    // Finds and configures the device; the workgroup is known after this.
    bool open(std::string* error) {
        auto fail = [&](const char* what, OSStatus s) {
            if (error)
                *error = std::string(what) + " (OSStatus " +
                         std::to_string(int(s)) + ")";
            return false;
        };
        UInt32 size = sizeof(device_);
        auto a = addr(kAudioHardwarePropertyDefaultOutputDevice);
        OSStatus s = AudioObjectGetPropertyData(kAudioObjectSystemObject, &a,
                                                0, nullptr, &size, &device_);
        if (s || device_ == kAudioObjectUnknown)
            return fail("no default output device", s);

        Float64 rate = cfg_.sampleRate;
        a = addr(kAudioDevicePropertyNominalSampleRate);
        AudioObjectSetPropertyData(device_, &a, 0, nullptr, sizeof(rate),
                                   &rate);
        size = sizeof(rate);
        AudioObjectGetPropertyData(device_, &a, 0, nullptr, &size, &rate);
        cfg_.sampleRate = rate;

        UInt32 frames = UInt32(cfg_.blockSize);
        a = addr(kAudioDevicePropertyBufferFrameSize);
        AudioObjectSetPropertyData(device_, &a, 0, nullptr, sizeof(frames),
                                   &frames);
        size = sizeof(frames);
        AudioObjectGetPropertyData(device_, &a, 0, nullptr, &size, &frames);
        cfg_.blockSize = int(frames);

        cfg_.numInputs = 0;
        cfg_.numOutputs = countChannels(kAudioObjectPropertyScopeOutput);
        outPtrs_.assign(std::size_t(cfg_.numOutputs), nullptr);
        scratch_.assign(std::size_t(cfg_.numOutputs) * 4096, 0.0f);

        size = sizeof(workgroup_);
        a = addr(kAudioDevicePropertyIOThreadOSWorkgroup);
        if (AudioObjectGetPropertyData(device_, &a, 0, nullptr, &size,
                                       &workgroup_))
            workgroup_ = nullptr;
        return true;
    }

    bool start(RenderFn fn, void* ctx, std::string* error) override {
        stop();
        auto fail = [&](const char* what, OSStatus s) {
            if (error)
                *error = std::string(what) + " (OSStatus " +
                         std::to_string(int(s)) + ")";
            return false;
        };
        fn_ = fn;
        ctx_ = ctx;
        auto a = addr(kAudioDeviceProcessorOverload);
        AudioObjectAddPropertyListener(device_, &a, &CoreAudioDriver::onOverload,
                                       this);

        OSStatus s = AudioDeviceCreateIOProcID(
            device_, &CoreAudioDriver::ioProc, this, &proc_);
        if (s) return fail("AudioDeviceCreateIOProcID", s);
        s = AudioDeviceStart(device_, proc_);
        if (s) return fail("AudioDeviceStart", s);
        return true;
    }

    void stop() override {
        if (!proc_) return;
        AudioDeviceStop(device_, proc_);
        AudioDeviceDestroyIOProcID(device_, proc_);
        auto a = addr(kAudioDeviceProcessorOverload);
        AudioObjectRemovePropertyListener(
            device_, &a, &CoreAudioDriver::onOverload, this);
        proc_ = nullptr;
    }

private:
    int countChannels(AudioObjectPropertyScope scope) {
        auto a = addr(kAudioDevicePropertyStreamConfiguration, scope);
        UInt32 size = 0;
        if (AudioObjectGetPropertyDataSize(device_, &a, 0, nullptr, &size))
            return 0;
        std::vector<char> buf(size);
        auto* list = reinterpret_cast<AudioBufferList*>(buf.data());
        if (AudioObjectGetPropertyData(device_, &a, 0, nullptr, &size, list))
            return 0;
        int n = 0;
        for (UInt32 i = 0; i < list->mNumberBuffers; ++i)
            n += int(list->mBuffers[i].mNumberChannels);
        return n;
    }

    static OSStatus onOverload(AudioObjectID, UInt32,
                               const AudioObjectPropertyAddress*, void* me) {
        static_cast<CoreAudioDriver*>(me)->stats_.xruns.fetch_add(1);
        return noErr;
    }

    static OSStatus ioProc(AudioObjectID, const AudioTimeStamp*,
                           const AudioBufferList*, const AudioTimeStamp*,
                           AudioBufferList* out, const AudioTimeStamp*,
                           void* me) {
        static_cast<CoreAudioDriver*>(me)->render(out);
        return noErr;
    }

    // HAL buffers are interleaved float32; the engine renders planar into
    // scratch and we interleave.
    void render(AudioBufferList* out) {
        rt::ScopedRealtime realtime;
        if (!out || out->mNumberBuffers == 0) return;
        const auto& b0 = out->mBuffers[0];
        const int ch0 = int(b0.mNumberChannels);
        const int frames = int(b0.mDataByteSize / (sizeof(float) * ch0));
        if (frames > 4096) return;
        for (int c = 0; c < cfg_.numOutputs; ++c)
            outPtrs_[c] = &scratch_[std::size_t(c) * 4096];

        const auto t0 = rt::nowNs();
        fn_(ctx_, nullptr, 0, outPtrs_.data(), cfg_.numOutputs, frames);
        const auto t1 = rt::nowNs();

        int base = 0;
        for (UInt32 b = 0; b < out->mNumberBuffers; ++b) {
            auto& buf = out->mBuffers[b];
            auto* dst = static_cast<float*>(buf.mData);
            const int nch = int(buf.mNumberChannels);
            for (int i = 0; i < frames; ++i)
                for (int c = 0; c < nch; ++c)
                    dst[i * nch + c] = base + c < cfg_.numOutputs
                                           ? outPtrs_[base + c][i]
                                           : 0.0f;
            base += nch;
        }

        stats_.callbacks.fetch_add(1, std::memory_order_relaxed);
        if (!stats_.samples.push({std::uint32_t(t1 - t0),
                                  std::uint32_t(frames)}))
            stats_.dropped.fetch_add(1, std::memory_order_relaxed);
    }

    DriverConfig cfg_;
    AudioObjectID device_ = kAudioObjectUnknown;
    AudioDeviceIOProcID proc_ = nullptr;
    os_workgroup_t workgroup_ = nullptr;
    RenderFn fn_ = nullptr;
    void* ctx_ = nullptr;
    std::vector<float*> outPtrs_;
    std::vector<float> scratch_;
};

}  // namespace

std::unique_ptr<Driver> makeCoreAudioDriver(const DriverConfig& cfg,
                                            std::string* error) {
    auto d = std::make_unique<CoreAudioDriver>(cfg);
    if (!d->open(error)) return nullptr;
    return d;
}

}  // namespace pm
