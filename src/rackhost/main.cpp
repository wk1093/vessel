#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <suil/suil.h>

#include "plugins.h"
#include "plugins_runtime.h"
#include "protocol.h"

namespace {
std::atomic<bool> g_running{true};

struct AudioState {
    std::atomic<float> volume{1.0f};
    std::atomic<bool> bypassed{false};
    std::atomic<float> in_peak{0.0f};
    std::atomic<float> out_peak{0.0f};
};


struct PluginInstance {
    uint32_t instance_id = 0;
    bool bypassed = false;
    std::unique_ptr<RackPlugin> plugin;
    bool custom_ui_open = false;
    std::vector<PluginParamSpec> param_specs;
    std::vector<float> last_param_values;
    uint64_t last_ui_schema_version = 0;
};

struct SavedPluginState {
    std::string plugin_id;
    bool bypassed = false;
    std::vector<std::pair<uint32_t, float>> params;
    std::vector<std::pair<std::string, std::string>> state;
};

struct SavedRackState {
    float master_gain = 1.0f;
    bool bypassed = false;
    std::vector<SavedPluginState> plugins;
};

struct RunnerRack {
    struct pw_filter* filter = nullptr;
    void* input_ports[2] = {nullptr, nullptr};
    void* output_ports[2] = {nullptr, nullptr};

    AudioState audio_state;
    std::vector<PluginInstance> plugins;
    std::vector<DiscoveredLv2Plugin> lv2_plugins;
    uint32_t next_plugin_instance_id = 1;

    std::vector<uint8_t> rx_buffer;

    ~RunnerRack() {
        if (filter != nullptr) {
            pw_filter_destroy(filter);
            filter = nullptr;
        }
    }
};

void signal_handler(int) {
    g_running.store(false);
}

PluginInstance* find_plugin_instance(RunnerRack& rack, uint32_t instance_id) {
    for (auto& instance : rack.plugins) {
        if (instance.instance_id == instance_id) {
            return &instance;
        }
    }
    return nullptr;
}

bool move_plugin_instance(std::vector<PluginInstance>& plugins, uint32_t instance_id, uint32_t target_index) {
    if (plugins.empty()) {
        return false;
    }

    size_t from = plugins.size();
    for (size_t i = 0; i < plugins.size(); ++i) {
        if (plugins[i].instance_id == instance_id) {
            from = i;
            break;
        }
    }
    if (from == plugins.size()) {
        return false;
    }

    size_t to = std::min<size_t>(target_index, plugins.size() - 1);
    if (from == to) {
        return false;
    }

    PluginInstance moved = std::move(plugins[from]);
    plugins.erase(plugins.begin() + static_cast<long>(from));
    if (to > from) {
        --to;
    }
    plugins.insert(plugins.begin() + static_cast<long>(to), std::move(moved));
    return true;
}

const DiscoveredLv2Plugin* find_lv2_plugin(const RunnerRack& rack, uint32_t plugin_type_id) {
    for (const auto& entry : rack.lv2_plugins) {
        if (entry.plugin_type_id == plugin_type_id) {
            return &entry;
        }
    }
    return nullptr;
}

const DiscoveredLv2Plugin* find_lv2_plugin_by_uri(const RunnerRack& rack, const std::string& uri) {
    for (const auto& entry : rack.lv2_plugins) {
        if (entry.uri == uri) {
            return &entry;
        }
    }
    return nullptr;
}

std::string plugin_identity_for_instance(const RunnerRack& rack, const PluginInstance& instance) {
    if (!instance.plugin) {
        return "";
    }

    const uint32_t type_id = instance.plugin->type_id();
    const vessel::PluginManifestEntry* manifest = vessel::find_plugin_manifest_entry(type_id);
    if (manifest && manifest->is_builtin) {
        return "builtin:" + std::to_string(type_id);
    }

    const DiscoveredLv2Plugin* lv2 = find_lv2_plugin(rack, type_id);
    if (lv2 && !lv2->uri.empty()) {
        return "lv2:" + lv2->uri;
    }

    return "builtin:" + std::to_string(type_id);
}

bool resolve_plugin_type_id(const RunnerRack& rack, const std::string& plugin_id, uint32_t& out_type_id) {
    if (plugin_id.empty()) {
        return false;
    }

    const std::string_view id(plugin_id);
    if (id.rfind("builtin:", 0) == 0) {
        const std::string value(plugin_id.substr(8));
        try {
            out_type_id = static_cast<uint32_t>(std::stoul(value));
            return true;
        } catch (...) {
            return false;
        }
    }

    if (id.rfind("lv2:", 0) == 0) {
        const std::string uri(plugin_id.substr(4));
        const DiscoveredLv2Plugin* lv2 = find_lv2_plugin_by_uri(rack, uri);
        if (!lv2) {
            return false;
        }
        out_type_id = lv2->plugin_type_id;
        return true;
    }

    bool all_digits = true;
    for (const char c : plugin_id) {
        if (c < '0' || c > '9') {
            all_digits = false;
            break;
        }
    }
    if (all_digits) {
        try {
            out_type_id = static_cast<uint32_t>(std::stoul(plugin_id));
            return true;
        } catch (...) {
            return false;
        }
    }

    const DiscoveredLv2Plugin* lv2 = find_lv2_plugin_by_uri(rack, plugin_id);
    if (!lv2) {
        return false;
    }
    out_type_id = lv2->plugin_type_id;
    return true;
}

template <typename T>
void send_ipc(int client_fd, const T& msg) {
    if (client_fd < 0) return;
    const ssize_t n = ::send(client_fd, &msg, sizeof(T), MSG_NOSIGNAL);
    if (n <= 0) {
        // Client disconnect is handled by the select loop; ignore send errors here.
    }
}

void send_plugin_ui_state(int client_fd, uint32_t instance_id, bool is_open) {
    vessel::MsgPluginUiState msg;
    msg.instance_id = instance_id;
    msg.is_open = is_open ? 1 : 0;
    send_ipc(client_fd, msg);
}

void send_plugin_params(int client_fd, uint32_t instance_id, RackPlugin& plugin);

void send_plugin_custom_controls(int client_fd, uint32_t instance_id, RackPlugin& plugin);

void send_plugin_params_reset(int client_fd, uint32_t instance_id) {
    vessel::MsgPluginParamsReset msg;
    msg.instance_id = instance_id;
    send_ipc(client_fd, msg);
}

void send_rack_state_reset(int client_fd) {
    vessel::MsgRackStateReset msg;
    send_ipc(client_fd, msg);
}

void send_plugin_param_desc_value(int client_fd, uint32_t instance_id, const PluginParamSpec& spec, float value, uint8_t is_last) {
    vessel::MsgPluginParamDesc msg;
    msg.instance_id = instance_id;
    msg.param_id = spec.id;
    msg.widget = spec.widget;
    msg.value_type = spec.value_type;
    msg.flags = spec.flags;
    msg.layout = spec.layout;
    msg.ui_width = spec.ui_width;
    msg.min_value = spec.min_value;
    msg.max_value = spec.max_value;
    msg.value = value;
    std::strncpy(msg.name, spec.name, vessel::kMaxParamNameLen);
    msg.name[vessel::kMaxParamNameLen] = '\0';
    msg.is_last = is_last;
    send_ipc(client_fd, msg);
}

void send_plugin_param_enum_options(int client_fd, uint32_t instance_id, const PluginParamSpec& spec) {
    if (spec.value_type != vessel::ParamValueType::ENUM || spec.enum_options.empty()) {
        return;
    }

    for (size_t i = 0; i < spec.enum_options.size(); ++i) {
        vessel::MsgPluginParamEnumOption msg;
        msg.instance_id = instance_id;
        msg.param_id = spec.id;
        msg.enum_value = spec.enum_options[i].value;
        std::strncpy(msg.label, spec.enum_options[i].label.c_str(), vessel::kMaxParamNameLen);
        msg.label[vessel::kMaxParamNameLen] = '\0';
        msg.is_last = (i + 1 == spec.enum_options.size()) ? 1 : 0;
        send_ipc(client_fd, msg);
    }
}

void refresh_plugin_param_cache(PluginInstance& instance) {
    instance.param_specs = instance.plugin->param_specs();
    instance.last_ui_schema_version = instance.plugin->ui_schema_version();
    instance.last_param_values.clear();
    instance.last_param_values.reserve(instance.param_specs.size());
    for (const auto& spec : instance.param_specs) {
        instance.last_param_values.push_back(instance.plugin->get_param(spec.id));
    }
}

void flush_plugin_param_changes(int client_fd, PluginInstance& instance) {
    if (instance.param_specs.empty()) {
        return;
    }
    if (instance.last_param_values.size() != instance.param_specs.size()) {
        refresh_plugin_param_cache(instance);
    }

    for (size_t i = 0; i < instance.param_specs.size(); ++i) {
        const float value = instance.plugin->get_param(instance.param_specs[i].id);
        if (std::fabs(value - instance.last_param_values[i]) > 1e-6f) {
            instance.last_param_values[i] = value;
            send_plugin_param_desc_value(client_fd, instance.instance_id, instance.param_specs[i], value, 1);
        }
    }
}

std::string cstr_from_fixed(const char* data, size_t max_len) {
    size_t len = 0;
    while (len < max_len && data[len] != '\0') {
        ++len;
    }
    return std::string(data, len);
}

bool save_rack_to_file(const RunnerRack& rack, const std::string& path) {
    if (path.empty()) {
        return false;
    }

    try {
        const std::filesystem::path p(path);
        if (!p.parent_path().empty()) {
            std::filesystem::create_directories(p.parent_path());
        }
    } catch (...) {
        return false;
    }

    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }

    out << "VSRK2\n";
    out << "master_gain " << rack.audio_state.volume.load(std::memory_order_relaxed) << "\n";
    out << "bypassed " << (rack.audio_state.bypassed.load(std::memory_order_relaxed) ? 1 : 0) << "\n";
    out << "plugin_count " << rack.plugins.size() << "\n";

    for (const auto& instance : rack.plugins) {
        const std::string plugin_id = plugin_identity_for_instance(rack, instance);
        const std::vector<PluginParamSpec> specs = instance.plugin ? instance.plugin->param_specs() : std::vector<PluginParamSpec>{};
        const std::vector<std::pair<std::string, std::string>> plugin_state = instance.plugin ? instance.plugin->save_state() : std::vector<std::pair<std::string, std::string>>{};
        out << "plugin " << plugin_id << " " << (instance.bypassed ? 1 : 0) << " " << specs.size() << " " << plugin_state.size() << "\n";
        for (const auto& spec : specs) {
            const float value = instance.plugin->get_param(spec.id);
            out << "param " << spec.id << " " << value << "\n";
        }
        for (const auto& kv : plugin_state) {
            out << "state " << std::quoted(kv.first) << " " << std::quoted(kv.second) << "\n";
        }
    }

    return out.good();
}

bool load_rack_from_file(const std::string& path, SavedRackState& out_state) {
    std::ifstream in(path);
    if (!in.is_open()) {
        return false;
    }

    std::string magic;
    in >> magic;
    const bool is_v1 = (magic == "VSRK1");
    const bool is_v2 = (magic == "VSRK2");
    if (!in.good() || (!is_v1 && !is_v2)) {
        return false;
    }

    std::string key;
    in >> key >> out_state.master_gain;
    if (!in.good() || key != "master_gain") {
        return false;
    }

    int bypassed = 0;
    in >> key >> bypassed;
    if (!in.good() || key != "bypassed") {
        return false;
    }
    out_state.bypassed = bypassed != 0;

    size_t plugin_count = 0;
    in >> key >> plugin_count;
    if (!in.good() || key != "plugin_count") {
        return false;
    }

    out_state.plugins.clear();
    out_state.plugins.reserve(plugin_count);

    for (size_t i = 0; i < plugin_count; ++i) {
        SavedPluginState plugin;
        int plugin_bypassed = 0;
        size_t param_count = 0;
        size_t state_count = 0;

        std::string plugin_token;
        in >> key >> plugin_token >> plugin_bypassed >> param_count;
        if (is_v2) {
            in >> state_count;
        }
        if (!in.good() || key != "plugin") {
            return false;
        }
        plugin.plugin_id = plugin_token;
        plugin.bypassed = plugin_bypassed != 0;

        plugin.params.reserve(param_count);
        for (size_t p = 0; p < param_count; ++p) {
            uint32_t param_id = 0;
            float value = 0.0f;
            in >> key >> param_id >> value;
            if (!in.good() || key != "param") {
                return false;
            }
            plugin.params.push_back({param_id, value});
        }

        plugin.state.reserve(state_count);
        for (size_t s = 0; s < state_count; ++s) {
            std::string state_key;
            std::string state_value;
            in >> key >> std::quoted(state_key) >> std::quoted(state_value);
            if (!in.good() || key != "state") {
                return false;
            }
            plugin.state.push_back({std::move(state_key), std::move(state_value)});
        }

        out_state.plugins.push_back(std::move(plugin));
    }

    return true;
}

bool save_plugin_preset_to_file(const RunnerRack& rack, const PluginInstance& instance, const std::string& path) {
    if (!instance.plugin || path.empty()) {
        return false;
    }

    try {
        const std::filesystem::path p(path);
        if (!p.parent_path().empty()) {
            std::filesystem::create_directories(p.parent_path());
        }
    } catch (...) {
        return false;
    }

    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }

    const std::vector<PluginParamSpec> specs = instance.plugin->param_specs();
    const std::string plugin_id = plugin_identity_for_instance(rack, instance);
    out << "VSPT2\n";
    out << "plugin_id " << plugin_id << "\n";
    out << "param_count " << specs.size() << "\n";
    const std::vector<std::pair<std::string, std::string>> plugin_state = instance.plugin->save_state();
    out << "state_count " << plugin_state.size() << "\n";
    for (const auto& spec : specs) {
        const float value = instance.plugin->get_param(spec.id);
        out << "param " << spec.id << " " << value << "\n";
    }
    for (const auto& kv : plugin_state) {
        out << "state " << std::quoted(kv.first) << " " << std::quoted(kv.second) << "\n";
    }

    return out.good();
}

bool load_plugin_preset_from_file(
    const std::string& path,
    std::string& plugin_id_out,
    std::vector<std::pair<uint32_t, float>>& params_out,
    std::vector<std::pair<std::string, std::string>>& state_out) {
    std::ifstream in(path);
    if (!in.is_open()) {
        return false;
    }

    std::string magic;
    in >> magic;
    const bool is_v1 = (magic == "VSPT1");
    const bool is_v2 = (magic == "VSPT2");
    if (!in.good() || (!is_v1 && !is_v2)) {
        return false;
    }

    std::string key;
    in >> key;
    if (!in.good()) {
        return false;
    }
    if (key == "plugin_id") {
        in >> plugin_id_out;
    } else if (key == "plugin_type_id") {
        uint32_t legacy_type_id = 0;
        in >> legacy_type_id;
        plugin_id_out = std::to_string(legacy_type_id);
    } else {
        return false;
    }
    if (!in.good()) {
        return false;
    }

    size_t param_count = 0;
    in >> key >> param_count;
    if (!in.good() || key != "param_count") {
        return false;
    }

    size_t state_count = 0;
    if (is_v2) {
        in >> key >> state_count;
        if (!in.good() || key != "state_count") {
            return false;
        }
    }

    params_out.clear();
    params_out.reserve(param_count);

    for (size_t i = 0; i < param_count; ++i) {
        uint32_t param_id = 0;
        float value = 0.0f;
        in >> key >> param_id >> value;
        if (!in.good() || key != "param") {
            return false;
        }
        params_out.push_back({param_id, value});
    }

    state_out.clear();
    state_out.reserve(state_count);
    for (size_t i = 0; i < state_count; ++i) {
        std::string state_key;
        std::string state_value;
        in >> key >> std::quoted(state_key) >> std::quoted(state_value);
        if (!in.good() || key != "state") {
            return false;
        }
        state_out.push_back({std::move(state_key), std::move(state_value)});
    }

    return true;
}

void send_plugin_instance_state(int client_fd, const PluginInstance& instance) {
    if (!instance.plugin) {
        return;
    }

    vessel::MsgPluginInstanceAdded added_msg;
    added_msg.instance_id = instance.instance_id;
    added_msg.plugin_type_id = instance.plugin->type_id();
    std::strncpy(added_msg.name, instance.plugin->display_name(), vessel::kMaxPluginNameLen);
    added_msg.name[vessel::kMaxPluginNameLen] = '\0';
    added_msg.has_custom_ui = instance.plugin->has_custom_ui() ? 1 : 0;
    send_ipc(client_fd, added_msg);

    send_plugin_ui_state(client_fd, instance.instance_id, instance.custom_ui_open);
    send_plugin_params(client_fd, instance.instance_id, *instance.plugin);
    send_plugin_custom_controls(client_fd, instance.instance_id, *instance.plugin);
}

void send_plugin_catalog(int client_fd) {
    std::vector<const vessel::PluginManifestEntry*> supported;
    supported.reserve(vessel::kDefaultPluginCount);

    for (size_t i = 0; i < vessel::kDefaultPluginCount; ++i) {
        const vessel::PluginManifestEntry& entry = vessel::kDefaultPlugins[i];
        if (plugin_is_supported(entry.plugin_type_id)) {
            supported.push_back(&entry);
        }
    }

    for (size_t i = 0; i < supported.size(); ++i) {
        vessel::MsgPluginCatalogEntry msg;
        msg.plugin_type_id = supported[i]->plugin_type_id;
        std::strncpy(msg.name, supported[i]->name, vessel::kMaxPluginNameLen);
        msg.name[vessel::kMaxPluginNameLen] = '\0';
        msg.is_last = (i + 1 == supported.size()) ? 1 : 0;
        send_ipc(client_fd, msg);
    }
}

void send_lv2_catalog(const RunnerRack& rack, int client_fd) {
    for (size_t i = 0; i < rack.lv2_plugins.size(); ++i) {
        vessel::MsgLv2CatalogEntry msg;
        msg.plugin_type_id = rack.lv2_plugins[i].plugin_type_id;
        std::strncpy(msg.name, rack.lv2_plugins[i].name.c_str(), vessel::kMaxPluginNameLen);
        msg.name[vessel::kMaxPluginNameLen] = '\0';
        msg.is_last = (i + 1 == rack.lv2_plugins.size()) ? 1 : 0;
        send_ipc(client_fd, msg);
    }
}

void send_plugin_params(int client_fd, uint32_t instance_id, RackPlugin& plugin) {
    const std::vector<PluginParamSpec> specs = plugin.param_specs();
    for (size_t i = 0; i < specs.size(); ++i) {
        const float value = plugin.get_param(specs[i].id);
        send_plugin_param_desc_value(client_fd, instance_id, specs[i], value, (i + 1 == specs.size()) ? 1 : 0);
        send_plugin_param_enum_options(client_fd, instance_id, specs[i]);
    }
}

void send_plugin_custom_controls(int client_fd, uint32_t instance_id, RackPlugin& plugin) {
    const std::vector<PluginCustomControlSpec> controls = plugin.custom_controls();
    for (size_t i = 0; i < controls.size(); ++i) {
        vessel::MsgPluginCustomControl msg;
        msg.instance_id = instance_id;
        msg.action_id = controls[i].action_id;
        msg.expects_text = controls[i].expects_text ? 1 : 0;
        msg.layout = controls[i].layout;
        msg.ui_width = controls[i].ui_width;
        std::strncpy(msg.label, controls[i].label.c_str(), vessel::kMaxParamNameLen);
        msg.label[vessel::kMaxParamNameLen] = '\0';
        msg.is_last = (i + 1 == controls.size()) ? 1 : 0;
        send_ipc(client_fd, msg);
    }
}

static void on_process(void* data, struct spa_io_position* position) {
    RunnerRack* rack = static_cast<RunnerRack*>(data);

    float sample_rate = 48000.0f;
    if (position && position->clock.rate.denom != 0) {
        const float maybe_rate = static_cast<float>(position->clock.rate.num)
            / static_cast<float>(position->clock.rate.denom);
        if (maybe_rate > 1000.0f) {
            sample_rate = maybe_rate;
        }
    }

    const float vol = rack->audio_state.volume.load(std::memory_order_relaxed);
    const bool bypassed = rack->audio_state.bypassed.load(std::memory_order_relaxed);
    float p_in = 0.0f;
    float p_out = 0.0f;

    for (int ch = 0; ch < 2; ++ch) {
        auto* b_in = static_cast<struct pw_buffer*>(pw_filter_dequeue_buffer(rack->input_ports[ch]));
        auto* b_out = static_cast<struct pw_buffer*>(pw_filter_dequeue_buffer(rack->output_ports[ch]));

        if (!b_out) {
            if (b_in) pw_filter_queue_buffer(rack->input_ports[ch], b_in);
            continue;
        }

        auto& d_out = b_out->buffer->datas[0];

        if (!b_in) {
            if (d_out.data) std::memset(d_out.data, 0, d_out.maxsize);
            d_out.chunk->offset = 0;
            d_out.chunk->stride = sizeof(float);
            d_out.chunk->size = d_out.maxsize;
            pw_filter_queue_buffer(rack->output_ports[ch], b_out);
            continue;
        }

        auto& d_in = b_in->buffer->datas[0];
        const float* in = static_cast<float*>(d_in.data);
        float* out = static_cast<float*>(d_out.data);
        const uint32_t n_samples = d_in.chunk->size / sizeof(float);

        for (uint32_t i = 0; i < n_samples; ++i) {
            float s = in[i];
            p_in = std::max(p_in, std::fabs(s));

            if (!bypassed) {
                s *= vol;
            }

            for (auto& instance : rack->plugins) {
                if (!instance.bypassed) {
                    s = instance.plugin->process_sample(s, sample_rate, ch);
                }
            }

            out[i] = s;
            p_out = std::max(p_out, std::fabs(s));
        }

        d_out.chunk->offset = 0;
        d_out.chunk->stride = sizeof(float);
        d_out.chunk->size = n_samples * sizeof(float);

        pw_filter_queue_buffer(rack->input_ports[ch], b_in);
        pw_filter_queue_buffer(rack->output_ports[ch], b_out);
    }

    rack->audio_state.in_peak.store(p_in, std::memory_order_relaxed);
    rack->audio_state.out_peak.store(p_out, std::memory_order_relaxed);
}

const struct pw_filter_events kFilterEvents = [] {
    struct pw_filter_events events{};
    events.version = PW_VERSION_FILTER_EVENTS;
    events.process = on_process;
    return events;
}();

uint32_t parse_rack_id(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--id") {
            return static_cast<uint32_t>(std::stoul(argv[i + 1]));
        }
    }
    return 0;
}

bool setup_audio(RunnerRack& rack, pw_loop* loop, uint32_t rack_id) {
    const std::string node_name = "vessel-rack-" + std::to_string(rack_id);
    rack.filter = pw_filter_new_simple(
        loop,
        node_name.c_str(),
        pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Filter",
            PW_KEY_MEDIA_CLASS, "Audio/Filter",
            PW_KEY_NODE_NAME, node_name.c_str(),
            nullptr),
        &kFilterEvents,
        &rack);

    if (!rack.filter) {
        return false;
    }

    rack.input_ports[0] = pw_filter_add_port(
        rack.filter,
        PW_DIRECTION_INPUT,
        PW_FILTER_PORT_FLAG_MAP_BUFFERS,
        sizeof(float),
        pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono", PW_KEY_PORT_NAME, "input_FL", "audio.channel", "FL", nullptr),
        nullptr,
        0);

    rack.input_ports[1] = pw_filter_add_port(
        rack.filter,
        PW_DIRECTION_INPUT,
        PW_FILTER_PORT_FLAG_MAP_BUFFERS,
        sizeof(float),
        pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono", PW_KEY_PORT_NAME, "input_FR", "audio.channel", "FR", nullptr),
        nullptr,
        0);

    rack.output_ports[0] = pw_filter_add_port(
        rack.filter,
        PW_DIRECTION_OUTPUT,
        PW_FILTER_PORT_FLAG_MAP_BUFFERS,
        sizeof(float),
        pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono", PW_KEY_PORT_NAME, "output_FL", "audio.channel", "FL", nullptr),
        nullptr,
        0);

    rack.output_ports[1] = pw_filter_add_port(
        rack.filter,
        PW_DIRECTION_OUTPUT,
        PW_FILTER_PORT_FLAG_MAP_BUFFERS,
        sizeof(float),
        pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono", PW_KEY_PORT_NAME, "output_FR", "audio.channel", "FR", nullptr),
        nullptr,
        0);

    if (!rack.input_ports[0] || !rack.input_ports[1] || !rack.output_ports[0] || !rack.output_ports[1]) {
        return false;
    }

    return pw_filter_connect(rack.filter, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0) >= 0;
}

bool read_client_bytes(RunnerRack& rack, int& client_fd) {
    if (client_fd < 0) return true;

    uint8_t chunk[1024];
    while (true) {
        const ssize_t n = ::recv(client_fd, chunk, sizeof(chunk), MSG_DONTWAIT);
        if (n > 0) {
            rack.rx_buffer.insert(rack.rx_buffer.end(), chunk, chunk + n);
            continue;
        }
        if (n == 0) {
            ::close(client_fd);
            client_fd = -1;
            rack.rx_buffer.clear();
            return false;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        }
        ::close(client_fd);
        client_fd = -1;
        rack.rx_buffer.clear();
        return false;
    }
}

void handle_messages(RunnerRack& rack, int client_fd, pw_thread_loop* thread_loop) {
    while (rack.rx_buffer.size() >= sizeof(vessel::MsgHeader)) {
        const auto* hdr = reinterpret_cast<const vessel::MsgHeader*>(rack.rx_buffer.data());
        const size_t frame_size = hdr->size;
        if (frame_size < sizeof(vessel::MsgHeader)) {
            rack.rx_buffer.clear();
            return;
        }
        if (rack.rx_buffer.size() < frame_size) {
            return;
        }

        const uint8_t* frame = rack.rx_buffer.data();

        if (hdr->type == vessel::MsgType::SET_GAIN && frame_size == sizeof(vessel::MsgSetGain)) {
            const auto* msg = reinterpret_cast<const vessel::MsgSetGain*>(frame);
            rack.audio_state.volume.store(msg->gain, std::memory_order_relaxed);
        } else if (hdr->type == vessel::MsgType::SET_BYPASS && frame_size == sizeof(vessel::MsgSetBypass)) {
            const auto* msg = reinterpret_cast<const vessel::MsgSetBypass*>(frame);
            rack.audio_state.bypassed.store(msg->bypassed != 0, std::memory_order_relaxed);
        } else if (hdr->type == vessel::MsgType::REQ_PLUGIN_CATALOG && frame_size == sizeof(vessel::MsgReqPluginCatalog)) {
            send_plugin_catalog(client_fd);
        } else if (hdr->type == vessel::MsgType::REQ_LV2_CATALOG && frame_size == sizeof(vessel::MsgReqLv2Catalog)) {
            send_lv2_catalog(rack, client_fd);
        } else if (hdr->type == vessel::MsgType::ADD_PLUGIN && frame_size == sizeof(vessel::MsgAddPlugin)) {
            const auto* msg = reinterpret_cast<const vessel::MsgAddPlugin*>(frame);
            std::unique_ptr<RackPlugin> plugin = create_plugin(msg->plugin_type_id);
            if (!plugin) {
                const DiscoveredLv2Plugin* lv2 = find_lv2_plugin(rack, msg->plugin_type_id);
                if (lv2) {
                    plugin = create_lv2_plugin(*lv2);
                }
            }
            if (plugin) {
                const uint32_t instance_id = rack.next_plugin_instance_id++;

                pw_thread_loop_lock(thread_loop);
                PluginInstance new_instance;
                new_instance.instance_id = instance_id;
                new_instance.bypassed = false;
                new_instance.plugin = std::move(plugin);
                rack.plugins.push_back(std::move(new_instance));
                PluginInstance* added_instance = find_plugin_instance(rack, instance_id);
                if (added_instance && added_instance->plugin) {
                    refresh_plugin_param_cache(*added_instance);
                    added_instance->custom_ui_open = false;
                    send_plugin_instance_state(client_fd, *added_instance);
                }
                pw_thread_loop_unlock(thread_loop);
            }
        } else if (hdr->type == vessel::MsgType::REQ_PLUGIN_PARAMS && frame_size == sizeof(vessel::MsgReqPluginParams)) {
            const auto* msg = reinterpret_cast<const vessel::MsgReqPluginParams*>(frame);
            pw_thread_loop_lock(thread_loop);
            PluginInstance* instance = find_plugin_instance(rack, msg->instance_id);
            if (instance && instance->plugin) {
                send_plugin_params_reset(client_fd, msg->instance_id);
                send_plugin_params(client_fd, msg->instance_id, *instance->plugin);
                send_plugin_custom_controls(client_fd, msg->instance_id, *instance->plugin);
            }
            pw_thread_loop_unlock(thread_loop);
        } else if (hdr->type == vessel::MsgType::SET_PLUGIN_PARAM && frame_size == sizeof(vessel::MsgSetPluginParam)) {
            const auto* msg = reinterpret_cast<const vessel::MsgSetPluginParam*>(frame);
            pw_thread_loop_lock(thread_loop);
            PluginInstance* instance = find_plugin_instance(rack, msg->instance_id);
            if (instance && instance->plugin) {
                const uint64_t before = instance->plugin->ui_schema_version();
                instance->plugin->set_param(msg->param_id, msg->value);
                const uint64_t after = instance->plugin->ui_schema_version();
                if (after != before) {
                    refresh_plugin_param_cache(*instance);
                    send_plugin_params_reset(client_fd, msg->instance_id);
                    send_plugin_params(client_fd, msg->instance_id, *instance->plugin);
                    send_plugin_custom_controls(client_fd, msg->instance_id, *instance->plugin);
                }
            }
            pw_thread_loop_unlock(thread_loop);
        } else if (hdr->type == vessel::MsgType::REMOVE_PLUGIN && frame_size == sizeof(vessel::MsgRemovePlugin)) {
            const auto* msg = reinterpret_cast<const vessel::MsgRemovePlugin*>(frame);
            pw_thread_loop_lock(thread_loop);
            rack.plugins.erase(
                std::remove_if(
                    rack.plugins.begin(),
                    rack.plugins.end(),
                    [&](const PluginInstance& p) { return p.instance_id == msg->instance_id; }),
                rack.plugins.end());
            pw_thread_loop_unlock(thread_loop);
        } else if (hdr->type == vessel::MsgType::SET_PLUGIN_BYPASS && frame_size == sizeof(vessel::MsgSetPluginBypass)) {
            const auto* msg = reinterpret_cast<const vessel::MsgSetPluginBypass*>(frame);
            pw_thread_loop_lock(thread_loop);
            PluginInstance* instance = find_plugin_instance(rack, msg->instance_id);
            if (instance) {
                instance->bypassed = msg->bypassed != 0;
            }
            pw_thread_loop_unlock(thread_loop);
        } else if (hdr->type == vessel::MsgType::MOVE_PLUGIN && frame_size == sizeof(vessel::MsgMovePlugin)) {
            const auto* msg = reinterpret_cast<const vessel::MsgMovePlugin*>(frame);
            pw_thread_loop_lock(thread_loop);
            move_plugin_instance(rack.plugins, msg->instance_id, msg->target_index);
            pw_thread_loop_unlock(thread_loop);
        } else if (hdr->type == vessel::MsgType::OPEN_PLUGIN_UI && frame_size == sizeof(vessel::MsgOpenPluginUi)) {
            const auto* msg = reinterpret_cast<const vessel::MsgOpenPluginUi*>(frame);
            pw_thread_loop_lock(thread_loop);
            PluginInstance* instance = find_plugin_instance(rack, msg->instance_id);
            if (instance && instance->plugin && instance->plugin->has_custom_ui()) {
                if (instance->plugin->is_custom_ui_open()) {
                    instance->plugin->close_custom_ui();
                } else {
                    instance->plugin->open_custom_ui();
                }
                const bool is_open = instance->plugin->is_custom_ui_open();
                instance->custom_ui_open = is_open;
                send_plugin_ui_state(client_fd, msg->instance_id, is_open);
            }
            pw_thread_loop_unlock(thread_loop);
        } else if (hdr->type == vessel::MsgType::TRIGGER_PLUGIN_CUSTOM_ACTION
                   && frame_size == sizeof(vessel::MsgTriggerPluginCustomAction)) {
            const auto* msg = reinterpret_cast<const vessel::MsgTriggerPluginCustomAction*>(frame);
            pw_thread_loop_lock(thread_loop);
            PluginInstance* instance = find_plugin_instance(rack, msg->instance_id);
            if (instance && instance->plugin) {
                const uint64_t before = instance->plugin->ui_schema_version();
                instance->plugin->trigger_custom_action(msg->action_id);
                const uint64_t after = instance->plugin->ui_schema_version();
                if (after != before) {
                    refresh_plugin_param_cache(*instance);
                    send_plugin_params_reset(client_fd, msg->instance_id);
                    send_plugin_params(client_fd, msg->instance_id, *instance->plugin);
                    send_plugin_custom_controls(client_fd, msg->instance_id, *instance->plugin);
                }
            }
            pw_thread_loop_unlock(thread_loop);
        } else if (hdr->type == vessel::MsgType::SET_PLUGIN_CUSTOM_TEXT
                   && frame_size == sizeof(vessel::MsgSetPluginCustomText)) {
            const auto* msg = reinterpret_cast<const vessel::MsgSetPluginCustomText*>(frame);
            const std::string text = cstr_from_fixed(msg->text, vessel::kMaxFilePathLen);
            pw_thread_loop_lock(thread_loop);
            PluginInstance* instance = find_plugin_instance(rack, msg->instance_id);
            if (instance && instance->plugin) {
                const uint64_t before = instance->plugin->ui_schema_version();
                instance->plugin->set_custom_text(msg->action_id, text);
                const uint64_t after = instance->plugin->ui_schema_version();
                if (after != before) {
                    refresh_plugin_param_cache(*instance);
                    send_plugin_params_reset(client_fd, msg->instance_id);
                    send_plugin_params(client_fd, msg->instance_id, *instance->plugin);
                    send_plugin_custom_controls(client_fd, msg->instance_id, *instance->plugin);
                }
            }
            pw_thread_loop_unlock(thread_loop);
        } else if (hdr->type == vessel::MsgType::SAVE_RACK_TO_FILE && frame_size == sizeof(vessel::MsgSaveRackToFile)) {
            const auto* msg = reinterpret_cast<const vessel::MsgSaveRackToFile*>(frame);
            const std::string path = cstr_from_fixed(msg->path, vessel::kMaxFilePathLen);
            pw_thread_loop_lock(thread_loop);
            save_rack_to_file(rack, path);
            pw_thread_loop_unlock(thread_loop);
        } else if (hdr->type == vessel::MsgType::LOAD_RACK_FROM_FILE && frame_size == sizeof(vessel::MsgLoadRackFromFile)) {
            const auto* msg = reinterpret_cast<const vessel::MsgLoadRackFromFile*>(frame);
            const std::string path = cstr_from_fixed(msg->path, vessel::kMaxFilePathLen);

            SavedRackState state;
            if (!load_rack_from_file(path, state)) {
                rack.rx_buffer.erase(rack.rx_buffer.begin(), rack.rx_buffer.begin() + static_cast<long>(frame_size));
                continue;
            }

            pw_thread_loop_lock(thread_loop);

            rack.audio_state.volume.store(state.master_gain, std::memory_order_relaxed);
            rack.audio_state.bypassed.store(state.bypassed, std::memory_order_relaxed);
            rack.plugins.clear();

            for (const auto& saved : state.plugins) {
                uint32_t resolved_type_id = 0;
                if (!resolve_plugin_type_id(rack, saved.plugin_id, resolved_type_id)) {
                    continue;
                }

                std::unique_ptr<RackPlugin> plugin = create_plugin(resolved_type_id);
                if (!plugin) {
                    const DiscoveredLv2Plugin* lv2 = find_lv2_plugin(rack, resolved_type_id);
                    if (lv2) {
                        plugin = create_lv2_plugin(*lv2);
                    }
                }
                if (!plugin) {
                    continue;
                }

                const uint32_t instance_id = rack.next_plugin_instance_id++;
                PluginInstance instance;
                instance.instance_id = instance_id;
                instance.bypassed = saved.bypassed;
                instance.plugin = std::move(plugin);

                instance.plugin->load_state(saved.state);

                for (const auto& param : saved.params) {
                    instance.plugin->set_param(param.first, param.second);
                }

                refresh_plugin_param_cache(instance);
                instance.custom_ui_open = false;
                rack.plugins.push_back(std::move(instance));
            }

            send_rack_state_reset(client_fd);
            for (const auto& instance : rack.plugins) {
                send_plugin_instance_state(client_fd, instance);
            }

            pw_thread_loop_unlock(thread_loop);
        } else if (hdr->type == vessel::MsgType::SAVE_PLUGIN_PRESET && frame_size == sizeof(vessel::MsgSavePluginPreset)) {
            const auto* msg = reinterpret_cast<const vessel::MsgSavePluginPreset*>(frame);
            const std::string path = cstr_from_fixed(msg->path, vessel::kMaxFilePathLen);
            pw_thread_loop_lock(thread_loop);
            PluginInstance* instance = find_plugin_instance(rack, msg->instance_id);
            if (instance) {
                save_plugin_preset_to_file(rack, *instance, path);
            }
            pw_thread_loop_unlock(thread_loop);
        } else if (hdr->type == vessel::MsgType::LOAD_PLUGIN_PRESET && frame_size == sizeof(vessel::MsgLoadPluginPreset)) {
            const auto* msg = reinterpret_cast<const vessel::MsgLoadPluginPreset*>(frame);
            const std::string path = cstr_from_fixed(msg->path, vessel::kMaxFilePathLen);

            std::string preset_plugin_id;
            std::vector<std::pair<uint32_t, float>> params;
            std::vector<std::pair<std::string, std::string>> preset_state;
            if (!load_plugin_preset_from_file(path, preset_plugin_id, params, preset_state)) {
                rack.rx_buffer.erase(rack.rx_buffer.begin(), rack.rx_buffer.begin() + static_cast<long>(frame_size));
                continue;
            }

            pw_thread_loop_lock(thread_loop);
            PluginInstance* instance = find_plugin_instance(rack, msg->instance_id);
            uint32_t preset_type_id = 0;
            const bool resolved = resolve_plugin_type_id(rack, preset_plugin_id, preset_type_id);
            if (instance && instance->plugin && resolved && instance->plugin->type_id() == preset_type_id) {
                instance->plugin->load_state(preset_state);
                for (const auto& param : params) {
                    instance->plugin->set_param(param.first, param.second);
                }
            }
            pw_thread_loop_unlock(thread_loop);
        }

        rack.rx_buffer.erase(rack.rx_buffer.begin(), rack.rx_buffer.begin() + static_cast<long>(frame_size));
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    const uint32_t rack_id = parse_rack_id(argc, argv);

    pw_init(&argc, &argv);
    suil_init(&argc, &argv, SUIL_ARG_NONE);

    struct pw_thread_loop* thread_loop = pw_thread_loop_new("VesselRunnerLoop", nullptr);
    if (!thread_loop) {
        std::cerr << "failed to create PipeWire thread loop" << std::endl;
        pw_deinit();
        return 1;
    }

    if (pw_thread_loop_start(thread_loop) < 0) {
        std::cerr << "failed to start PipeWire thread loop" << std::endl;
        pw_thread_loop_destroy(thread_loop);
        pw_deinit();
        return 1;
    }

    RunnerRack rack;
    rack.lv2_plugins = scan_lv2_plugins();

    pw_thread_loop_lock(thread_loop);
    bool ok = setup_audio(rack, pw_thread_loop_get_loop(thread_loop), rack_id);
    pw_thread_loop_unlock(thread_loop);

    if (!ok) {
        std::cerr << "failed to initialize runner for rack id: " << rack_id << std::endl;
        pw_thread_loop_stop(thread_loop);
        pw_thread_loop_destroy(thread_loop);
        pw_deinit();
        return 1;
    }

    int server_fd = -1;
    int client_fd = -1;
    const std::string sock_path = vessel::socket_path_by_id(rack_id);
    ::unlink(sock_path.c_str());

    server_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd >= 0) {
        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);
        if (::bind(server_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0
            && ::listen(server_fd, 4) == 0) {
            ::fcntl(server_fd, F_SETFL, O_NONBLOCK);
        } else {
            ::close(server_fd);
            server_fd = -1;
        }
    }

    auto last_peak_send = std::chrono::steady_clock::now();

    while (g_running.load(std::memory_order_relaxed)) {
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;
        if (server_fd >= 0) {
            FD_SET(server_fd, &rfds);
            maxfd = std::max(maxfd, server_fd);
        }
        if (client_fd >= 0) {
            FD_SET(client_fd, &rfds);
            maxfd = std::max(maxfd, client_fd);
        }

        struct timeval tv{0, 10000};
        const int sel = ::select(maxfd + 1, &rfds, nullptr, nullptr, &tv);
        if (sel < 0) continue;

        if (sel > 0) {
            if (server_fd >= 0 && FD_ISSET(server_fd, &rfds)) {
                const int new_fd = ::accept(server_fd, nullptr, nullptr);
                if (new_fd >= 0) {
                    if (client_fd >= 0) ::close(client_fd);
                    client_fd = new_fd;
                    rack.rx_buffer.clear();
                    ::fcntl(client_fd, F_SETFL, O_NONBLOCK);
                }
            }

            if (client_fd >= 0 && FD_ISSET(client_fd, &rfds)) {
                if (read_client_bytes(rack, client_fd)) {
                    handle_messages(rack, client_fd, thread_loop);
                }
            }
        }

        for (auto& instance : rack.plugins) {
            instance.plugin->pump_ui();

            const bool ui_is_open = instance.plugin->is_custom_ui_open();
            if (ui_is_open != instance.custom_ui_open) {
                instance.custom_ui_open = ui_is_open;
                send_plugin_ui_state(client_fd, instance.instance_id, ui_is_open);
            }

            flush_plugin_param_changes(client_fd, instance);
        }

        const auto now = std::chrono::steady_clock::now();
        if (client_fd >= 0
            && std::chrono::duration_cast<std::chrono::milliseconds>(now - last_peak_send).count() >= 50) {
            vessel::MsgPeakLevels msg;
            msg.in_peak = rack.audio_state.in_peak.exchange(0.0f, std::memory_order_relaxed);
            msg.out_peak = rack.audio_state.out_peak.exchange(0.0f, std::memory_order_relaxed);
            send_ipc(client_fd, msg);
            last_peak_send = now;
        }
    }

    if (client_fd >= 0) ::close(client_fd);
    if (server_fd >= 0) {
        ::close(server_fd);
        ::unlink(sock_path.c_str());
    }

    pw_thread_loop_stop(thread_loop);
    pw_thread_loop_destroy(thread_loop);
    pw_deinit();

    return 0;
}
