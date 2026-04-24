#pragma once

#include <cstdint>
#include <string>

namespace vessel {

// Returns the Unix domain socket path for a rack identified by a stable integer ID.
inline std::string socket_path_by_id(uint32_t id) {
    return "/tmp/vessel-rack-" + std::to_string(id) + ".sock";
}

static constexpr uint32_t kMaxNameLen = 63;  // excluding null terminator; used by GUI for rack display names

enum class MsgType : uint8_t {
    SET_GAIN    = 0x01,
    SET_BYPASS  = 0x02,
    PEAK_LEVELS = 0x03,  // runner -> GUI
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

struct __attribute__((packed)) MsgPeakLevels {
    MsgHeader hdr{MsgType::PEAK_LEVELS, sizeof(MsgPeakLevels)};
    float in_peak{0.0f};
    float out_peak{0.0f};
};

}  // namespace vessel
