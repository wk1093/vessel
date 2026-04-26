#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

#include <pipewire/filter.h>
#include <pipewire/pipewire.h>

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

RackPlugin* find_plugin(RunnerRack& rack, uint32_t instance_id) {
    for (auto& instance : rack.plugins) {
        if (instance.instance_id == instance_id) {
            return instance.plugin.get();
        }
    }
    return nullptr;
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

template <typename T>
void send_ipc(int client_fd, const T& msg) {
    if (client_fd < 0) return;
    const ssize_t n = ::send(client_fd, &msg, sizeof(T), MSG_NOSIGNAL);
    if (n <= 0) {
        // Client disconnect is handled by the select loop; ignore send errors here.
    }
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
        const PluginParamSpec& spec = specs[i];
        vessel::MsgPluginParamDesc msg;
        msg.instance_id = instance_id;
        msg.param_id = spec.id;
        msg.widget = spec.widget;
        msg.min_value = spec.min_value;
        msg.max_value = spec.max_value;
        msg.value = plugin.get_param(spec.id);
        std::strncpy(msg.name, spec.name, vessel::kMaxParamNameLen);
        msg.name[vessel::kMaxParamNameLen] = '\0';
        msg.is_last = (i + 1 == specs.size()) ? 1 : 0;
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
                const uint32_t type_id = msg->plugin_type_id;
                const char* display_name = plugin->display_name();

                pw_thread_loop_lock(thread_loop);
                rack.plugins.push_back({instance_id, false, std::move(plugin)});
                RackPlugin* added = find_plugin(rack, instance_id);
                if (added) {
                    vessel::MsgPluginInstanceAdded added_msg;
                    added_msg.instance_id = instance_id;
                    added_msg.plugin_type_id = type_id;
                    std::strncpy(added_msg.name, display_name, vessel::kMaxPluginNameLen);
                    added_msg.name[vessel::kMaxPluginNameLen] = '\0';
                    send_ipc(client_fd, added_msg);
                    send_plugin_params(client_fd, instance_id, *added);
                }
                pw_thread_loop_unlock(thread_loop);
            }
        } else if (hdr->type == vessel::MsgType::REQ_PLUGIN_PARAMS && frame_size == sizeof(vessel::MsgReqPluginParams)) {
            const auto* msg = reinterpret_cast<const vessel::MsgReqPluginParams*>(frame);
            pw_thread_loop_lock(thread_loop);
            RackPlugin* plugin = find_plugin(rack, msg->instance_id);
            if (plugin) {
                send_plugin_params(client_fd, msg->instance_id, *plugin);
            }
            pw_thread_loop_unlock(thread_loop);
        } else if (hdr->type == vessel::MsgType::SET_PLUGIN_PARAM && frame_size == sizeof(vessel::MsgSetPluginParam)) {
            const auto* msg = reinterpret_cast<const vessel::MsgSetPluginParam*>(frame);
            pw_thread_loop_lock(thread_loop);
            RackPlugin* plugin = find_plugin(rack, msg->instance_id);
            if (plugin) {
                plugin->set_param(msg->param_id, msg->value);
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
