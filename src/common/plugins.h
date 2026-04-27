#pragma once

#include <cstddef>
#include <cstdint>

namespace vessel {

struct PluginManifestEntry {
    uint32_t plugin_type_id;
    const char* name;
    bool is_builtin;
    const char* lv2_uri;
};

inline constexpr PluginManifestEntry kDefaultPlugins[] = {
    {1, "Sine Mixer (Test)", true, ""},
    {2, "Gain", true, ""},
    {3, "Soft Clip", true, ""},
    {4, "Soundboard", true, ""},
};

inline constexpr size_t kDefaultPluginCount = sizeof(kDefaultPlugins) / sizeof(kDefaultPlugins[0]);

inline const PluginManifestEntry* find_plugin_manifest_entry(uint32_t plugin_type_id) {
    for (size_t i = 0; i < kDefaultPluginCount; ++i) {
        if (kDefaultPlugins[i].plugin_type_id == plugin_type_id) {
            return &kDefaultPlugins[i];
        }
    }
    return nullptr;
}

}  // namespace vessel
