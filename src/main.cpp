#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <atomic>
#include <thread>
#include <memory>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <pipewire/pipewire.h>
#include <pipewire/filter.h>

std::vector<std::string> get_pw_ports(bool input) {
    std::vector<std::string> ports;
    // -i lists inputs (sources), -o lists outputs (sinks)
    FILE* pipe = popen(input ? "pw-link -i" : "pw-link -o", "r");
    if (!pipe) return ports;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        std::string port = buffer;
        port.erase(port.find_last_not_of(" \n\r\t") + 1); // trim
        // Filter out Vessel's own ports to avoid feedback loops
        if (port.find("Vessel") == std::string::npos) {
            ports.push_back(port);
        }
    }
    pclose(pipe);
    return ports;
}

// Shared state between UI and Audio thread
struct AudioState {
    std::atomic<float> volume{1.0f};
    std::atomic<bool> bypassed{false};
    std::atomic<float> in_peak{0.0f};
    std::atomic<float> out_peak{0.0f};

    AudioState() = default;

    // Explicitly handle the fact that atomics can't move
    AudioState(const AudioState&) = delete;
    AudioState& operator=(const AudioState&) = delete;
    AudioState(AudioState&&) = delete;
    AudioState& operator=(AudioState&&) = delete;
};

struct RackPlugin {
    std::string name;
    bool is_open = true;
    float volume = 1.0f;
    bool bypassed = false;
};

// Forward declaration of process callback
static void on_process(void *data, struct spa_io_position *pos);

static const struct pw_filter_events filter_events = {
    PW_VERSION_FILTER_EVENTS,
    .process = on_process,
};

struct Rack {
    std::string name;
    bool visible = true;
    
    // PipeWire Objects
    struct pw_filter *filter = nullptr;
    void *input_port = nullptr;
    void *output_port = nullptr;
    AudioState audio_state;

    // Routing UI state
    std::string input_device_name = "None";
    std::string output_device_name = "None";

    std::vector<RackPlugin> plugins;

    Rack(const std::string& name) : name(name) {

    }

    ~Rack() {
        if (filter) pw_filter_destroy(filter);
    }

    void connect_to_node(const std::string& target_name, bool is_input) {
        // In PipeWire filters, you can trigger a reconnection 
        // by updating the metadata or using pw_filter_connect with a target.
        // For now, let's use the simplest approach: manual linking via system call 
        // or pw-link while we're in PoC phase.
        
        std::string cmd = is_input ? 
            "pw-link \"" + target_name + "\" \"" + name + ":input\"" :
            "pw-link \"" + name + ":output\" \"" + target_name + "\"";
        
        system(cmd.c_str()); 
        // This is a "cheat" for the PoC, but it works instantly on Arch 
        // without writing 200 lines of registry listener code.
    }

    // Initialize the PipeWire filter for this rack
    void setup_audio(struct pw_loop *loop) {
        filter = pw_filter_new_simple(loop, name.c_str(), 
            pw_properties_new(
                PW_KEY_MEDIA_TYPE, "Audio", 
                PW_KEY_MEDIA_CATEGORY, "Filter",
                "node.name", name.c_str(), // Explicit node name for pw-link
                NULL),
            &filter_events, this);

        input_port = pw_filter_add_port(filter, PW_DIRECTION_INPUT, 
            PW_FILTER_PORT_FLAG_MAP_BUFFERS, sizeof(float), 
            pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono", PW_KEY_PORT_NAME, "input", NULL), 
            NULL, 0);

        output_port = pw_filter_add_port(filter, PW_DIRECTION_OUTPUT, 
            PW_FILTER_PORT_FLAG_MAP_BUFFERS, sizeof(float), 
            pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono", PW_KEY_PORT_NAME, "output", NULL), 
            NULL, 0);

        pw_filter_connect(filter, PW_FILTER_FLAG_RT_PROCESS, NULL, 0);
    }
};

// --- AUDIO THREAD CALLBACK ---
static void on_process(void *data, struct spa_io_position *pos) {
    Rack* rack = static_cast<Rack*>(data);
    
    // Use MAP_BUFFERS friendly dequeue
    struct pw_buffer *b_in = (struct pw_buffer *)pw_filter_dequeue_buffer(rack->input_port);
    struct pw_buffer *b_out = (struct pw_buffer *)pw_filter_dequeue_buffer(rack->output_port);

    if (!b_in || !b_out) return;

    float *in = (float *)b_in->buffer->datas[0].data;
    float *out = (float *)b_out->buffer->datas[0].data;
    
    // Calculate samples based on chunk size
    uint32_t n_samples = b_in->buffer->datas[0].chunk->size / sizeof(float);

    float p_in = 0.0f;
    float p_out = 0.0f;
    float vol = rack->audio_state.volume.load();

    for (uint32_t i = 0; i < n_samples; i++) {
        float s = in[i];
        if (std::abs(s) > p_in) p_in = std::abs(s);

        if (!rack->audio_state.bypassed.load()) {
            s *= vol;
        }

        out[i] = s;
        if (std::abs(s) > p_out) p_out = std::abs(s);
    }

    rack->audio_state.in_peak.store(p_in);
    rack->audio_state.out_peak.store(p_out);

    pw_filter_queue_buffer(rack->input_port, b_in);
    pw_filter_queue_buffer(rack->output_port, b_out);
}

int main(int argc, char* argv[]) {
    pw_init(&argc, &argv);
    struct pw_thread_loop *thread_loop = pw_thread_loop_new("VesselLoop", NULL);
    pw_thread_loop_start(thread_loop);

    if (!glfwInit()) return -1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(1280, 720, "Vessel", NULL, NULL);
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

    // Dynamic Rack Storage
    std::vector<std::unique_ptr<Rack>> racks;
    // Initialize with one default rack
    auto add_rack = [&](std::string name) {
        auto r = std::make_unique<Rack>(name);
        // CRITICAL: Lock the loop before touching any PipeWire objects
        pw_thread_loop_lock(thread_loop);
        r->setup_audio(pw_thread_loop_get_loop(thread_loop));
        pw_thread_loop_unlock(thread_loop);
        racks.emplace_back(std::move(r));
    };

    add_rack("Main Rack");

    bool show_confirm_delete = false;
    int rack_to_delete = -1;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("Vessel")) {
                if (ImGui::MenuItem("Exit")) glfwSetWindowShouldClose(window, true);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Racks")) {
                if (ImGui::MenuItem("Add New Rack")) {
                    add_rack("New Rack " + std::to_string(racks.size() + 1));
                }
                ImGui::Separator();
                for (auto& rack : racks) {
                    ImGui::MenuItem(rack->name.c_str(), NULL, &rack->visible);
                }
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        // --- Render Racks ---
        for (int r = 0; r < (int)racks.size(); r++) {
            if (!racks[r]->visible) continue;

            ImGui::Begin(racks[r]->name.c_str(), &racks[r]->visible);

            float in_val = racks[r]->audio_state.in_peak.load();
            float out_val = racks[r]->audio_state.out_peak.load();
            ImGui::Text("IN "); ImGui::SameLine(); ImGui::ProgressBar(in_val, ImVec2(-1, 12), "");
            ImGui::Text("OUT"); ImGui::SameLine(); ImGui::ProgressBar(out_val, ImVec2(-1, 12), "");
            float vol = racks[r]->audio_state.volume.load();
            if (ImGui::SliderFloat("Master Gain", &vol, 0.0f, 2.0f)) {
                racks[r]->audio_state.volume.store(vol);
            }
            ImGui::Separator();

            if (ImGui::CollapsingHeader("Rack Configuration", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Indent();

                // Input Selection
                if (ImGui::BeginCombo("Input Source", racks[r]->input_device_name.c_str())) {
                    auto inputs = get_pw_ports(true); // Get real system inputs
                    for (auto& in : inputs) {
                        if (ImGui::Selectable(in.c_str())) { racks[r]->input_device_name = in; }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                if (ImGui::Button("Intercept In")) {
                    // TODO: Logic to link all outputs of selected node to this rack's input
                    std::cout << "Intercepting Input for " << racks[r]->name << std::endl;
                    std::string cmd = "pw-link \"" + racks[r]->input_device_name + "\" \"" + racks[r]->name + ":input\"";
                    system(cmd.c_str());
                }

                // Output Selection
                if (ImGui::BeginCombo("Output Sink", racks[r]->output_device_name.c_str())) {
                    auto outputs = get_pw_ports(false); // Get real system outputs
                    for (auto& out : outputs) {
                        if (ImGui::Selectable(out.c_str())) { racks[r]->output_device_name = out; }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                if (ImGui::Button("Intercept Out")) {
                    // TODO: Logic to link this rack's output to the selected node's input
                    std::cout << "Intercepting Output for " << racks[r]->name << std::endl;
                    std::string cmd = "pw-link \"" + racks[r]->name + ":output\" \"" + racks[r]->output_device_name + "\"";
                    system(cmd.c_str());
                }

                ImGui::Unindent();
                ImGui::Separator();
            }

            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PLUGIN_MOVE")) {
                    int* data = (int*)payload->Data;
                    int src_rack = data[0], src_idx = data[1];

                    RackPlugin moved_plugin = racks[src_rack]->plugins[src_idx];
                    racks[src_rack]->plugins.erase(racks[src_rack]->plugins.begin() + src_idx);
                    racks[r]->plugins.push_back(moved_plugin);
                }
                ImGui::EndDragDropTarget();
            }
            
            // Right-click rack tab to delete
            if (ImGui::BeginPopupContextWindow()) {
                if (ImGui::MenuItem("Delete Rack")) {
                    show_confirm_delete = true;
                    rack_to_delete = r;
                }
                ImGui::EndPopup();
            }

            for (int p = 0; p < (int)racks[r]->plugins.size(); p++) {
                auto& plugin = racks[r]->plugins[p];
                ImGui::PushID(p);

                // --- Custom Header with Bypass Button ---
                // We use a small button before the header
                bool style_pushed = false;
                if (plugin.bypassed) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
                    style_pushed = true;
                }
                
                if (ImGui::Button(plugin.bypassed ? "B" : "P", ImVec2(25, 0))) {
                    plugin.bypassed = !plugin.bypassed;
                }
                
                if (style_pushed) ImGui::PopStyleColor();

                ImGui::SameLine();

                bool header_open = ImGui::CollapsingHeader(plugin.name.c_str(), ImGuiTreeNodeFlags_AllowOverlap);
                
                if (ImGui::BeginDragDropSource()) {
                    int payload_data[2] = { r, p };
                    ImGui::SetDragDropPayload("PLUGIN_MOVE", payload_data, sizeof(int) * 2);
                    ImGui::Text("Moving %s", plugin.name.c_str());
                    ImGui::EndDragDropSource();
                }

                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PLUGIN_MOVE")) {
                        int* data = (int*)payload->Data;
                        int src_rack = data[0], src_idx = data[1];

                        RackPlugin moved_plugin = racks[src_rack]->plugins[src_idx];
                        
                        // Erase from old position
                        racks[src_rack]->plugins.erase(racks[src_rack]->plugins.begin() + src_idx);
                        
                        // If moving within same rack and moving "down", adjust index
                        int insert_at = p;
                        if (src_rack == r && src_idx < p) {
                            // No adjustment needed because erase shifted items up
                        }
                        
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
                racks[r]->plugins.push_back({"New Plugin", true, 1.0f});
            }

            ImGui::End();
        }

        // --- Confirm Delete Popup ---
        if (show_confirm_delete) {
            ImGui::OpenPopup("Confirm Delete?");
        }

        if (ImGui::BeginPopupModal("Confirm Delete?", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Are you sure you want to delete this rack?\nThis cannot be undone.");
            ImGui::Separator();

            if (ImGui::Button("OK", ImVec2(120, 0))) {
                if (rack_to_delete != -1) {
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

        // Rendering Boilerplate
        ImGui::Render();
        glClearColor(0.06f, 0.06f, 0.08f, 1.00f);
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

    // Cleanup
    pw_thread_loop_stop(thread_loop);
    pw_thread_loop_destroy(thread_loop);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    pw_deinit();

    return 0;
}