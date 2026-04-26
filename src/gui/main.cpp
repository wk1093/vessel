#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "plugins.h"
#include "protocol.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

struct RackPluginType {
    uint32_t plugin_type_id = 0;
    std::string name;
};

struct RackPluginParam {
    uint32_t param_id = 0;
    vessel::ParamWidget widget = vessel::ParamWidget::SLIDER;
    std::string name;
    float min_value = 0.0f;
    float max_value = 1.0f;
    float value = 0.0f;
};

struct RackPluginInstance {
    uint32_t instance_id = 0;
    uint32_t plugin_type_id = 0;
    std::string name;
    bool bypassed = false;
    bool is_open = true;
    std::vector<RackPluginParam> params;
};

struct Rack {
    std::string name;
    uint32_t id = 0;
    bool visible = true;
    bool runner_alive = false;
    pid_t runner_pid = -1;

    int sock_fd = -1;
    float master_gain = 1.0f;
    bool bypassed_ui = false;

    float in_peak = 0.0f;
    float out_peak = 0.0f;

    bool open_plugin_browser = false;
    std::vector<RackPluginType> available_plugins;
    std::vector<RackPluginInstance> plugins;
    std::vector<uint8_t> rx_buffer;

    ~Rack() {
        if (sock_fd >= 0) {
            ::close(sock_fd);
            sock_fd = -1;
        }
    }
};

pid_t spawn_runner(const std::string& runner_binary, uint32_t id) {
    pid_t pid = fork();
    if (pid == 0) {
        const std::string id_str = std::to_string(id);
        execl(runner_binary.c_str(), runner_binary.c_str(), "--id", id_str.c_str(), nullptr);
        _exit(127);
    }
    return pid;
}

void poll_runner_status(Rack& rack) {
    if (!rack.runner_alive || rack.runner_pid <= 0) {
        return;
    }

    int status = 0;
    pid_t rc = waitpid(rack.runner_pid, &status, WNOHANG);
    if (rc == rack.runner_pid) {
        rack.runner_alive = false;
        rack.runner_pid = -1;
    }
}

void terminate_runner(Rack& rack) {
    if (rack.runner_pid <= 0) {
        rack.runner_alive = false;
        rack.runner_pid = -1;
        return;
    }

    kill(rack.runner_pid, SIGTERM);
    waitpid(rack.runner_pid, nullptr, 0);
    rack.runner_alive = false;
    rack.runner_pid = -1;
}

std::string get_runner_binary_path(const char* argv0) {
    std::filesystem::path exe_path(argv0);
    if (!exe_path.is_absolute()) {
        exe_path = std::filesystem::absolute(exe_path);
    }

    return (exe_path.parent_path() / "vessel-runner").string();
}

void try_connect_socket(Rack& rack) {
    if (rack.sock_fd >= 0) return;

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const std::string path = vessel::socket_path_by_id(rack.id);
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
        ::fcntl(fd, F_SETFL, O_NONBLOCK);
        rack.sock_fd = fd;
        rack.rx_buffer.clear();
    } else {
        ::close(fd);
    }
}

template <typename T>
void send_ipc(Rack& rack, const T& msg) {
    if (rack.sock_fd < 0) return;
    const ssize_t n = ::send(rack.sock_fd, &msg, sizeof(T), MSG_NOSIGNAL);
    if (n <= 0) {
        ::close(rack.sock_fd);
        rack.sock_fd = -1;
    }
}

RackPluginInstance* find_plugin_instance(Rack& rack, uint32_t instance_id) {
    for (auto& plugin : rack.plugins) {
        if (plugin.instance_id == instance_id) {
            return &plugin;
        }
    }
    return nullptr;
}

bool move_plugin_instance(std::vector<RackPluginInstance>& plugins, uint32_t instance_id, size_t target_index) {
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

    size_t to = std::min(target_index, plugins.size() - 1);
    if (from == to) {
        return false;
    }

    RackPluginInstance moved = std::move(plugins[from]);
    plugins.erase(plugins.begin() + static_cast<long>(from));
    if (to > from) {
        --to;
    }
    plugins.insert(plugins.begin() + static_cast<long>(to), std::move(moved));
    return true;
}

void drain_ipc(Rack& rack) {
    if (rack.sock_fd < 0) return;

    uint8_t chunk[1024];
    while (true) {
        const ssize_t n = ::recv(rack.sock_fd, chunk, sizeof(chunk), MSG_DONTWAIT);
        if (n > 0) {
            rack.rx_buffer.insert(rack.rx_buffer.end(), chunk, chunk + n);
            continue;
        }

        if (n == 0) {
            ::close(rack.sock_fd);
            rack.sock_fd = -1;
            rack.rx_buffer.clear();
            return;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }

        ::close(rack.sock_fd);
        rack.sock_fd = -1;
        rack.rx_buffer.clear();
        return;
    }

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

        if (hdr->type == vessel::MsgType::PEAK_LEVELS && frame_size == sizeof(vessel::MsgPeakLevels)) {
            const auto* msg = reinterpret_cast<const vessel::MsgPeakLevels*>(frame);
            rack.in_peak = std::max(rack.in_peak, msg->in_peak);
            rack.out_peak = std::max(rack.out_peak, msg->out_peak);
        } else if (hdr->type == vessel::MsgType::PLUGIN_CATALOG_ENTRY
                   && frame_size == sizeof(vessel::MsgPluginCatalogEntry)) {
            const auto* msg = reinterpret_cast<const vessel::MsgPluginCatalogEntry*>(frame);
            bool exists = false;
            for (const auto& existing : rack.available_plugins) {
                if (existing.plugin_type_id == msg->plugin_type_id) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                rack.available_plugins.push_back({msg->plugin_type_id, msg->name});
            }
        } else if (hdr->type == vessel::MsgType::PLUGIN_INSTANCE_ADDED
                   && frame_size == sizeof(vessel::MsgPluginInstanceAdded)) {
            const auto* msg = reinterpret_cast<const vessel::MsgPluginInstanceAdded*>(frame);
            if (!find_plugin_instance(rack, msg->instance_id)) {
                RackPluginInstance plugin;
                plugin.instance_id = msg->instance_id;
                plugin.plugin_type_id = msg->plugin_type_id;
                plugin.name = msg->name;
                rack.plugins.push_back(std::move(plugin));
            }
        } else if (hdr->type == vessel::MsgType::PLUGIN_PARAM_DESC
                   && frame_size == sizeof(vessel::MsgPluginParamDesc)) {
            const auto* msg = reinterpret_cast<const vessel::MsgPluginParamDesc*>(frame);
            RackPluginInstance* plugin = find_plugin_instance(rack, msg->instance_id);
            if (plugin) {
                RackPluginParam* param_ptr = nullptr;
                for (auto& existing : plugin->params) {
                    if (existing.param_id == msg->param_id) {
                        param_ptr = &existing;
                        break;
                    }
                }
                if (!param_ptr) {
                    plugin->params.push_back({});
                    param_ptr = &plugin->params.back();
                }
                param_ptr->param_id = msg->param_id;
                param_ptr->widget = msg->widget;
                param_ptr->name = msg->name;
                param_ptr->min_value = msg->min_value;
                param_ptr->max_value = msg->max_value;
                param_ptr->value = msg->value;
            }
        }

        rack.rx_buffer.erase(rack.rx_buffer.begin(), rack.rx_buffer.begin() + static_cast<long>(frame_size));
    }
}

static void draw_level_meter(const char* label, float level) {
    const float width = ImGui::GetContentRegionAvail().x - 36.0f;
    const float height = 10.0f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();

    draw->AddRectFilled(pos, {pos.x + width, pos.y + height}, IM_COL32(35, 35, 35, 255), 2.0f);

    const float filled = std::min(level, 1.0f) * width;
    if (filled > 0.0f) {
        const float g_end = std::min(filled, 0.70f * width);
        if (g_end > 0) {
            draw->AddRectFilled(pos, {pos.x + g_end, pos.y + height}, IM_COL32(50, 200, 50, 255), 2.0f);
        }

        const float y_start = 0.70f * width;
        const float y_end = std::min(filled, 0.90f * width);
        if (y_end > y_start) {
            draw->AddRectFilled({pos.x + y_start, pos.y}, {pos.x + y_end, pos.y + height}, IM_COL32(220, 200, 30, 255));
        }

        const float r_start = 0.90f * width;
        if (filled > r_start) {
            draw->AddRectFilled({pos.x + r_start, pos.y}, {pos.x + filled, pos.y + height}, IM_COL32(220, 60, 60, 255));
        }
    }

    draw->AddRect(pos, {pos.x + width, pos.y + height}, IM_COL32(80, 80, 80, 255), 2.0f);
    ImGui::Dummy({width, height});
    ImGui::SameLine(0, 6);
    ImGui::TextUnformatted(label);
}

int main(int, char* argv[]) {
    const std::string runner_binary = get_runner_binary_path(argv[0]);

    if (!glfwInit()) {
        return -1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(1280, 720, "Vessel UI", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return -1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    std::vector<std::unique_ptr<Rack>> racks;
    uint32_t next_rack_id = 1;
    auto add_rack = [&](const std::string& name) {
        auto rack = std::make_unique<Rack>();
        rack->id = next_rack_id++;
        rack->name = name;
        rack->runner_pid = spawn_runner(runner_binary, rack->id);
        rack->runner_alive = rack->runner_pid > 0;
        racks.emplace_back(std::move(rack));
    };

    add_rack("Main Rack");

    bool show_confirm_delete = false;
    int rack_to_delete = -1;

    bool show_rename = false;
    int rack_to_rename = -1;
    char rename_buf[vessel::kMaxNameLen + 1]{};

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        for (auto& rack : racks) {
            poll_runner_status(*rack);
            if (rack->runner_alive) {
                try_connect_socket(*rack);
            }
            rack->in_peak *= 0.90f;
            rack->out_peak *= 0.90f;
            drain_ipc(*rack);
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("Vessel")) {
                if (ImGui::MenuItem("Exit")) {
                    glfwSetWindowShouldClose(window, true);
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Racks")) {
                if (ImGui::MenuItem("Add New Rack")) {
                    add_rack("Rack " + std::to_string(racks.size() + 1));
                }

                ImGui::Separator();
                for (auto& rack : racks) {
                    ImGui::MenuItem(rack->name.c_str(), nullptr, &rack->visible);
                }
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        for (int r = 0; r < static_cast<int>(racks.size()); ++r) {
            Rack& rack = *racks[r];
            if (!rack.visible) {
                continue;
            }

            ImGui::Begin(rack.name.c_str(), &rack.visible);

            ImGui::Text("Runner PID: %d", rack.runner_pid);
            ImGui::SameLine();
            ImGui::TextColored(
                rack.runner_alive ? ImVec4(0.3f, 0.8f, 0.3f, 1.0f) : ImVec4(0.9f, 0.3f, 0.3f, 1.0f),
                rack.runner_alive ? "Alive" : "Offline");

            if (!rack.runner_alive && ImGui::Button("Restart Runner")) {
                rack.runner_pid = spawn_runner(runner_binary, rack.id);
                rack.runner_alive = rack.runner_pid > 0;
            }

            const bool connected = rack.sock_fd >= 0;
            ImGui::TextColored(
                connected ? ImVec4(0.3f, 0.8f, 0.3f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                connected ? "IPC Connected" : "IPC Disconnected");
            ImGui::Separator();

            if (ImGui::SliderFloat("Master Gain", &rack.master_gain, 0.0f, 2.0f)) {
                vessel::MsgSetGain msg;
                msg.gain = rack.master_gain;
                send_ipc(rack, msg);
            }

            bool bypassed = rack.bypassed_ui;
            if (ImGui::Checkbox("Bypass", &bypassed)) {
                rack.bypassed_ui = bypassed;
                vessel::MsgSetBypass msg;
                msg.bypassed = bypassed ? 1 : 0;
                send_ipc(rack, msg);
            }

            draw_level_meter("In", rack.in_peak);
            draw_level_meter("Out", rack.out_peak);
            ImGui::Separator();

            if (ImGui::Button("+ Add Plugin")) {
                rack.available_plugins.clear();
                for (size_t i = 0; i < vessel::kDefaultPluginCount; ++i) {
                    const vessel::PluginManifestEntry& entry = vessel::kDefaultPlugins[i];
                    rack.available_plugins.push_back({entry.plugin_type_id, entry.name});
                }
                rack.open_plugin_browser = true;
            }

            std::string popup_name = "Plugin Browser##" + std::to_string(rack.id);
            if (rack.open_plugin_browser) {
                ImGui::OpenPopup(popup_name.c_str());
                rack.open_plugin_browser = false;
            }

            if (ImGui::BeginPopupModal(popup_name.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextUnformatted("Available Plugins");
                ImGui::Separator();

                for (const auto& plugin_type : rack.available_plugins) {
                    if (ImGui::Selectable(plugin_type.name.c_str())) {
                        vessel::MsgAddPlugin msg;
                        msg.plugin_type_id = plugin_type.plugin_type_id;
                        send_ipc(rack, msg);
                        ImGui::CloseCurrentPopup();
                    }
                }

                if (rack.available_plugins.empty()) {
                    ImGui::TextUnformatted("No plugins listed in plugins.h manifest.");
                }

                ImGui::Separator();
                if (ImGui::Button("Close", ImVec2(120, 0))) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            ImGui::Separator();
            ImGui::TextUnformatted("Plugins");

            uint32_t pending_move_instance = 0;
            int pending_move_target_index = -1;

            for (auto pit = rack.plugins.begin(); pit != rack.plugins.end();) {
                RackPluginInstance& plugin = *pit;
                const int plugin_index = static_cast<int>(pit - rack.plugins.begin());
                ImGui::PushID(static_cast<int>(plugin.instance_id));

                bool remove_requested = false;
                bool plugin_bypassed = plugin.bypassed;
                if (ImGui::Checkbox("##plugin_bypass", &plugin_bypassed)) {
                    plugin.bypassed = plugin_bypassed;
                    vessel::MsgSetPluginBypass msg;
                    msg.instance_id = plugin.instance_id;
                    msg.bypassed = plugin_bypassed ? 1 : 0;
                    send_ipc(rack, msg);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Bypass plugin");
                }
                ImGui::SameLine();

                const bool header_open = ImGui::CollapsingHeader(plugin.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen);

                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoDisableHover)) {
                    const uint32_t dragged_instance_id = plugin.instance_id;
                    ImGui::SetDragDropPayload("VESSEL_PLUGIN", &dragged_instance_id, sizeof(dragged_instance_id));
                    ImGui::Text("Move: %s", plugin.name.c_str());
                    ImGui::EndDragDropSource();
                }

                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("VESSEL_PLUGIN")) {
                        if (payload->DataSize == static_cast<int>(sizeof(uint32_t))) {
                            pending_move_instance = *static_cast<const uint32_t*>(payload->Data);
                            pending_move_target_index = plugin_index;
                        }
                    }
                    ImGui::EndDragDropTarget();
                }

                if (ImGui::Button("Remove Plugin")) {
                    vessel::MsgRemovePlugin msg;
                    msg.instance_id = plugin.instance_id;
                    send_ipc(rack, msg);
                    remove_requested = true;
                }

                if (header_open) {
                    for (auto& param : plugin.params) {
                        ImGui::PushID(static_cast<int>(param.param_id));

                        if (param.widget == vessel::ParamWidget::SLIDER) {
                            float v = param.value;
                            if (ImGui::SliderFloat(param.name.c_str(), &v, param.min_value, param.max_value)) {
                                param.value = v;
                                vessel::MsgSetPluginParam msg;
                                msg.instance_id = plugin.instance_id;
                                msg.param_id = param.param_id;
                                msg.value = v;
                                send_ipc(rack, msg);
                            }
                        } else if (param.widget == vessel::ParamWidget::TOGGLE) {
                            bool b = param.value > 0.5f;
                            if (ImGui::Checkbox(param.name.c_str(), &b)) {
                                param.value = b ? 1.0f : 0.0f;
                                vessel::MsgSetPluginParam msg;
                                msg.instance_id = plugin.instance_id;
                                msg.param_id = param.param_id;
                                msg.value = param.value;
                                send_ipc(rack, msg);
                            }
                        } else if (param.widget == vessel::ParamWidget::BUTTON) {
                            if (ImGui::Button(param.name.c_str())) {
                                vessel::MsgSetPluginParam msg;
                                msg.instance_id = plugin.instance_id;
                                msg.param_id = param.param_id;
                                msg.value = 1.0f;
                                send_ipc(rack, msg);
                            }
                        }

                        ImGui::PopID();
                    }
                }

                ImGui::PopID();

                if (remove_requested) {
                    pit = rack.plugins.erase(pit);
                } else {
                    ++pit;
                }
            }

            if (pending_move_instance != 0 && pending_move_target_index >= 0) {
                if (move_plugin_instance(rack.plugins, pending_move_instance, static_cast<size_t>(pending_move_target_index))) {
                    vessel::MsgMovePlugin msg;
                    msg.instance_id = pending_move_instance;
                    msg.target_index = static_cast<uint32_t>(pending_move_target_index);
                    send_ipc(rack, msg);
                }
            }

            if (ImGui::BeginPopupContextWindow()) {
                if (ImGui::MenuItem("Rename Rack")) {
                    show_rename = true;
                    rack_to_rename = r;
                    std::strncpy(rename_buf, rack.name.c_str(), vessel::kMaxNameLen);
                    rename_buf[vessel::kMaxNameLen] = '\0';
                }
                if (ImGui::MenuItem("Delete Rack")) {
                    show_confirm_delete = true;
                    rack_to_delete = r;
                }
                ImGui::EndPopup();
            }

            ImGui::End();
        }

        if (show_rename) {
            ImGui::OpenPopup("Rename Rack");
        }

        if (ImGui::BeginPopupModal("Rename Rack", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("New name:");
            if (show_rename) ImGui::SetKeyboardFocusHere();
            show_rename = false;
            ImGui::SetNextItemWidth(300.0f);
            const bool entered = ImGui::InputText("##rename", rename_buf, sizeof(rename_buf), ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::Separator();
            const bool ok_clicked = ImGui::Button("OK", ImVec2(120, 0));
            if (entered || ok_clicked) {
                rename_buf[vessel::kMaxNameLen] = '\0';
                if (rename_buf[0] != '\0' && rack_to_rename >= 0 && rack_to_rename < static_cast<int>(racks.size())) {
                    racks[rack_to_rename]->name = rename_buf;
                }
                rack_to_rename = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemDefaultFocus();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                rack_to_rename = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (show_confirm_delete) {
            ImGui::OpenPopup("Confirm Delete?");
        }

        if (ImGui::BeginPopupModal("Confirm Delete?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Are you sure you want to delete this rack?\nThis cannot be undone.");
            ImGui::Separator();

            if (ImGui::Button("OK", ImVec2(120, 0))) {
                if (rack_to_delete >= 0 && rack_to_delete < static_cast<int>(racks.size())) {
                    terminate_runner(*racks[rack_to_delete]);
                    racks.erase(racks.begin() + rack_to_delete);
                    rack_to_delete = -1;
                }
                show_confirm_delete = false;
                ImGui::CloseCurrentPopup();
            }

            ImGui::SetItemDefaultFocus();
            ImGui::SameLine();

            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                show_confirm_delete = false;
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }

        ImGui::Render();
        glClearColor(0.06f, 0.06f, 0.08f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            GLFWwindow* backup_current_context = glfwGetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            glfwMakeContextCurrent(backup_current_context);
        }

        glfwSwapBuffers(window);
    }

    for (auto& rack : racks) {
        terminate_runner(*rack);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
