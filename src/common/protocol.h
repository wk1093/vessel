#pragma once

#include <cstdint>
#include <string>

namespace vessel {

// Returns the Unix domain socket path for a rack identified by a stable integer ID.
inline std::string socket_path_by_id(uint32_t id) {
    return "/tmp/vessel-rack-" + std::to_string(id) + ".sock";
}

static constexpr uint32_t kMaxNameLen = 63;  // excluding null terminator; used by GUI for rack display names
static constexpr uint32_t kMaxPluginNameLen = 63;
static constexpr uint32_t kMaxParamNameLen = 47;
static constexpr uint32_t kMaxFilePathLen = 191;

enum class MsgType : uint8_t {
    SET_GAIN             = 0x01,
    SET_BYPASS           = 0x02,
    PEAK_LEVELS          = 0x03,  // runner -> GUI
    REQ_PLUGIN_CATALOG   = 0x10,  // GUI -> runner
    PLUGIN_CATALOG_ENTRY = 0x11,  // runner -> GUI
    ADD_PLUGIN           = 0x12,  // GUI -> runner
    PLUGIN_INSTANCE_ADDED = 0x13, // runner -> GUI
    REQ_PLUGIN_PARAMS    = 0x14,  // GUI -> runner
    PLUGIN_PARAM_DESC    = 0x15,  // runner -> GUI
    SET_PLUGIN_PARAM     = 0x16,  // GUI -> runner
    REMOVE_PLUGIN        = 0x17,  // GUI -> runner
    SET_PLUGIN_BYPASS    = 0x18,  // GUI -> runner
    MOVE_PLUGIN          = 0x19,  // GUI -> runner
    REQ_LV2_CATALOG      = 0x1A,  // GUI -> runner
    LV2_CATALOG_ENTRY    = 0x1B,  // runner -> GUI
    OPEN_PLUGIN_UI       = 0x1C,  // GUI -> runner
    PLUGIN_UI_STATE      = 0x1D,  // runner -> GUI
    PLUGIN_PARAM_ENUM_OPTION = 0x1E, // runner -> GUI
    SAVE_RACK_TO_FILE    = 0x1F,  // GUI -> runner
    LOAD_RACK_FROM_FILE  = 0x20,  // GUI -> runner
    SAVE_PLUGIN_PRESET   = 0x21,  // GUI -> runner
    LOAD_PLUGIN_PRESET   = 0x22,  // GUI -> runner
    RACK_STATE_RESET     = 0x23,  // runner -> GUI
    PLUGIN_CUSTOM_CONTROL = 0x24, // runner -> GUI
    TRIGGER_PLUGIN_CUSTOM_ACTION = 0x25, // GUI -> runner
    SET_PLUGIN_CUSTOM_TEXT = 0x26, // GUI -> runner
    PLUGIN_PARAMS_RESET  = 0x27,  // runner -> GUI
};

enum class ParamWidget : uint8_t {
    SLIDER = 0,
    BUTTON = 1,
    TOGGLE = 2,
    KNOB = 3,
    VSLIDER = 4,
    DRAG = 5,
};

enum class ParamValueType : uint8_t {
    FLOAT = 0,
    INT = 1,
    BOOL = 2,
    ENUM = 3,
};

enum class ParamLayoutHint : uint8_t {
    AUTO = 0,
    SAME_LINE = 1,
};

enum class CustomControlTextMode : uint8_t {
    NONE = 0,
    FILE_PATH = 1,
    PLAIN_TEXT = 2,
};

enum ParamFlags : uint8_t {
    PARAM_FLAG_NONE = 0,
    PARAM_FLAG_LOGARITHMIC = 1u << 0,
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

struct __attribute__((packed)) MsgReqPluginCatalog {
    MsgHeader hdr{MsgType::REQ_PLUGIN_CATALOG, sizeof(MsgReqPluginCatalog)};
};

struct __attribute__((packed)) MsgPluginCatalogEntry {
    MsgHeader hdr{MsgType::PLUGIN_CATALOG_ENTRY, sizeof(MsgPluginCatalogEntry)};
    uint32_t plugin_type_id{0};
    char name[kMaxPluginNameLen + 1]{};
    uint8_t is_last{0};
};

struct __attribute__((packed)) MsgAddPlugin {
    MsgHeader hdr{MsgType::ADD_PLUGIN, sizeof(MsgAddPlugin)};
    uint32_t plugin_type_id{0};
};

struct __attribute__((packed)) MsgPluginInstanceAdded {
    MsgHeader hdr{MsgType::PLUGIN_INSTANCE_ADDED, sizeof(MsgPluginInstanceAdded)};
    uint32_t instance_id{0};
    uint32_t plugin_type_id{0};
    char name[kMaxPluginNameLen + 1]{};
    uint8_t has_custom_ui{0};
};

struct __attribute__((packed)) MsgReqPluginParams {
    MsgHeader hdr{MsgType::REQ_PLUGIN_PARAMS, sizeof(MsgReqPluginParams)};
    uint32_t instance_id{0};
};

struct __attribute__((packed)) MsgPluginParamDesc {
    MsgHeader hdr{MsgType::PLUGIN_PARAM_DESC, sizeof(MsgPluginParamDesc)};
    uint32_t instance_id{0};
    uint32_t param_id{0};
    ParamWidget widget{ParamWidget::SLIDER};
    ParamValueType value_type{ParamValueType::FLOAT};
    uint8_t flags{PARAM_FLAG_NONE};
    ParamLayoutHint layout{ParamLayoutHint::AUTO};
    float ui_width{0.0f};
    float min_value{0.0f};
    float max_value{1.0f};
    float value{0.0f};
    char name[kMaxParamNameLen + 1]{};
    uint8_t is_last{0};
};

struct __attribute__((packed)) MsgSetPluginParam {
    MsgHeader hdr{MsgType::SET_PLUGIN_PARAM, sizeof(MsgSetPluginParam)};
    uint32_t instance_id{0};
    uint32_t param_id{0};
    float value{0.0f};
};

struct __attribute__((packed)) MsgRemovePlugin {
    MsgHeader hdr{MsgType::REMOVE_PLUGIN, sizeof(MsgRemovePlugin)};
    uint32_t instance_id{0};
};

struct __attribute__((packed)) MsgSetPluginBypass {
    MsgHeader hdr{MsgType::SET_PLUGIN_BYPASS, sizeof(MsgSetPluginBypass)};
    uint32_t instance_id{0};
    uint8_t bypassed{0};
};

struct __attribute__((packed)) MsgMovePlugin {
    MsgHeader hdr{MsgType::MOVE_PLUGIN, sizeof(MsgMovePlugin)};
    uint32_t instance_id{0};
    uint32_t target_index{0};
};

struct __attribute__((packed)) MsgReqLv2Catalog {
    MsgHeader hdr{MsgType::REQ_LV2_CATALOG, sizeof(MsgReqLv2Catalog)};
};

struct __attribute__((packed)) MsgLv2CatalogEntry {
    MsgHeader hdr{MsgType::LV2_CATALOG_ENTRY, sizeof(MsgLv2CatalogEntry)};
    uint32_t plugin_type_id{0};
    char name[kMaxPluginNameLen + 1]{};
    uint8_t is_last{0};
};

struct __attribute__((packed)) MsgOpenPluginUi {
    MsgHeader hdr{MsgType::OPEN_PLUGIN_UI, sizeof(MsgOpenPluginUi)};
    uint32_t instance_id{0};
};

struct __attribute__((packed)) MsgPluginUiState {
    MsgHeader hdr{MsgType::PLUGIN_UI_STATE, sizeof(MsgPluginUiState)};
    uint32_t instance_id{0};
    uint8_t is_open{0};
};

struct __attribute__((packed)) MsgPluginParamEnumOption {
    MsgHeader hdr{MsgType::PLUGIN_PARAM_ENUM_OPTION, sizeof(MsgPluginParamEnumOption)};
    uint32_t instance_id{0};
    uint32_t param_id{0};
    int32_t enum_value{0};
    char label[kMaxParamNameLen + 1]{};
    uint8_t is_last{0};
};

struct __attribute__((packed)) MsgSaveRackToFile {
    MsgHeader hdr{MsgType::SAVE_RACK_TO_FILE, sizeof(MsgSaveRackToFile)};
    char path[kMaxFilePathLen + 1]{};
};

struct __attribute__((packed)) MsgLoadRackFromFile {
    MsgHeader hdr{MsgType::LOAD_RACK_FROM_FILE, sizeof(MsgLoadRackFromFile)};
    char path[kMaxFilePathLen + 1]{};
};

struct __attribute__((packed)) MsgSavePluginPreset {
    MsgHeader hdr{MsgType::SAVE_PLUGIN_PRESET, sizeof(MsgSavePluginPreset)};
    uint32_t instance_id{0};
    char path[kMaxFilePathLen + 1]{};
};

struct __attribute__((packed)) MsgLoadPluginPreset {
    MsgHeader hdr{MsgType::LOAD_PLUGIN_PRESET, sizeof(MsgLoadPluginPreset)};
    uint32_t instance_id{0};
    char path[kMaxFilePathLen + 1]{};
};

struct __attribute__((packed)) MsgRackStateReset {
    MsgHeader hdr{MsgType::RACK_STATE_RESET, sizeof(MsgRackStateReset)};
};

struct __attribute__((packed)) MsgPluginCustomControl {
    MsgHeader hdr{MsgType::PLUGIN_CUSTOM_CONTROL, sizeof(MsgPluginCustomControl)};
    uint32_t instance_id{0};
    uint32_t action_id{0};
    CustomControlTextMode text_mode{CustomControlTextMode::NONE};
    ParamLayoutHint layout{ParamLayoutHint::AUTO};
    float ui_width{0.0f};
    char label[kMaxParamNameLen + 1]{};
    char text_value[kMaxParamNameLen + 1]{};
    uint8_t is_last{0};
};

struct __attribute__((packed)) MsgTriggerPluginCustomAction {
    MsgHeader hdr{MsgType::TRIGGER_PLUGIN_CUSTOM_ACTION, sizeof(MsgTriggerPluginCustomAction)};
    uint32_t instance_id{0};
    uint32_t action_id{0};
};

struct __attribute__((packed)) MsgSetPluginCustomText {
    MsgHeader hdr{MsgType::SET_PLUGIN_CUSTOM_TEXT, sizeof(MsgSetPluginCustomText)};
    uint32_t instance_id{0};
    uint32_t action_id{0};
    char text[kMaxFilePathLen + 1]{};
};

struct __attribute__((packed)) MsgPluginParamsReset {
    MsgHeader hdr{MsgType::PLUGIN_PARAMS_RESET, sizeof(MsgPluginParamsReset)};
    uint32_t instance_id{0};
};

}  // namespace vessel
