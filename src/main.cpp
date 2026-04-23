#include <iostream>
#include <vector>
#include <string>
#include <algorithm>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <pipewire/pipewire.h>

struct RackPlugin {
    std::string name;
    bool is_open = true;
    float volume = 1.0f;
    bool bypassed = false;
};

struct Rack {
    std::string name;
    bool visible = true;
    
    // PipeWire Routing State
    std::string input_device_name = "None";
    std::string output_device_name = "None";
    uint32_t input_node_id = 0;
    uint32_t output_node_id = 0;

    std::vector<RackPlugin> plugins;
};

int main(int argc, char* argv[]) {
    pw_init(&argc, &argv);

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
    std::vector<Rack> racks;
    // Initialize with one default rack
    racks.push_back((Rack){"Main Rack", true, "None", "None", 0, 0, {{"Soundboard", true, 0.8f}, {"Compressor", true, 1.0f}}});

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
                    racks.push_back({"New Rack " + std::to_string(racks.size() + 1), true, {}});
                }
                ImGui::Separator();
                for (auto& rack : racks) {
                    ImGui::MenuItem(rack.name.c_str(), NULL, &rack.visible);
                }
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        // --- Render Racks ---
        for (int r = 0; r < (int)racks.size(); r++) {
            if (!racks[r].visible) continue;

            ImGui::Begin(racks[r].name.c_str(), &racks[r].visible);

            if (ImGui::CollapsingHeader("Rack Configuration", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Indent();

                // Input Selection
                if (ImGui::BeginCombo("Input Source", racks[r].input_device_name.c_str())) {
                    // In a real version, you'd iterate over found PipeWire nodes here
                    if (ImGui::Selectable("System Microphone")) { 
                        racks[r].input_device_name = "System Microphone"; 
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                if (ImGui::Button("Intercept In")) {
                    // TODO: Logic to link all outputs of selected node to this rack's input
                    std::cout << "Intercepting Input for " << racks[r].name << std::endl;
                }

                // Output Selection
                if (ImGui::BeginCombo("Output Sink", racks[r].output_device_name.c_str())) {
                    if (ImGui::Selectable("Built-in Audio Analog Stereo")) { 
                        racks[r].output_device_name = "Built-in Audio Analog Stereo"; 
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                if (ImGui::Button("Intercept Out")) {
                    // TODO: Logic to link this rack's output to the selected node's input
                    std::cout << "Intercepting Output for " << racks[r].name << std::endl;
                }

                ImGui::Unindent();
                ImGui::Separator();
            }

            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PLUGIN_MOVE")) {
                    int* data = (int*)payload->Data;
                    int src_rack = data[0], src_idx = data[1];

                    RackPlugin moved_plugin = racks[src_rack].plugins[src_idx];
                    racks[src_rack].plugins.erase(racks[src_rack].plugins.begin() + src_idx);
                    racks[r].plugins.push_back(moved_plugin);
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

            for (int p = 0; p < (int)racks[r].plugins.size(); p++) {
                auto& plugin = racks[r].plugins[p];
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

                        RackPlugin moved_plugin = racks[src_rack].plugins[src_idx];
                        
                        // Erase from old position
                        racks[src_rack].plugins.erase(racks[src_rack].plugins.begin() + src_idx);
                        
                        // If moving within same rack and moving "down", adjust index
                        int insert_at = p;
                        if (src_rack == r && src_idx < p) {
                            // No adjustment needed because erase shifted items up
                        }
                        
                        racks[r].plugins.insert(racks[r].plugins.begin() + insert_at, moved_plugin);
                    }
                    ImGui::EndDragDropTarget();
                }

                if (header_open) {
                    ImGui::Indent();
                    ImGui::SliderFloat("Volume", &plugin.volume, 0.0f, 1.0f);
                    if (ImGui::Button("Remove Plugin")) {
                         racks[r].plugins.erase(racks[r].plugins.begin() + p);
                    }
                    ImGui::Unindent();
                }
                ImGui::PopID();
            }

            if (ImGui::Button("+ Add Plugin")) {
                racks[r].plugins.push_back({"New Plugin", true, 1.0f});
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
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    pw_deinit();

    return 0;
}