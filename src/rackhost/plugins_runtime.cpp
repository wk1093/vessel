#include "plugins_runtime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <lilv/lilv.h>
#include <lv2/core/lv2.h>
#include <lv2/port-props/port-props.h>
#include <lv2/ui/ui.h>
#include <sndfile.h>
#include <suil/suil.h>

#include "plugins.h"

namespace {

constexpr uint32_t kPluginTypeSineMixer = 1;
constexpr uint32_t kPluginTypeGain = 2;
constexpr uint32_t kPluginTypeSoftClip = 3;
constexpr uint32_t kPluginTypeSoundboard = 4;
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

float quantize_by_type(vessel::ParamValueType value_type, float value) {
    switch (value_type) {
        case vessel::ParamValueType::BOOL:
            return value >= 0.5f ? 1.0f : 0.0f;
        case vessel::ParamValueType::INT:
        case vessel::ParamValueType::ENUM:
            return std::round(value);
        case vessel::ParamValueType::FLOAT:
        default:
            return value;
    }
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
            {kParamInputVol, "Input Vol", vessel::ParamWidget::SLIDER, 0.0f, 2.0f, 1.0f, vessel::ParamValueType::FLOAT, vessel::PARAM_FLAG_NONE, {}},
            {kParamSineVol, "Sine Vol", vessel::ParamWidget::SLIDER, 0.0f, 1.0f, 0.0f, vessel::ParamValueType::FLOAT, vessel::PARAM_FLAG_NONE, {}},
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
            {kParamGain, "Gain", vessel::ParamWidget::SLIDER, 0.0f, 2.0f, 1.0f, vessel::ParamValueType::FLOAT, vessel::PARAM_FLAG_NONE, {}},
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
            {kParamDrive, "Drive", vessel::ParamWidget::SLIDER, 0.1f, 8.0f, 1.0f, vessel::ParamValueType::FLOAT, vessel::PARAM_FLAG_NONE, {}},
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

class SoundboardPlugin final : public RackPlugin {
public:
    uint32_t type_id() const override { return kPluginTypeSoundboard; }
    const char* display_name() const override { return "Soundboard"; }

    SoundboardPlugin() {
        add_pad("Pad 1");
    }

    float process_sample(float in, float sample_rate, int channel) override {
        if (channel == 1) {
            return last_frame_out_;
        }

        const float sr = std::max(sample_rate, 1.0f);
        for (size_t pad = 0; pad < pads_.size(); ++pad) {
            if (pads_[pad]->trigger.exchange(false, std::memory_order_relaxed)) {
                voices_[pad].active = true;
                voices_[pad].position = 0.0f;
                voices_[pad].step = pads_[pad]->sample_rate > 0
                    ? static_cast<float>(pads_[pad]->sample_rate) / sr
                    : 1.0f;
            }
        }

        float board = 0.0f;
        for (size_t pad = 0; pad < pads_.size(); ++pad) {
            const float gain = pads_[pad]->gain.load(std::memory_order_relaxed);
            board += render_voice(voices_[pad], *pads_[pad]) * gain;
        }

        const float mixed = in + board * master_mix_.load(std::memory_order_relaxed);
        last_frame_out_ = mixed;
        return mixed;
    }

    std::vector<PluginParamSpec> param_specs() const override {
        std::vector<PluginParamSpec> out;
        const bool customize = customize_mode_.load(std::memory_order_relaxed);

        out.push_back({
            kParamCustomizeMode,
            "Customize",
            vessel::ParamWidget::TOGGLE,
            0.0f,
            1.0f,
            1.0f,
            vessel::ParamValueType::BOOL,
            vessel::PARAM_FLAG_NONE,
            {},
            vessel::ParamLayoutHint::AUTO,
            140.0f,
        });

        if (customize) {
            out.push_back({
                kParamMasterMix,
                "Master Mix",
                vessel::ParamWidget::KNOB,
                0.0f,
                1.0f,
                0.8f,
                vessel::ParamValueType::FLOAT,
                vessel::PARAM_FLAG_NONE,
                {},
                vessel::ParamLayoutHint::SAME_LINE,
                0.0f,
            });

            out.push_back({
                kParamAddPad,
                "+ Add Pad",
                vessel::ParamWidget::BUTTON,
                0.0f,
                1.0f,
                0.0f,
                vessel::ParamValueType::BOOL,
                vessel::PARAM_FLAG_NONE,
                {},
                vessel::ParamLayoutHint::SAME_LINE,
                0.0f,
            });
        }

        const size_t pad_count = pads_.size();
        const size_t columns = std::max<size_t>(1, static_cast<size_t>(std::ceil(std::sqrt(static_cast<double>(std::max<size_t>(1, pad_count))))));
        for (size_t pad = 0; pad < pad_count; ++pad) {
            const vessel::ParamLayoutHint trigger_layout = (!customize && (pad % columns) != 0)
                ? vessel::ParamLayoutHint::SAME_LINE
                : vessel::ParamLayoutHint::AUTO;

            out.push_back({
                pad_trigger_param_id(static_cast<uint32_t>(pad)),
                pads_[pad]->name.c_str(),
                vessel::ParamWidget::BUTTON,
                0.0f,
                1.0f,
                0.0f,
                vessel::ParamValueType::BOOL,
                vessel::PARAM_FLAG_NONE,
                {},
                trigger_layout,
                !customize ? 140.0f : 0.0f,
            });

            if (customize) {
                out.push_back({
                    pad_gain_param_id(static_cast<uint32_t>(pad)),
                    "Gain",
                    vessel::ParamWidget::KNOB,
                    0.0f,
                    1.0f,
                    0.75f,
                    vessel::ParamValueType::FLOAT,
                    vessel::PARAM_FLAG_NONE,
                    {},
                    vessel::ParamLayoutHint::SAME_LINE,
                    0.0f,
                });
            }
        }

        return out;
    }

    float get_param(uint32_t param_id) const override {
        if (param_id == kParamCustomizeMode) {
            return customize_mode_.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
        }
        if (param_id == kParamMasterMix) {
            return master_mix_.load(std::memory_order_relaxed);
        }
        if (param_id == kParamAddPad) {
            return 0.0f;
        }

        uint32_t pad = 0;
        if (param_id_to_pad(param_id, pad) && pad < pads_.size()) {
            if (param_id == pad_trigger_param_id(pad)) {
                return 0.0f;
            }
            if (param_id == pad_gain_param_id(pad)) {
                return pads_[pad]->gain.load(std::memory_order_relaxed);
            }
        }

        return 0.0f;
    }

    void set_param(uint32_t param_id, float value) override {
        if (param_id == kParamCustomizeMode) {
            const bool customize = value > 0.5f;
            const bool old = customize_mode_.load(std::memory_order_relaxed);
            if (customize != old) {
                customize_mode_.store(customize, std::memory_order_relaxed);
                ++ui_schema_version_;
            }
            return;
        }

        if (param_id == kParamMasterMix) {
            master_mix_.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
            return;
        }

        if (param_id == kParamAddPad) {
            if (value > 0.5f) {
                add_pad("Pad " + std::to_string(pads_.size() + 1));
            }
            return;
        }

        uint32_t pad = 0;
        if (!param_id_to_pad(param_id, pad) || pad >= pads_.size()) {
            return;
        }

        if (param_id == pad_trigger_param_id(pad)) {
            if (value > 0.5f) {
                pads_[pad]->trigger.store(true, std::memory_order_relaxed);
            }
            return;
        }
        if (param_id == pad_gain_param_id(pad)) {
            pads_[pad]->gain.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
            return;
        }
    }

    std::vector<PluginCustomControlSpec> custom_controls() const override {
        std::vector<PluginCustomControlSpec> out;
        if (!customize_mode_.load(std::memory_order_relaxed)) {
            return out;
        }

        out.reserve(pads_.size() * 3);
        for (size_t i = 0; i < pads_.size(); ++i) {
            out.push_back({
                load_action_id(i),
                std::string("Load Sound: ") + pads_[i]->name,
                std::string(),
                vessel::CustomControlTextMode::FILE_PATH,
                vessel::ParamLayoutHint::AUTO,
                0.0f,
            });
            out.push_back({
                rename_action_id(i),
                std::string("Rename: ") + pads_[i]->name,
                pads_[i]->name,
                vessel::CustomControlTextMode::PLAIN_TEXT,
                vessel::ParamLayoutHint::SAME_LINE,
                200.0f,
            });
            if (pads_.size() > 1) {
                out.push_back({
                    remove_action_id(i),
                    std::string("Remove Pad: ") + pads_[i]->name,
                    std::string(),
                    vessel::CustomControlTextMode::NONE,
                    vessel::ParamLayoutHint::SAME_LINE,
                    0.0f,
                });
            }
        }
        return out;
    }

    void trigger_custom_action(uint32_t action_id) override {
        if (action_id >= kActionRemoveBase && action_id < (kActionRemoveBase + 1000)) {
            const size_t index = static_cast<size_t>(action_id - kActionRemoveBase);
            remove_pad(index);
        }
    }

    void set_custom_text(uint32_t action_id, const std::string& text) override {
        if (action_id >= kActionRenameBase && action_id < (kActionRenameBase + 1000)) {
            const size_t index = static_cast<size_t>(action_id - kActionRenameBase);
            rename_pad(index, text);
            return;
        }
        if (action_id >= kActionLoadBase && action_id < kActionRenameBase) {
            const size_t index = static_cast<size_t>(action_id - kActionLoadBase);
            load_sound_into_pad(index, text, false);
        }
    }

    uint64_t ui_schema_version() const override {
        return ui_schema_version_;
    }

    std::vector<std::pair<std::string, std::string>> save_state() const override {
        std::vector<std::pair<std::string, std::string>> out;
        out.emplace_back("pad_count", std::to_string(pads_.size()));
        out.emplace_back("customize", customize_mode_.load(std::memory_order_relaxed) ? "1" : "0");
        out.emplace_back("master_mix", std::to_string(master_mix_.load(std::memory_order_relaxed)));
        for (size_t i = 0; i < pads_.size(); ++i) {
            out.emplace_back("pad_name_" + std::to_string(i), pads_[i]->name);
            out.emplace_back("pad_gain_" + std::to_string(i), std::to_string(pads_[i]->gain.load(std::memory_order_relaxed)));
            if (!pads_[i]->path.empty()) {
                out.emplace_back("pad_path_" + std::to_string(i), pads_[i]->path);
            }
        }
        return out;
    }

    void load_state(const std::vector<std::pair<std::string, std::string>>& state) override {
        if (state.empty()) {
            return;
        }

        size_t target_count = 1;
        bool customize = true;
        float master = 0.8f;
        std::vector<std::pair<size_t, std::string>> names;
        std::vector<std::pair<size_t, std::string>> paths;
        std::vector<std::pair<size_t, float>> gains;

        for (const auto& kv : state) {
            if (kv.first == "pad_count") {
                try {
                    target_count = static_cast<size_t>(std::stoul(kv.second));
                } catch (...) {
                    target_count = 1;
                }
                target_count = std::clamp<size_t>(target_count, 1, 64);
                continue;
            }

            if (kv.first == "customize") {
                customize = kv.second == "1";
                continue;
            }
            if (kv.first == "master_mix") {
                try {
                    master = std::stof(kv.second);
                } catch (...) {
                    master = 0.8f;
                }
                continue;
            }

            auto parse_index = [](const std::string& key, const char* prefix, size_t& out_index) {
                if (key.rfind(prefix, 0) != 0) {
                    return false;
                }
                const std::string index_str = key.substr(std::char_traits<char>::length(prefix));
                try {
                    out_index = static_cast<size_t>(std::stoul(index_str));
                    return true;
                } catch (...) {
                    return false;
                }
            };

            size_t idx = 0;
            if (parse_index(kv.first, "pad_name_", idx)) {
                names.emplace_back(idx, kv.second);
                continue;
            }
            if (parse_index(kv.first, "pad_path_", idx)) {
                paths.emplace_back(idx, kv.second);
                continue;
            }
            if (parse_index(kv.first, "pad_gain_", idx)) {
                try {
                    gains.emplace_back(idx, std::stof(kv.second));
                } catch (...) {
                }
                continue;
            }
        }

        pads_.clear();
        voices_.clear();
        for (size_t i = 0; i < target_count; ++i) {
            add_pad("Pad " + std::to_string(i + 1));
        }

        for (const auto& entry : names) {
            if (entry.first < pads_.size() && !entry.second.empty()) {
                pads_[entry.first]->name = entry.second;
            }
        }

        for (const auto& entry : gains) {
            if (entry.first < pads_.size()) {
                pads_[entry.first]->gain.store(std::clamp(entry.second, 0.0f, 1.0f), std::memory_order_relaxed);
            }
        }

        for (const auto& entry : paths) {
            if (entry.first < pads_.size()) {
                load_sound_into_pad(entry.first, entry.second, true);
            }
        }

        customize_mode_.store(customize, std::memory_order_relaxed);
        master_mix_.store(std::clamp(master, 0.0f, 1.0f), std::memory_order_relaxed);
        ++ui_schema_version_;
    }

private:
    struct PadRuntime {
        std::string name;
        std::string path;
        std::vector<float> samples;
        uint32_t sample_rate = 48000;
        std::atomic<float> gain{0.75f};
        std::atomic<bool> trigger{false};
    };

    struct Voice {
        bool active = false;
        float position = 0.0f;
        float step = 1.0f;
    };

    static constexpr uint32_t kParamMasterMix = 1;
    static constexpr uint32_t kParamCustomizeMode = 2;
    static constexpr uint32_t kParamAddPad = 3;
    static constexpr uint32_t kParamBase = 100;
    static constexpr uint32_t kParamsPerPad = 2;
    static constexpr uint32_t kActionLoadBase = 1000;
    static constexpr uint32_t kActionRenameBase = 1500;
    static constexpr uint32_t kActionRemoveBase = 2000;

    static uint32_t load_action_id(size_t index) {
        return kActionLoadBase + static_cast<uint32_t>(index);
    }

    static uint32_t rename_action_id(size_t index) {
        return kActionRenameBase + static_cast<uint32_t>(index);
    }

    static uint32_t remove_action_id(size_t index) {
        return kActionRemoveBase + static_cast<uint32_t>(index);
    }

    static uint32_t pad_trigger_param_id(uint32_t pad) {
        return kParamBase + (pad * kParamsPerPad);
    }

    static uint32_t pad_gain_param_id(uint32_t pad) {
        return pad_trigger_param_id(pad) + 1;
    }

    static bool param_id_to_pad(uint32_t param_id, uint32_t& pad_out) {
        if (param_id < kParamBase) {
            return false;
        }
        const uint32_t rel = param_id - kParamBase;
        pad_out = rel / kParamsPerPad;
        return static_cast<size_t>(pad_out) < 64;
    }

    bool decode_audio_file(const std::string& path, std::vector<float>& out_samples, uint32_t& out_sample_rate) {
        SF_INFO info{};
        SNDFILE* file = sf_open(path.c_str(), SFM_READ, &info);
        if (!file) {
            return false;
        }

        const sf_count_t max_frames = static_cast<sf_count_t>(info.samplerate) * 120;
        const sf_count_t frames_to_read = std::max<sf_count_t>(0, std::min(info.frames, max_frames));
        if (frames_to_read <= 0 || info.channels <= 0) {
            sf_close(file);
            return false;
        }

        std::vector<float> interleaved(static_cast<size_t>(frames_to_read) * static_cast<size_t>(info.channels));
        const sf_count_t read_frames = sf_readf_float(file, interleaved.data(), frames_to_read);
        sf_close(file);
        if (read_frames <= 0) {
            return false;
        }

        out_samples.resize(static_cast<size_t>(read_frames));
        for (sf_count_t i = 0; i < read_frames; ++i) {
            float sum = 0.0f;
            for (int ch = 0; ch < info.channels; ++ch) {
                sum += interleaved[static_cast<size_t>(i) * static_cast<size_t>(info.channels) + static_cast<size_t>(ch)];
            }
            out_samples[static_cast<size_t>(i)] = sum / static_cast<float>(info.channels);
        }

        out_sample_rate = static_cast<uint32_t>(std::max(info.samplerate, 1));

        return true;
    }

    void add_pad(const std::string& name) {
        auto pad = std::make_shared<PadRuntime>();
        pad->name = name;
        pads_.push_back(std::move(pad));
        voices_.push_back({});
        ++ui_schema_version_;
    }

    void remove_pad(size_t index) {
        if (pads_.size() <= 1 || index >= pads_.size()) {
            return;
        }

        pads_.erase(pads_.begin() + static_cast<long>(index));
        voices_.erase(voices_.begin() + static_cast<long>(index));

        ++ui_schema_version_;
    }

    void rename_pad(size_t index, const std::string& name) {
        if (index >= pads_.size()) {
            return;
        }

        if (name.empty()) {
            return;
        }

        pads_[index]->name = name;
        ++ui_schema_version_;
    }

    void load_sound_into_pad(size_t index, const std::string& path, bool keep_name) {
        if (index >= pads_.size() || path.empty()) {
            return;
        }

        std::vector<float> decoded_samples;
        uint32_t decoded_rate = 48000;
        if (!decode_audio_file(path, decoded_samples, decoded_rate)) {
            return;
        }

        pads_[index]->samples = std::move(decoded_samples);
        pads_[index]->sample_rate = decoded_rate;
        pads_[index]->path = path;
        if (!keep_name && pads_[index]->name.empty()) {
            pads_[index]->name = "Pad " + std::to_string(index + 1);
            ++ui_schema_version_;
        }
    }

    float render_voice(Voice& v, const PadRuntime& pad) {
        if (!v.active) {
            return 0.0f;
        }

        if (pad.samples.empty()) {
            v.active = false;
            return 0.0f;
        }

        const size_t i0 = static_cast<size_t>(v.position);
        if (i0 >= pad.samples.size()) {
            v.active = false;
            return 0.0f;
        }

        const size_t i1 = std::min(i0 + 1, pad.samples.size() - 1);
        const float frac = v.position - static_cast<float>(i0);
        const float s0 = pad.samples[i0];
        const float s1 = pad.samples[i1];
        const float sample = s0 + (s1 - s0) * frac;

        v.position += std::max(0.0001f, v.step);
        if (v.position >= static_cast<float>(pad.samples.size())) {
            v.active = false;
        }

        return sample;
    }

    std::atomic<float> master_mix_{0.8f};
    std::atomic<bool> customize_mode_{true};
    std::vector<std::shared_ptr<PadRuntime>> pads_;
    std::vector<Voice> voices_;
    uint64_t ui_schema_version_ = 1;
    float last_frame_out_ = 0.0f;
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

        choose_supported_ui();

        LilvNode* audio_class = lilv_new_uri(world_, LV2_CORE__AudioPort);
        LilvNode* control_class = lilv_new_uri(world_, LV2_CORE__ControlPort);
        LilvNode* input_class = lilv_new_uri(world_, LV2_CORE__InputPort);
        LilvNode* output_class = lilv_new_uri(world_, LV2_CORE__OutputPort);
        LilvNode* port_property_pred = lilv_new_uri(world_, LV2_CORE__portProperty);
        LilvNode* toggled_prop = lilv_new_uri(world_, LV2_CORE__toggled);
        LilvNode* integer_prop = lilv_new_uri(world_, LV2_CORE__integer);
        LilvNode* enumeration_prop = lilv_new_uri(world_, LV2_CORE__enumeration);
        LilvNode* logarithmic_prop = lilv_new_uri(world_, LV2_PORT_PROPS__logarithmic);

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

                bool is_toggled = false;
                bool is_integer = false;
                bool is_enumeration = false;
                bool is_logarithmic = false;
                if (port_property_pred) {
                    LilvNodes* properties = lilv_port_get_value(plugin_, port, port_property_pred);
                    if (properties) {
                        LILV_FOREACH(nodes, pit, properties) {
                            const LilvNode* prop = lilv_nodes_get(properties, pit);
                            if (toggled_prop && lilv_node_equals(prop, toggled_prop)) {
                                is_toggled = true;
                            } else if (integer_prop && lilv_node_equals(prop, integer_prop)) {
                                is_integer = true;
                            } else if (enumeration_prop && lilv_node_equals(prop, enumeration_prop)) {
                                is_enumeration = true;
                            } else if (logarithmic_prop && lilv_node_equals(prop, logarithmic_prop)) {
                                is_logarithmic = true;
                            }
                        }
                        lilv_nodes_free(properties);
                    }
                }

                ControlParam p;
                p.param_id = next_param_id++;
                p.port_index = i;
                p.name = raw_name ? raw_name : "Control";
                p.widget = vessel::ParamWidget::SLIDER;
                p.value_type = vessel::ParamValueType::FLOAT;
                p.flags = vessel::PARAM_FLAG_NONE;

                if (is_toggled) {
                    p.widget = vessel::ParamWidget::TOGGLE;
                    p.value_type = vessel::ParamValueType::BOOL;
                } else if (is_enumeration) {
                    p.value_type = vessel::ParamValueType::ENUM;
                } else if (is_integer) {
                    p.value_type = vessel::ParamValueType::INT;
                }

                if (is_logarithmic && p.value_type == vessel::ParamValueType::FLOAT) {
                    p.flags = static_cast<uint8_t>(p.flags | vessel::PARAM_FLAG_LOGARITHMIC);
                }

                p.min_value = sanitize_min(mins[i]);
                p.max_value = sanitize_max(maxs[i], p.min_value);
                p.default_value = std::clamp(sanitize_default(defs[i], p.min_value), p.min_value, p.max_value);

                if (p.value_type == vessel::ParamValueType::BOOL) {
                    p.min_value = 0.0f;
                    p.max_value = 1.0f;
                }

                p.default_value = std::clamp(
                    quantize_by_type(p.value_type, p.default_value),
                    p.min_value,
                    p.max_value);
                p.value = p.default_value;

                if (p.value_type == vessel::ParamValueType::ENUM) {
                    LilvScalePoints* scale_points = lilv_port_get_scale_points(plugin_, port);
                    if (scale_points) {
                        LILV_FOREACH(scale_points, sit, scale_points) {
                            const LilvScalePoint* point = lilv_scale_points_get(scale_points, sit);
                            if (!point) {
                                continue;
                            }

                            const LilvNode* value_node = lilv_scale_point_get_value(point);
                            const LilvNode* label_node = lilv_scale_point_get_label(point);
                            if (!value_node || !label_node) {
                                continue;
                            }

                            const int enum_value = static_cast<int>(std::lround(lilv_node_as_float(value_node)));
                            const char* raw_label = lilv_node_as_string(label_node);
                            const std::string label = raw_label ? raw_label : std::to_string(enum_value);

                            bool duplicate = false;
                            for (const auto& existing : p.enum_options) {
                                if (existing.value == enum_value) {
                                    duplicate = true;
                                    break;
                                }
                            }
                            if (!duplicate) {
                                p.enum_options.push_back({enum_value, label});
                            }
                        }
                        lilv_scale_points_free(scale_points);
                    }

                    std::sort(
                        p.enum_options.begin(),
                        p.enum_options.end(),
                        [](const PluginParamEnumOption& a, const PluginParamEnumOption& b) {
                            return a.value < b.value;
                        });
                }

                controls_.push_back(std::move(p));
            }
        }

        lilv_node_free(audio_class);
        lilv_node_free(control_class);
        lilv_node_free(input_class);
        lilv_node_free(output_class);
        lilv_node_free(port_property_pred);
        lilv_node_free(toggled_prop);
        lilv_node_free(integer_prop);
        lilv_node_free(enumeration_prop);
        lilv_node_free(logarithmic_prop);

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
        close_custom_ui();

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
                p.widget,
                p.min_value,
                p.max_value,
                p.default_value,
                p.value_type,
                p.flags,
                p.enum_options,
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
                const float clamped = std::clamp(
                    quantize_by_type(controls_[i].value_type, value),
                    controls_[i].min_value,
                    controls_[i].max_value);
                controls_[i].value = clamped;
                control_values_l_[i] = clamped;
                control_values_r_[i] = clamped;
                if (ui_instance_) {
                    suil_instance_port_event(ui_instance_, controls_[i].port_index, sizeof(float), 0, &clamped);
                }
                return;
            }
        }
    }

    bool has_custom_ui() const override {
        return !ui_uri_.empty();
    }

    bool open_custom_ui() override {
        if (!has_custom_ui()) {
            return false;
        }

        if (!ui_instance_) {
            ui_host_ = suil_host_new(&Lv2RackPlugin::ui_write_cb, nullptr, nullptr, nullptr);
            if (!ui_host_) {
                return false;
            }

            const char* plugin_uri = lilv_node_as_uri(lilv_plugin_get_uri(plugin_));
            ui_instance_ = suil_instance_new(
                ui_host_,
                this,
                nullptr,
                plugin_uri,
                ui_uri_.c_str(),
                ui_type_uri_.c_str(),
                ui_bundle_path_.c_str(),
                ui_binary_path_.c_str(),
                nullptr);

            if (!ui_instance_) {
                suil_host_free(ui_host_);
                ui_host_ = nullptr;
                return false;
            }

            ui_idle_iface_ = static_cast<const LV2UI_Idle_Interface*>(
                suil_instance_extension_data(ui_instance_, LV2_UI__idleInterface));
            ui_show_iface_ = static_cast<const LV2UI_Show_Interface*>(
                suil_instance_extension_data(ui_instance_, LV2_UI__showInterface));

            for (const auto& p : controls_) {
                suil_instance_port_event(ui_instance_, p.port_index, sizeof(float), 0, &p.value);
            }
        }

        if (ui_show_iface_ && ui_show_iface_->show) {
            const bool opened = ui_show_iface_->show(suil_instance_get_handle(ui_instance_)) == 0;
            ui_open_ = opened;
            return opened;
        }

        ui_open_ = ui_instance_ != nullptr;
        return ui_open_;
    }

    void close_custom_ui() override {
        close_custom_ui_impl();
    }

    bool is_custom_ui_open() const override {
        return ui_open_;
    }

    void pump_ui() override {
        if (!ui_instance_ || !ui_idle_iface_ || !ui_idle_iface_->idle) {
            return;
        }
        if (ui_idle_iface_->idle(suil_instance_get_handle(ui_instance_)) != 0) {
            close_custom_ui_impl();
        }
    }

private:
    static void ui_write_cb(SuilController controller, uint32_t port_index, uint32_t buffer_size, uint32_t protocol, const void* buffer) {
        if (!controller || protocol != 0 || buffer_size != sizeof(float) || !buffer) {
            return;
        }
        static_cast<Lv2RackPlugin*>(controller)->on_ui_write(port_index, *static_cast<const float*>(buffer));
    }

    void on_ui_write(uint32_t port_index, float value) {
        for (size_t i = 0; i < controls_.size(); ++i) {
            if (controls_[i].port_index == port_index) {
                set_param(controls_[i].param_id, value);
                return;
            }
        }
    }

    void close_custom_ui_impl() {
        if (ui_instance_) {
            if (ui_show_iface_ && ui_show_iface_->hide) {
                ui_show_iface_->hide(suil_instance_get_handle(ui_instance_));
            }
            suil_instance_free(ui_instance_);
            ui_instance_ = nullptr;
        }
        ui_open_ = false;
        ui_idle_iface_ = nullptr;
        ui_show_iface_ = nullptr;
        if (ui_host_) {
            suil_host_free(ui_host_);
            ui_host_ = nullptr;
        }
    }

    void choose_supported_ui() {
        LilvUIs* uis = lilv_plugin_get_uis(plugin_);
        if (!uis) {
            return;
        }

        auto choose_for_container = [&](const char* container_uri) {
            LilvNode* container = lilv_new_uri(world_, container_uri);
            if (!container) {
                return false;
            }

            unsigned best_quality = 0;
            const LilvUI* best_ui = nullptr;
            const LilvNode* best_ui_type = nullptr;

            LILV_FOREACH(uis, it, uis) {
                const LilvUI* ui = lilv_uis_get(uis, it);
                const LilvNode* ui_type = nullptr;
                const unsigned quality = lilv_ui_is_supported(ui, suil_ui_supported, container, &ui_type);
                if (quality > 0 && (best_quality == 0 || quality < best_quality)) {
                    best_quality = quality;
                    best_ui = ui;
                    best_ui_type = ui_type;
                }
            }

            if (best_ui && best_ui_type) {
                const LilvNode* ui_uri_node = lilv_ui_get_uri(best_ui);
                const LilvNode* bundle_uri_node = lilv_ui_get_bundle_uri(best_ui);
                const LilvNode* binary_uri_node = lilv_ui_get_binary_uri(best_ui);
                if (ui_uri_node && bundle_uri_node && binary_uri_node) {
                    ui_uri_ = lilv_node_as_uri(ui_uri_node);
                    ui_type_uri_ = lilv_node_as_uri(best_ui_type);
                    char* bundle_path = lilv_node_get_path(bundle_uri_node, nullptr);
                    char* binary_path = lilv_node_get_path(binary_uri_node, nullptr);
                    if (bundle_path && binary_path) {
                        ui_bundle_path_ = bundle_path;
                        ui_binary_path_ = binary_path;
                    }
                    if (bundle_path) {
                        lilv_free(bundle_path);
                    }
                    if (binary_path) {
                        lilv_free(binary_path);
                    }
                }
            }

            lilv_node_free(container);
            return !ui_uri_.empty() && !ui_type_uri_.empty() && !ui_bundle_path_.empty() && !ui_binary_path_.empty();
        };

        if (!choose_for_container(LV2_UI__Qt5UI)) {
            choose_for_container(LV2_UI__X11UI);
        }

        lilv_uis_free(uis);
    }

    struct ControlParam {
        uint32_t param_id = 0;
        uint32_t port_index = 0;
        std::string name;
        vessel::ParamWidget widget = vessel::ParamWidget::SLIDER;
        vessel::ParamValueType value_type = vessel::ParamValueType::FLOAT;
        uint8_t flags = vessel::PARAM_FLAG_NONE;
        std::vector<PluginParamEnumOption> enum_options;
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

    std::string ui_uri_;
    std::string ui_type_uri_;
    std::string ui_bundle_path_;
    std::string ui_binary_path_;
    SuilHost* ui_host_ = nullptr;
    SuilInstance* ui_instance_ = nullptr;
    const LV2UI_Idle_Interface* ui_idle_iface_ = nullptr;
    const LV2UI_Show_Interface* ui_show_iface_ = nullptr;
    bool ui_open_ = false;

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
        case kPluginTypeSoundboard:
            return std::make_unique<SoundboardPlugin>();
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
