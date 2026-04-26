cmake_minimum_required(VERSION 3.15)
project(vessel VERSION 0.1.0 LANGUAGES C CXX)

# Standard C++ for ImGui and future C Alloy integration
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Dependencies ---

# 1. GLFW via system package
find_package(glfw3 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(SPA REQUIRED IMPORTED_TARGET libspa-0.2)
pkg_check_modules(LILV REQUIRED IMPORTED_TARGET lilv-0)

# 2. ImGui (Docking Branch) via FetchContent
include(FetchContent)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
)
FetchContent_MakeAvailable(imgui)

# --- Project Structure ---

# Define the GLAD source and include paths
set(GLAD_SRC "${CMAKE_CURRENT_SOURCE_DIR}/vendor/glad/src/glad.c")
set(GLAD_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/vendor/glad/include")
set(IMGUI_SRC ${imgui_SOURCE_DIR}/imgui.cpp
              ${imgui_SOURCE_DIR}/imgui_draw.cpp
              ${imgui_SOURCE_DIR}/imgui_widgets.cpp
              ${imgui_SOURCE_DIR}/imgui_tables.cpp)

# --- Executables ---

add_executable(vessel-ui
    src/gui/main.cpp
    ${GLAD_SRC}
    ${IMGUI_SRC}
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

add_executable(vessel-runner
    src/rackhost/main.cpp
    src/rackhost/plugins_runtime.cpp
)

# Keep a convenient legacy target name.
add_executable(${PROJECT_NAME} ALIAS vessel-ui)

# --- Include Directories ---

target_include_directories(vessel-ui PRIVATE
    "include"
    "${GLAD_INCLUDE}"
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/common"
)

target_include_directories(vessel-runner PRIVATE
    "include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/common"
)

# --- Linking ---

target_link_libraries(vessel-ui PRIVATE
    glfw
)

target_link_libraries(vessel-runner PRIVATE
    PkgConfig::PIPEWIRE
    PkgConfig::SPA
    PkgConfig::LILV
)

# --- Compiler Optimizations ---
if(MSVC)
    target_compile_options(vessel-ui PRIVATE /W4)
    target_compile_options(vessel-runner PRIVATE /W4)
else()
    target_compile_options(vessel-ui PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(vessel-runner PRIVATE -Wall -Wextra -Wpedantic)
endif()