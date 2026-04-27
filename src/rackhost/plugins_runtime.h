#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "protocol.h"

struct PluginParamEnumOption {
    int value;
    std::string label;
};

struct PluginParamSpec {
    uint32_t id;
    const char* name;
    vessel::ParamWidget widget;
    float min_value;
    float max_value;
    float default_value;
    vessel::ParamValueType value_type{vessel::ParamValueType::FLOAT};
    uint8_t flags{vessel::PARAM_FLAG_NONE};
    std::vector<PluginParamEnumOption> enum_options;
    vessel::ParamLayoutHint layout{vessel::ParamLayoutHint::AUTO};
    float ui_width{0.0f};
};

struct PluginCustomControlSpec {
    uint32_t action_id;
    std::string label;
    std::string text_value;
    vessel::CustomControlTextMode text_mode{vessel::CustomControlTextMode::NONE};
    vessel::ParamLayoutHint layout{vessel::ParamLayoutHint::AUTO};
    float ui_width{0.0f};
};

class RackPlugin {
public:
    virtual ~RackPlugin() = default;

    virtual uint32_t type_id() const = 0;
    virtual const char* display_name() const = 0;
    virtual float process_sample(float in, float sample_rate, int channel) = 0;
    virtual std::vector<PluginParamSpec> param_specs() const = 0;
    virtual float get_param(uint32_t param_id) const = 0;
    virtual void set_param(uint32_t param_id, float value) = 0;

    virtual bool has_custom_ui() const { return false; }
    virtual bool open_custom_ui() { return false; }
    virtual void close_custom_ui() {}
    virtual bool is_custom_ui_open() const { return false; }
    virtual void pump_ui() {}

    virtual std::vector<PluginCustomControlSpec> custom_controls() const { return {}; }
    virtual void trigger_custom_action(uint32_t) {}
    virtual void set_custom_text(uint32_t, const std::string&) {}
    virtual uint64_t ui_schema_version() const { return 0; }

    virtual std::vector<std::pair<std::string, std::string>> save_state() const { return {}; }
    virtual void load_state(const std::vector<std::pair<std::string, std::string>>&) {}
};

struct DiscoveredLv2Plugin {
    uint32_t plugin_type_id = 0;
    std::string name;
    std::string uri;
    bool has_ui = false;
};

std::unique_ptr<RackPlugin> create_plugin(uint32_t plugin_type_id);
bool plugin_is_supported(uint32_t plugin_type_id);
std::vector<DiscoveredLv2Plugin> scan_lv2_plugins();
std::unique_ptr<RackPlugin> create_lv2_plugin(const DiscoveredLv2Plugin& desc);
