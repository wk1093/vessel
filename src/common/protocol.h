#pragma once

#include <cctype>
#include <cstdint>
#include <string>

namespace vessel {

// Returns the Unix domain socket path for a named rack.
// The rack name is sanitized so the path is filesystem-safe.
inline std::string socket_path(const std::string& rack_name) {
    std::string sanitized;
    sanitized.reserve(rack_name.size());
    for (char c : rack_name) {
        sanitized += (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
    }
    return "/tmp/vessel-" + sanitized + ".sock";
}

enum class MsgType : uint8_t {
    SET_GAIN   = 0x01,
    SET_BYPASS = 0x02,
};

// Every message begins with this 2-byte header.
// `size` is the total byte count of the full message (header + payload).
struct __attribute__((packed)) MsgHeader {
    MsgType type;
    uint8_t size;
};

struct __attribute__((packed)) MsgSetGain {
    MsgHeader hdr{MsgType::SET_GAIN, sizeof(MsgSetGain)};
    float gain{1.0f};
};

struct __attribute__((packed)) MsgSetBypass {
    MsgHeader hdr{MsgType::SET_BYPASS, sizeof(MsgSetBypass)};
    uint8_t bypassed{0};
};

}  // namespace vessel
