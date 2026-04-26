#pragma once

#include <cstdint>
#include <memory>
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
    virtual float process_sample(float in, float sample_rate) = 0;
    virtual std::vector<PluginParamSpec> param_specs() const = 0;
    virtual float get_param(uint32_t param_id) const = 0;
    virtual void set_param(uint32_t param_id, float value) = 0;
};

std::unique_ptr<RackPlugin> create_plugin(uint32_t plugin_type_id);
bool plugin_is_supported(uint32_t plugin_type_id);
