#pragma once

#include "d3renderstream.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <vector>

namespace rs {
namespace schema {

// Serializes a Disguise schema JSON into a single contiguous buffer matching
// the memory model of rs_getStreams: one allocation, zero internal heap
// pointers.  Callers own the buffer and free it in one operation.
//
// Usage:
//   Serializer ser;
//   size_t sz = ser.Measure(json); // Pass 1 — compute required size
//   std::vector<uint8_t> buf(sz);
//   Schema* s = ser.Write(json, buf.data(), sz); // Pass 2 — fill
class Serializer {
public:
    // Walk |json| and return the minimum buffer size in bytes needed by
    // Write().  Returns 0 on error (invalid JSON structure).
    size_t Measure(const nlohmann::json& json);

    // Fill |buffer| (≥ Measure() bytes) with a fully self-contained Schema
    // tree.  Returns a pointer to the Schema at the start of the buffer, or
    // nullptr if |buffer| is null, |buffer_size| is too small, or a logic
    // inconsistency is detected.
    Schema* Write(const nlohmann::json& json, void* buffer, size_t buffer_size);

private:
    static constexpr size_t kNullOff = SIZE_MAX;

    static size_t Align(size_t offset, size_t alignment);

    // Measures one string: returns the offset where it will be placed, or
    // kNullOff for empty strings.  Advances |offset| past the string data.
    static size_t MeasureStr(const std::string& s, size_t& offset);

    // Copies |s| into |base[offset]| and sets |*ptr_field| pointing there.
    // If |offset| is kNullOff, |*ptr_field| is set to nullptr.
    static void WriteStr(uint8_t* base, const std::string& s, size_t offset, const char** ptr_field);

    // Carries byte offsets from Pass 1 (Measure) to Pass 2 (Write).
    struct Layout {
        size_t total_bytes = 0;

        size_t engine_name_off = kNullOff;
        size_t engine_version_off = kNullOff;
        size_t plugin_version_off = kNullOff;
        size_t info_off = kNullOff;

        uint32_t n_channels = 0;
        size_t channels_array_off = 0;
        std::vector<size_t> channel_str_offs;

        // Per-scene layouts
        struct Param {
            size_t group_off = kNullOff;
            size_t display_name_off = kNullOff;
            size_t key_off = kNullOff;
            size_t default_value_off = kNullOff;  // kNullOff for non-TEXT types
            uint32_t n_options = 0;
            size_t options_array_off = 0;
            std::vector<size_t> option_offs;
        };

        struct Scene {
            size_t name_off = kNullOff;
            uint32_t n_params = 0;
            size_t params_array_off = 0;
            std::vector<Param> params;
        };

        uint32_t n_scenes = 0;
        size_t scenes_array_off = 0;
        std::vector<Scene> scenes;
    };

    Layout layout_;
};

}  // namespace schema
}  // namespace rs
