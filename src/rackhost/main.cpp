#include <algorithm>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <pipewire/filter.h>
#include <pipewire/pipewire.h>

#include "protocol.h"

namespace {
std::atomic<bool> g_running{true};

struct AudioState {
    std::atomic<float> volume{1.0f};
    std::atomic<bool> bypassed{false};
    std::atomic<float> in_peak{0.0f};
    std::atomic<float> out_peak{0.0f};
};

struct RunnerRack {
    std::string name;
    struct pw_filter* filter = nullptr;
    void* input_port = nullptr;
    void* output_port = nullptr;
    AudioState audio_state;

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

static void on_process(void* data, struct spa_io_position*) {
    RunnerRack* rack = static_cast<RunnerRack*>(data);

    auto* b_in = static_cast<struct pw_buffer*>(pw_filter_dequeue_buffer(rack->input_port));
    auto* b_out = static_cast<struct pw_buffer*>(pw_filter_dequeue_buffer(rack->output_port));

    if (!b_in || !b_out) {
        if (b_in) {
            pw_filter_queue_buffer(rack->input_port, b_in);
        }
        if (b_out) {
            pw_filter_queue_buffer(rack->output_port, b_out);
        }
        return;
    }

    float* in = static_cast<float*>(b_in->buffer->datas[0].data);
    float* out = static_cast<float*>(b_out->buffer->datas[0].data);

    const uint32_t n_samples = b_in->buffer->datas[0].chunk->size / sizeof(float);

    float p_in = 0.0f;
    float p_out = 0.0f;
    const float vol = rack->audio_state.volume.load(std::memory_order_relaxed);
    const bool bypassed = rack->audio_state.bypassed.load(std::memory_order_relaxed);

    for (uint32_t i = 0; i < n_samples; ++i) {
        float s = in[i];
        p_in = std::max(p_in, std::fabs(s));

        if (!bypassed) {
            s *= vol;
        }

        out[i] = s;
        p_out = std::max(p_out, std::fabs(s));
    }

    rack->audio_state.in_peak.store(p_in, std::memory_order_relaxed);
    rack->audio_state.out_peak.store(p_out, std::memory_order_relaxed);

    pw_filter_queue_buffer(rack->input_port, b_in);
    pw_filter_queue_buffer(rack->output_port, b_out);
}

const struct pw_filter_events kFilterEvents = [] {
    struct pw_filter_events events{};
    events.version = PW_VERSION_FILTER_EVENTS;
    events.process = on_process;
    return events;
}();

std::string parse_rack_name(int argc, char** argv) {
    std::string rack_name = "Main Rack";
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--name") {
            rack_name = argv[i + 1];
            break;
        }
    }
    return rack_name;
}

bool setup_audio(RunnerRack& rack, pw_loop* loop) {
    rack.filter = pw_filter_new_simple(
        loop,
        rack.name.c_str(),
        pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Filter",
            "node.name", rack.name.c_str(),
            nullptr),
        &kFilterEvents,
        &rack);

    if (!rack.filter) {
        return false;
    }

    rack.input_port = pw_filter_add_port(
        rack.filter,
        PW_DIRECTION_INPUT,
        PW_FILTER_PORT_FLAG_MAP_BUFFERS,
        sizeof(float),
        pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono", PW_KEY_PORT_NAME, "input", nullptr),
        nullptr,
        0);

    rack.output_port = pw_filter_add_port(
        rack.filter,
        PW_DIRECTION_OUTPUT,
        PW_FILTER_PORT_FLAG_MAP_BUFFERS,
        sizeof(float),
        pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono", PW_KEY_PORT_NAME, "output", nullptr),
        nullptr,
        0);

    if (!rack.input_port || !rack.output_port) {
        return false;
    }

    return pw_filter_connect(rack.filter, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0) >= 0;
}
}  // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::string rack_name = parse_rack_name(argc, argv);

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
    rack.name = rack_name;

    pw_thread_loop_lock(thread_loop);
    bool ok = setup_audio(rack, pw_thread_loop_get_loop(thread_loop));
    pw_thread_loop_unlock(thread_loop);

    if (!ok) {
        std::cerr << "failed to initialize runner for rack: " << rack_name << std::endl;
        pw_thread_loop_stop(thread_loop);
        pw_thread_loop_destroy(thread_loop);
        pw_deinit();
        return 1;
    }

    // --- Unix Domain Socket Server ---
    int server_fd = -1;
    int client_fd = -1;
    const std::string sock_path = vessel::socket_path(rack_name);
    ::unlink(sock_path.c_str());  // remove stale socket from a previous crash

    server_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd >= 0) {
        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        ::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);
        if (::bind(server_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0
            && ::listen(server_fd, 4) == 0) {
            ::fcntl(server_fd, F_SETFL, O_NONBLOCK);
        } else {
            ::close(server_fd);
            server_fd = -1;
        }
    }

    // --- Main idle loop (select-based, 10 ms max latency per iteration) ---
    while (g_running.load(std::memory_order_relaxed)) {
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;
        if (server_fd >= 0) { FD_SET(server_fd, &rfds); maxfd = std::max(maxfd, server_fd); }
        if (client_fd >= 0) { FD_SET(client_fd, &rfds); maxfd = std::max(maxfd, client_fd); }

        struct timeval tv{0, 10000};  // 10 ms
        if (::select(maxfd + 1, &rfds, nullptr, nullptr, &tv) <= 0) {
            continue;
        }

        // Accept a new GUI connection (replaces any previously connected client).
        if (server_fd >= 0 && FD_ISSET(server_fd, &rfds)) {
            const int new_fd = ::accept(server_fd, nullptr, nullptr);
            if (new_fd >= 0) {
                if (client_fd >= 0) ::close(client_fd);
                client_fd = new_fd;
                ::fcntl(client_fd, F_SETFL, O_NONBLOCK);
            }
        }

        // Drain incoming control messages.
        if (client_fd >= 0 && FD_ISSET(client_fd, &rfds)) {
            while (true) {
                vessel::MsgHeader hdr{};
                const ssize_t n = ::recv(client_fd, &hdr, sizeof(hdr), MSG_DONTWAIT);
                if (n == 0) {
                    // GUI disconnected cleanly.
                    ::close(client_fd);
                    client_fd = -1;
                    break;
                }
                if (n < static_cast<ssize_t>(sizeof(hdr))) {
                    break;  // EAGAIN or incomplete header — try again next iteration.
                }

                const uint8_t payload_size = hdr.size - static_cast<uint8_t>(sizeof(hdr));

                if (hdr.type == vessel::MsgType::SET_GAIN
                    && payload_size == sizeof(vessel::MsgSetGain::gain)) {
                    float gain = 1.0f;
                    if (::recv(client_fd, &gain, sizeof(gain), MSG_DONTWAIT) == sizeof(gain)) {
                        rack.audio_state.volume.store(gain, std::memory_order_relaxed);
                    }
                } else if (hdr.type == vessel::MsgType::SET_BYPASS
                           && payload_size == sizeof(vessel::MsgSetBypass::bypassed)) {
                    uint8_t val = 0;
                    if (::recv(client_fd, &val, sizeof(val), MSG_DONTWAIT) == sizeof(val)) {
                        rack.audio_state.bypassed.store(val != 0, std::memory_order_relaxed);
                    }
                }
            }
        }
    }

    // --- Cleanup ---
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
