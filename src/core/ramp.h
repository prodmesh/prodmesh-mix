// Linear parameter ramp, advanced per sample on the audio thread. Setting the
// target it already holds is a no-op, which is what keeps unaffected paths
// bit-identical across a graph swap.
#pragma once

namespace pm {

class LinearRamp {
public:
    explicit LinearRamp(float v = 0) : cur_(v), target_(v) {}

    // Re-arming the current target, even mid-ramp, changes nothing: an
    // unrelated edit must not perturb a fader move in progress.
    void setTarget(float t, int samples) {
        if (t == target_) return;
        target_ = t;
        if (samples <= 0) {
            jump(t);
            return;
        }
        remaining_ = samples;
        step_ = (target_ - cur_) / float(samples);
    }

    void jump(float v) {
        cur_ = target_ = v;
        remaining_ = 0;
        step_ = 0;
    }

    float next() {
        if (remaining_ == 0) return cur_;
        if (--remaining_ == 0) cur_ = target_;
        else cur_ += step_;
        return cur_;
    }

    bool ramping() const { return remaining_ != 0; }
    float current() const { return cur_; }
    float target() const { return target_; }
    bool silent() const { return remaining_ == 0 && cur_ == 0.0f; }

    // out[i] += in[i] * gain, advancing the ramp. The steady-state branch
    // is a plain multiply-add so it vectorises.
    void mixInto(float* out, const float* in, int n) {
        int i = 0;
        for (; i < n && remaining_ != 0; ++i) out[i] += in[i] * next();
        const float g = cur_;
        for (; i < n; ++i) out[i] += in[i] * g;
    }

    // x[i] *= gain, advancing the ramp.
    void apply(float* x, int n) {
        int i = 0;
        for (; i < n && remaining_ != 0; ++i) x[i] *= next();
        const float g = cur_;
        if (g == 1.0f) return;
        for (; i < n; ++i) x[i] *= g;
    }

private:
    float cur_, target_;
    float step_ = 0;
    int remaining_ = 0;
};

}  // namespace pm
