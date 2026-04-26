#include "plugins_runtime.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

#include <lilv/lilv.h>
#include <lv2/core/lv2.h>
#include <lv2/ui/ui.h>

#include "plugins.h"

namespace {

constexpr uint32_t kPluginTypeSineMixer = 1;
constexpr uint32_t kPluginTypeGain = 2;
constexpr uint32_t kPluginTypeSoftClip = 3;
constexpr uint32_t kLv2PluginTypeBase = 10000;

float sanitize_default(float value, float fallback) {
    if (std::isnan(value) || std::isinf(value)) {
        return fallback;
    }
    return value;
}

float sanitize_min(float value) {
    if (std::isnan(value) || std::isinf(value)) {
        return 0.0f;
    }
    return value;
}

float sanitize_max(float value, float min_value) {
    if (std::isnan(value) || std::isinf(value) || value <= min_value) {
        return min_value + 1.0f;
    }
    return value;
}

class SineMixerPlugin final : public RackPlugin {
public:
    static constexpr uint32_t kParamInputVol = 1;
    static constexpr uint32_t kParamSineVol = 2;

    uint32_t type_id() const override { return kPluginTypeSineMixer; }
    const char* display_name() const override { return "Sine Mixer (Test)"; }

    float process_sample(float in, float sample_rate, int) override {
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

    float process_sample(float in, float, int) override {
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

    float process_sample(float in, float, int) override {
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

class Lv2RackPlugin final : public RackPlugin {
public:
    explicit Lv2RackPlugin(const DiscoveredLv2Plugin& desc)
        : plugin_type_id_(desc.plugin_type_id), display_name_(desc.name), uri_(desc.uri) {
        world_ = lilv_world_new();
        if (!world_) {
            valid_ = false;
            return;
        }

        lilv_world_load_all(world_);

        LilvNode* uri_node = lilv_new_uri(world_, uri_.c_str());
        if (!uri_node) {
            valid_ = false;
            return;
        }

        const LilvPlugins* all = lilv_world_get_all_plugins(world_);
        plugin_ = lilv_plugins_get_by_uri(all, uri_node);
        lilv_node_free(uri_node);

        if (!plugin_) {
            valid_ = false;
            return;
        }

        LilvNode* audio_class = lilv_new_uri(world_, LV2_CORE__AudioPort);
        LilvNode* control_class = lilv_new_uri(world_, LV2_CORE__ControlPort);
        LilvNode* input_class = lilv_new_uri(world_, LV2_CORE__InputPort);
        LilvNode* output_class = lilv_new_uri(world_, LV2_CORE__OutputPort);

        const uint32_t num_ports = lilv_plugin_get_num_ports(plugin_);
        std::vector<float> mins(num_ports, 0.0f);
        std::vector<float> maxs(num_ports, 1.0f);
        std::vector<float> defs(num_ports, 0.0f);
        lilv_plugin_get_port_ranges_float(plugin_, mins.data(), maxs.data(), defs.data());

        uint32_t next_param_id = 1;
        for (uint32_t i = 0; i < num_ports; ++i) {
            const LilvPort* port = lilv_plugin_get_port_by_index(plugin_, i);
            const bool is_audio = lilv_port_is_a(plugin_, port, audio_class);
            const bool is_control = lilv_port_is_a(plugin_, port, control_class);
            const bool is_input = lilv_port_is_a(plugin_, port, input_class);
            const bool is_output = lilv_port_is_a(plugin_, port, output_class);

            if (is_audio && is_input && audio_in_port_ == std::numeric_limits<uint32_t>::max()) {
                audio_in_port_ = i;
            }
            if (is_audio && is_output && audio_out_port_ == std::numeric_limits<uint32_t>::max()) {
                audio_out_port_ = i;
            }

            if (is_control && is_input) {
                const LilvNode* name_node = lilv_port_get_name(plugin_, port);
                const char* raw_name = name_node ? lilv_node_as_string(name_node) : nullptr;

                ControlParam p;
                p.param_id = next_param_id++;
                p.port_index = i;
                p.name = raw_name ? raw_name : "Control";
                p.min_value = sanitize_min(mins[i]);
                p.max_value = sanitize_max(maxs[i], p.min_value);
                p.default_value = std::clamp(sanitize_default(defs[i], p.min_value), p.min_value, p.max_value);
                p.value = p.default_value;
                controls_.push_back(std::move(p));
            }
        }

        lilv_node_free(audio_class);
        lilv_node_free(control_class);
        lilv_node_free(input_class);
        lilv_node_free(output_class);

        if (audio_in_port_ == std::numeric_limits<uint32_t>::max()
            || audio_out_port_ == std::numeric_limits<uint32_t>::max()) {
            valid_ = false;
            return;
        }

        instance_l_ = lilv_plugin_instantiate(plugin_, 48000.0, nullptr);
        instance_r_ = lilv_plugin_instantiate(plugin_, 48000.0, nullptr);
        if (!instance_l_ || !instance_r_) {
            valid_ = false;
            return;
        }

        control_values_l_.resize(controls_.size(), 0.0f);
        control_values_r_.resize(controls_.size(), 0.0f);
        for (size_t i = 0; i < controls_.size(); ++i) {
            control_values_l_[i] = controls_[i].value;
            control_values_r_[i] = controls_[i].value;
            lilv_instance_connect_port(instance_l_, controls_[i].port_index, &control_values_l_[i]);
            lilv_instance_connect_port(instance_r_, controls_[i].port_index, &control_values_r_[i]);
        }

        lilv_instance_connect_port(instance_l_, audio_in_port_, &in_l_);
        lilv_instance_connect_port(instance_l_, audio_out_port_, &out_l_);
        lilv_instance_connect_port(instance_r_, audio_in_port_, &in_r_);
        lilv_instance_connect_port(instance_r_, audio_out_port_, &out_r_);

        lilv_instance_activate(instance_l_);
        lilv_instance_activate(instance_r_);
        valid_ = true;
    }

    ~Lv2RackPlugin() override {
        if (instance_l_) {
            lilv_instance_deactivate(instance_l_);
            lilv_instance_free(instance_l_);
            instance_l_ = nullptr;
        }
        if (instance_r_) {
            lilv_instance_deactivate(instance_r_);
            lilv_instance_free(instance_r_);
            instance_r_ = nullptr;
        }
        if (world_) {
            lilv_world_free(world_);
            world_ = nullptr;
        }
    }

    bool valid() const { return valid_; }

    uint32_t type_id() const override { return plugin_type_id_; }
    const char* display_name() const override { return display_name_.c_str(); }

    float process_sample(float in, float, int channel) override {
        if (!valid_) {
            return in;
        }

        if (channel == 1) {
            in_r_ = in;
            lilv_instance_run(instance_r_, 1);
            return out_r_;
        }

        in_l_ = in;
        lilv_instance_run(instance_l_, 1);
        return out_l_;
    }

    std::vector<PluginParamSpec> param_specs() const override {
        std::vector<PluginParamSpec> out;
        out.reserve(controls_.size());
        for (const ControlParam& p : controls_) {
            out.push_back({
                p.param_id,
                p.name.c_str(),
                vessel::ParamWidget::SLIDER,
                p.min_value,
                p.max_value,
                p.default_value,
            });
        }
        return out;
    }

    float get_param(uint32_t param_id) const override {
        for (const ControlParam& p : controls_) {
            if (p.param_id == param_id) {
                return p.value;
            }
        }
        return 0.0f;
    }

    void set_param(uint32_t param_id, float value) override {
        for (size_t i = 0; i < controls_.size(); ++i) {
            if (controls_[i].param_id == param_id) {
                const float clamped = std::clamp(value, controls_[i].min_value, controls_[i].max_value);
                controls_[i].value = clamped;
                control_values_l_[i] = clamped;
                control_values_r_[i] = clamped;
                return;
            }
        }
    }

    bool has_custom_ui() const override {
        // Vessel currently does not embed/attach LV2 UIs to already running plugin instances.
        // Launching jalv creates a separate plugin instance, which is misleading.
        return false;
    }

private:
    struct ControlParam {
        uint32_t param_id = 0;
        uint32_t port_index = 0;
        std::string name;
        float min_value = 0.0f;
        float max_value = 1.0f;
        float default_value = 0.0f;
        float value = 0.0f;
    };

    uint32_t plugin_type_id_ = 0;
    std::string display_name_;
    std::string uri_;

    bool valid_ = false;
    LilvWorld* world_ = nullptr;
    const LilvPlugin* plugin_ = nullptr;
    LilvInstance* instance_l_ = nullptr;
    LilvInstance* instance_r_ = nullptr;

    uint32_t audio_in_port_ = std::numeric_limits<uint32_t>::max();
    uint32_t audio_out_port_ = std::numeric_limits<uint32_t>::max();

    float in_l_ = 0.0f;
    float out_l_ = 0.0f;
    float in_r_ = 0.0f;
    float out_r_ = 0.0f;

    std::vector<ControlParam> controls_;
    std::vector<float> control_values_l_;
    std::vector<float> control_values_r_;
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

std::vector<DiscoveredLv2Plugin> scan_lv2_plugins() {
    std::vector<DiscoveredLv2Plugin> out;

    LilvWorld* world = lilv_world_new();
    if (!world) {
        return out;
    }

    lilv_world_load_all(world);

    LilvNode* audio_class = lilv_new_uri(world, LV2_CORE__AudioPort);
    LilvNode* input_class = lilv_new_uri(world, LV2_CORE__InputPort);
    LilvNode* output_class = lilv_new_uri(world, LV2_CORE__OutputPort);

    const LilvPlugins* plugins = lilv_world_get_all_plugins(world);
    uint32_t next_type_id = kLv2PluginTypeBase;

    LILV_FOREACH(plugins, it, plugins) {
        const LilvPlugin* plugin = lilv_plugins_get(plugins, it);
        if (!plugin) {
            continue;
        }

        bool has_audio_in = false;
        bool has_audio_out = false;
        const uint32_t num_ports = lilv_plugin_get_num_ports(plugin);
        for (uint32_t i = 0; i < num_ports; ++i) {
            const LilvPort* port = lilv_plugin_get_port_by_index(plugin, i);
            const bool is_audio = lilv_port_is_a(plugin, port, audio_class);
            const bool is_input = lilv_port_is_a(plugin, port, input_class);
            const bool is_output = lilv_port_is_a(plugin, port, output_class);

            if (is_audio && is_input) {
                has_audio_in = true;
            }
            if (is_audio && is_output) {
                has_audio_out = true;
            }
        }

        if (!has_audio_in || !has_audio_out) {
            continue;
        }

        const LilvNode* uri_node = lilv_plugin_get_uri(plugin);
        const LilvNode* name_node = lilv_plugin_get_name(plugin);
        const char* uri = uri_node ? lilv_node_as_string(uri_node) : nullptr;
        const char* name = name_node ? lilv_node_as_string(name_node) : nullptr;
        if (!uri || !name) {
            continue;
        }

        bool has_ui = false;
        const LilvUIs* uis = lilv_plugin_get_uis(plugin);
        if (uis && lilv_uis_size(uis) > 0) {
            has_ui = true;
        }

        out.push_back({next_type_id++, name, uri, has_ui});
    }

    lilv_node_free(audio_class);
    lilv_node_free(input_class);
    lilv_node_free(output_class);
    lilv_world_free(world);

    return out;
}

std::unique_ptr<RackPlugin> create_lv2_plugin(const DiscoveredLv2Plugin& desc) {
    auto plugin = std::make_unique<Lv2RackPlugin>(desc);
    if (!plugin->valid()) {
        return nullptr;
    }
    return plugin;
}
