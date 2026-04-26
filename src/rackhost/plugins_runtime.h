#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "protocol.h"

struct PluginParamSpec {
    uint32_t id;
    const char* name;
    vessel::ParamWidget widget;
    float min_value;
    float max_value;
    float default_value;
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
};

struct DiscoveredLv2Plugin {
    uint32_t plugin_type_id = 0;
    std::string name;
    std::string uri;
};

std::unique_ptr<RackPlugin> create_plugin(uint32_t plugin_type_id);
bool plugin_is_supported(uint32_t plugin_type_id);
std::vector<DiscoveredLv2Plugin> scan_lv2_plugins();
std::unique_ptr<RackPlugin> create_lv2_plugin(const DiscoveredLv2Plugin& desc);
