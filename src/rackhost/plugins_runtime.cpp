#include "plugins_runtime.h"

#include <algorithm>
#include <atomic>
#include <cmath>

#include "plugins.h"

namespace {

constexpr uint32_t kPluginTypeSineMixer = 1;
constexpr uint32_t kPluginTypeGain = 2;
constexpr uint32_t kPluginTypeSoftClip = 3;

class SineMixerPlugin final : public RackPlugin {
public:
    static constexpr uint32_t kParamInputVol = 1;
    static constexpr uint32_t kParamSineVol = 2;

    uint32_t type_id() const override { return kPluginTypeSineMixer; }
    const char* display_name() const override { return "Sine Mixer (Test)"; }

    float process_sample(float in, float sample_rate) override {
        const float input_vol = input_vol_.load(std::memory_order_relaxed);
        const float sine_vol = sine_vol_.load(std::memory_order_relaxed);

        const float sine = std::sin(phase_);
        phase_ += (2.0f * 3.14159265358979323846f * 220.0f) / std::max(sample_rate, 1.0f);
        if (phase_ > 2.0f * 3.14159265358979323846f) {
            phase_ -= 2.0f * 3.14159265358979323846f;
        }

        return in * input_vol + sine * sine_vol;
    }

    std::vector<PluginParamSpec> param_specs() const override {
        return {
            {kParamInputVol, "Input Vol", vessel::ParamWidget::SLIDER, 0.0f, 2.0f, 1.0f},
            {kParamSineVol, "Sine Vol", vessel::ParamWidget::SLIDER, 0.0f, 1.0f, 0.0f},
        };
    }

    float get_param(uint32_t param_id) const override {
        if (param_id == kParamInputVol) return input_vol_.load(std::memory_order_relaxed);
        if (param_id == kParamSineVol) return sine_vol_.load(std::memory_order_relaxed);
        return 0.0f;
    }

    void set_param(uint32_t param_id, float value) override {
        if (param_id == kParamInputVol) {
            input_vol_.store(std::clamp(value, 0.0f, 2.0f), std::memory_order_relaxed);
        } else if (param_id == kParamSineVol) {
            sine_vol_.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
        }
    }

private:
    std::atomic<float> input_vol_{1.0f};
    std::atomic<float> sine_vol_{0.0f};
    float phase_ = 0.0f;
};

class GainPlugin final : public RackPlugin {
public:
    static constexpr uint32_t kParamGain = 1;

    uint32_t type_id() const override { return kPluginTypeGain; }
    const char* display_name() const override { return "Gain"; }

    float process_sample(float in, float) override {
        return in * gain_.load(std::memory_order_relaxed);
    }

    std::vector<PluginParamSpec> param_specs() const override {
        return {
            {kParamGain, "Gain", vessel::ParamWidget::SLIDER, 0.0f, 2.0f, 1.0f},
        };
    }

    float get_param(uint32_t param_id) const override {
        if (param_id == kParamGain) return gain_.load(std::memory_order_relaxed);
        return 0.0f;
    }

    void set_param(uint32_t param_id, float value) override {
        if (param_id == kParamGain) {
            gain_.store(std::clamp(value, 0.0f, 2.0f), std::memory_order_relaxed);
        }
    }

private:
    std::atomic<float> gain_{1.0f};
};

class SoftClipPlugin final : public RackPlugin {
public:
    static constexpr uint32_t kParamDrive = 1;

    uint32_t type_id() const override { return kPluginTypeSoftClip; }
    const char* display_name() const override { return "Soft Clip"; }

    float process_sample(float in, float) override {
        const float drive = drive_.load(std::memory_order_relaxed);
        const float x = in * std::max(0.1f, drive);
        return std::tanh(x);
    }

    std::vector<PluginParamSpec> param_specs() const override {
        return {
            {kParamDrive, "Drive", vessel::ParamWidget::SLIDER, 0.1f, 8.0f, 1.0f},
        };
    }

    float get_param(uint32_t param_id) const override {
        if (param_id == kParamDrive) return drive_.load(std::memory_order_relaxed);
        return 0.0f;
    }

    void set_param(uint32_t param_id, float value) override {
        if (param_id == kParamDrive) {
            drive_.store(std::clamp(value, 0.1f, 8.0f), std::memory_order_relaxed);
        }
    }

private:
    std::atomic<float> drive_{1.0f};
};

}  // namespace

std::unique_ptr<RackPlugin> create_plugin(uint32_t plugin_type_id) {
    switch (plugin_type_id) {
        case kPluginTypeSineMixer:
            return std::make_unique<SineMixerPlugin>();
        case kPluginTypeGain:
            return std::make_unique<GainPlugin>();
        case kPluginTypeSoftClip:
            return std::make_unique<SoftClipPlugin>();
        default:
            break;
    }
    return nullptr;
}

bool plugin_is_supported(uint32_t plugin_type_id) {
    const vessel::PluginManifestEntry* entry = vessel::find_plugin_manifest_entry(plugin_type_id);
    if (!entry || !entry->is_builtin) {
        return false;
    }
    return create_plugin(plugin_type_id) != nullptr;
}
