#include <algorithm>
#include <csignal>
#include <cstdlib>
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

#include "protocol.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

struct RackPlugin {
    std::string name;
    bool is_open = true;
    float volume = 1.0f;
    bool bypassed = false;
};

struct Rack {
    std::string name;
    bool visible = true;
    bool runner_alive = false;
    pid_t runner_pid = -1;

    int sock_fd = -1;
    float master_gain = 1.0f;
    bool bypassed_ui = false;

    std::vector<RackPlugin> plugins;

    ~Rack() {
        if (sock_fd >= 0) {
            ::close(sock_fd);
            sock_fd = -1;
        }
    }
};

pid_t spawn_runner(const std::string& runner_binary, const std::string& rack_name) {
    pid_t pid = fork();
    if (pid == 0) {
        execl(runner_binary.c_str(), runner_binary.c_str(), "--name", rack_name.c_str(), nullptr);
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
    const std::string path = vessel::socket_path(rack.name);
    ::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
        ::fcntl(fd, F_SETFL, O_NONBLOCK);
        rack.sock_fd = fd;
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
    auto add_rack = [&](const std::string& name) {
        auto rack = std::make_unique<Rack>();
        rack->name = name;
        rack->runner_pid = spawn_runner(runner_binary, rack->name);
        rack->runner_alive = rack->runner_pid > 0;
        racks.emplace_back(std::move(rack));
    };

    add_rack("Main Rack");

    bool show_confirm_delete = false;
    int rack_to_delete = -1;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        for (auto& rack : racks) {
            poll_runner_status(*rack);
            if (rack->runner_alive) {
                try_connect_socket(*rack);
            }
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
            if (!racks[r]->visible) {
                continue;
            }

            ImGui::Begin(racks[r]->name.c_str(), &racks[r]->visible);

            ImGui::Text("Runner PID: %d", racks[r]->runner_pid);
            ImGui::SameLine();
            ImGui::TextColored(
                racks[r]->runner_alive ? ImVec4(0.3f, 0.8f, 0.3f, 1.0f) : ImVec4(0.9f, 0.3f, 0.3f, 1.0f),
                racks[r]->runner_alive ? "Alive" : "Offline");

            if (!racks[r]->runner_alive && ImGui::Button("Restart Runner")) {
                racks[r]->runner_pid = spawn_runner(runner_binary, racks[r]->name);
                racks[r]->runner_alive = racks[r]->runner_pid > 0;
            }

            const bool connected = racks[r]->sock_fd >= 0;
            ImGui::TextColored(
                connected ? ImVec4(0.3f, 0.8f, 0.3f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                connected ? "IPC Connected" : "IPC Disconnected");
            ImGui::Separator();

            if (ImGui::SliderFloat("Master Gain", &racks[r]->master_gain, 0.0f, 2.0f)) {
                vessel::MsgSetGain msg;
                msg.gain = racks[r]->master_gain;
                send_ipc(*racks[r], msg);
            }

            bool bypassed = racks[r]->bypassed_ui;
            if (ImGui::Checkbox("Bypass", &bypassed)) {
                racks[r]->bypassed_ui = bypassed;
                vessel::MsgSetBypass msg;
                msg.bypassed = bypassed ? 1 : 0;
                send_ipc(*racks[r], msg);
            }
            ImGui::Separator();

            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PLUGIN_MOVE")) {
                    int* data = static_cast<int*>(payload->Data);
                    const int src_rack = data[0];
                    const int src_idx = data[1];

                    RackPlugin moved_plugin = racks[src_rack]->plugins[src_idx];
                    racks[src_rack]->plugins.erase(racks[src_rack]->plugins.begin() + src_idx);
                    racks[r]->plugins.push_back(moved_plugin);
                }
                ImGui::EndDragDropTarget();
            }

            if (ImGui::BeginPopupContextWindow()) {
                if (ImGui::MenuItem("Delete Rack")) {
                    show_confirm_delete = true;
                    rack_to_delete = r;
                }
                ImGui::EndPopup();
            }

            for (int p = 0; p < static_cast<int>(racks[r]->plugins.size()); ++p) {
                auto& plugin = racks[r]->plugins[p];
                ImGui::PushID(p);

                bool style_pushed = false;
                if (plugin.bypassed) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
                    style_pushed = true;
                }

                if (ImGui::Button(plugin.bypassed ? "B" : "P", ImVec2(25, 0))) {
                    plugin.bypassed = !plugin.bypassed;
                }

                if (style_pushed) {
                    ImGui::PopStyleColor();
                }

                ImGui::SameLine();

                const bool header_open = ImGui::CollapsingHeader(plugin.name.c_str(), ImGuiTreeNodeFlags_AllowOverlap);

                if (ImGui::BeginDragDropSource()) {
                    int payload_data[2] = {r, p};
                    ImGui::SetDragDropPayload("PLUGIN_MOVE", payload_data, sizeof(int) * 2);
                    ImGui::Text("Moving %s", plugin.name.c_str());
                    ImGui::EndDragDropSource();
                }

                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PLUGIN_MOVE")) {
                        int* data = static_cast<int*>(payload->Data);
                        const int src_rack = data[0];
                        const int src_idx = data[1];

                        RackPlugin moved_plugin = racks[src_rack]->plugins[src_idx];
                        racks[src_rack]->plugins.erase(racks[src_rack]->plugins.begin() + src_idx);

                        const int insert_at = p;
                        racks[r]->plugins.insert(racks[r]->plugins.begin() + insert_at, moved_plugin);
                    }
                    ImGui::EndDragDropTarget();
                }

                if (header_open) {
                    ImGui::Indent();
                    ImGui::SliderFloat("Volume", &plugin.volume, 0.0f, 1.0f);
                    if (ImGui::Button("Remove Plugin")) {
                        racks[r]->plugins.erase(racks[r]->plugins.begin() + p);
                    }
                    ImGui::Unindent();
                }

                ImGui::PopID();
            }

            if (ImGui::Button("+ Add Plugin")) {
                racks[r]->plugins.push_back({"New Plugin", true, 1.0f, false});
            }

            ImGui::End();
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
