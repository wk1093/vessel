# Vessel: System Architecture & Technical Design

This document outlines the architectural strategy for **Vessel**, a low-latency audio plugin host designed for Arch Linux. It details the transition from a monolithic GUI to a distributed, multi-executable system optimized for performance and reliability.

## 1. The Multi-Executable Architecture

Vessel follows a **"Manager-Worker"** pattern. The system is split into two distinct roles: **The Orchestrator (GUI)** and **The Runners (Audio Hosts)**. This ensures that high-priority audio processing is never interrupted by UI rendering or user input latency.

### A. Vessel-UI (The Controller)
* **Technology:** C++, Dear ImGui (Docking Branch), GLFW.
* **Responsibility:** Visualizing rack state, managing plugin drag-and-drop, and providing a front-end for PipeWire routing.
* **Lifecycle Management:** Spawns a new `vessel-runner` process whenever a user adds a rack. It monitors these child processes and handles Inter-Process Communication (IPC).

### B. Vessel-Runner (The DSP Engine)
* **Technology:** C++, libpipewire, spa (Simple Plugin API).
* **Responsibility:** Maintaining a single real-time PipeWire filter. It runs headless and executes the actual audio processing loop.
* **Performance:** Runs with RT (Real-Time) priority. It is designed to be lean, with zero memory allocations in the processing callback.

---

## 2. The "Black Box" Audio Strategy

In PipeWire, we represent each Rack as a single, atomic node. This is the **Black Box Approach**. Instead of exposing every plugin as a separate node in the system graph, the Runner handles them internally.

* **Cache Locality:** Audio buffers stay in the CPU cache as they pass from plugin to plugin.
* **Reduced Context Switching:** The kernel only needs to schedule one process per rack, rather than dozens of individual plugin nodes.
* **Determinism:** The order of processing is explicitly defined by the internal C++ vector, preventing unpredictable graph traversal.

### Internal Processing Flow
The `on_process` callback in the Runner receives a buffer from PipeWire. It then iterates through an internal list of loaded plugins (LV2, C Alloy, or built-in DSP):

`Buffer In` $\rightarrow$ `Plugin 1` $\rightarrow$ `Plugin 2` $\rightarrow$ `...` $\rightarrow$ `Plugin N` $\rightarrow$ `Buffer Out`

---

## 3. Inter-Process Communication (IPC)

Communication between the GUI and the Runners is split into two channels to optimize for speed and reliability.

### Control Channel (Unix Domain Sockets)
Used for discrete events. When a user moves a slider or toggles a bypass, the GUI sends a small binary packet to the specific Runner's socket.
* `SET_PARAM [plugin_id] [value]`
* `LOAD_PLUGIN [uri]`
* `BYPASS_RACK [bool]`

### Telemetry Channel (Shared Memory / SHM)
Used for high-frequency data like audio meters. Sending socket messages 60 times a second for every rack is inefficient. 
* The **Runner** writes the current `Peak Level` to a shared memory segment.
* The **GUI** reads this value whenever it renders a frame to update the `ImGui::ProgressBar`.

---

## 4. Fault Tolerance & Sandboxing

By utilizing this multi-process structure, Vessel achieves a level of stability impossible in monolithic hosts:

1.  **Plugin Isolation:** A segfault in a custom-coded C Alloy plugin only kills the `vessel-runner` process for that rack. Other racks and the GUI stay alive.
2.  **UI Responsiveness:** Even if the audio engine is under heavy load (100% RT CPU), the GUI remains fluid because it is a separate process scheduled differently by the Linux kernel.
3.  **Hot Reloading:** The GUI can tell a Runner to restart or reload its DSP logic (useful for development) without needing to restart the entire application.

---

## 5. Development Roadmap
* [ ] Implement Unix Domain Socket bridge.
* [ ] Create headless `vessel-runner` template.
* [ ] Integrate `shmget`/`shmat` for cross-process metering.
* [ ] Add dynamic `pw-link` orchestration within the GUI manager.

---

## 6. Current Source Layout (Transition Snapshot)

The repository now follows the initial split architecture:

* `src/gui/main.cpp` builds `vessel-ui` (orchestrator process with ImGui + GLFW).
* `src/rackhost/main.cpp` builds `vessel-runner` (headless PipeWire rack host).

Current behavior during this transition:

* `vessel-ui` spawns one `vessel-runner` process per rack and monitors lifecycle (PID/alive state).
* Rack input/output linking is still done through `pw-link` commands from the UI.
* IPC control socket and SHM telemetry are intentionally stubbed as TODO while the executable split stabilizes.