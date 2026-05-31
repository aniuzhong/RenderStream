#include "d3renderstream.h"
#include "schema_serializer.h"

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

json MakeMinimalJson() {
    json j = json::object();
    j["engineName"] = "TestEngine";
    j["engineVersion"] = "5.3";
    j["pluginVersion"] = "2.0";
    j["info"] = "Integration test";
    j["channels"] = {"chanA", "chanB"};

    json& sc0 = j["scenes"].emplace_back();
    sc0["name"] = "SceneOne";
    sc0["hash"] = 42ULL;

    json& p0 = sc0["parameters"].emplace_back();
    p0["group"] = "Transform";
    p0["displayName"] = "Speed";
    p0["key"] = "speed";
    p0["type"] = RS_PARAMETER_NUMBER;
    p0["min"] = 0.0;
    p0["max"] = 100.0;
    p0["step"] = 1.0;
    p0["defaultValue"] = 50.0;
    p0["options"] = json::array();
    p0["dmxOffset"] = -1;
    p0["dmxType"] = RS_DMX_DEFAULT;
    p0["flags"] = 0u;

    json& p1 = sc0["parameters"].emplace_back();
    p1["group"] = "Appearance";
    p1["displayName"] = "Label";
    p1["key"] = "label";
    p1["type"] = RS_PARAMETER_TEXT;
    p1["defaultValue"] = "hello";
    p1["options"] = {"A", "B", "C"};
    p1["dmxOffset"] = 5;
    p1["dmxType"] = RS_DMX_16_BE;
    p1["flags"] = 3u;

    return j;
}

// Returns true when every const-char* reachable from the Schema tree falls
// inside [buffer_begin, buffer_end).
bool AllPointersInBuffer(const Schema& s, const uint8_t* begin, const uint8_t* end) {
    auto in_range = [&](const void* p) -> bool {
        if (!p) return true;  // nullptr is fine
        const auto* b = static_cast<const uint8_t*>(p);
        return b >= begin && b < end;
    };

    if (!in_range(s.engineName)) return false;
    if (!in_range(s.engineVersion)) return false;
    if (!in_range(s.pluginVersion)) return false;
    if (!in_range(s.info)) return false;

    if (s.channels.nChannels > 0) {
        if (!in_range(s.channels.channels)) return false;
        for (uint32_t i = 0; i < s.channels.nChannels; ++i)
            if (!in_range(s.channels.channels[i])) return false;
    }

    if (s.scenes.nScenes > 0) {
        if (!in_range(s.scenes.scenes)) return false;
        for (uint32_t i = 0; i < s.scenes.nScenes; ++i) {
            const RemoteParameters& sc = s.scenes.scenes[i];
            if (!in_range(sc.name)) return false;
            if (sc.nParameters > 0) {
                if (!in_range(sc.parameters)) return false;
                for (uint32_t k = 0; k < sc.nParameters; ++k) {
                    const RemoteParameter& p = sc.parameters[k];
                    if (!in_range(p.group)) return false;
                    if (!in_range(p.displayName)) return false;
                    if (!in_range(p.key)) return false;
                    if (p.type == RS_PARAMETER_TEXT)
                        if (!in_range(p.defaults.text.defaultValue)) return false;
                    if (p.nOptions > 0) {
                        if (!in_range(p.options)) return false;
                        for (uint32_t o = 0; o < p.nOptions; ++o)
                            if (!in_range(p.options[o])) return false;
                    }
                }
            }
        }
    }
    return true;
}

// Returns true when every non-null string reachable from the Schema tree
// is null-terminated within the buffer.
bool AllStringsTerminated(const Schema& s,
                          const uint8_t* begin, const uint8_t* end) {
    auto terminated = [&](const char* p) -> bool {
        if (!p) return true;
        const auto* b = reinterpret_cast<const uint8_t*>(p);
        if (b < begin || b >= end) return false;
        // Scan for '\0' within remaining buffer
        while (b < end) {
            if (*b == 0) return true;
            ++b;
        }
        return false;
    };

    if (!terminated(s.engineName)) return false;
    if (!terminated(s.engineVersion)) return false;
    if (!terminated(s.pluginVersion)) return false;
    if (!terminated(s.info)) return false;

    for (uint32_t i = 0; i < s.channels.nChannels; ++i)
        if (!terminated(s.channels.channels[i])) return false;

    for (uint32_t i = 0; i < s.scenes.nScenes; ++i) {
        const RemoteParameters& sc = s.scenes.scenes[i];
        if (!terminated(sc.name)) return false;
        for (uint32_t k = 0; k < sc.nParameters; ++k) {
            const RemoteParameter& p = sc.parameters[k];
            if (!terminated(p.group)) return false;
            if (!terminated(p.displayName)) return false;
            if (!terminated(p.key)) return false;
            if (p.type == RS_PARAMETER_TEXT)
                if (!terminated(p.defaults.text.defaultValue)) return false;
            for (uint32_t o = 0; o < p.nOptions; ++o)
                if (!terminated(p.options[o])) return false;
        }
    }
    return true;
}

// Returns true when every array pointer in the Schema tree is 8-byte aligned.
bool AllArrayPointersAligned(const Schema& s) {
    auto aligned = [](const void* p, size_t a) -> bool {
        if (!p) return true;
        return (reinterpret_cast<uintptr_t>(p) & (a - 1)) == 0;
    };

    if (!aligned(s.channels.channels, alignof(const char*))) return false;
    if (!aligned(s.scenes.scenes, alignof(RemoteParameters))) return false;

    for (uint32_t i = 0; i < s.scenes.nScenes; ++i) {
        const RemoteParameters& sc = s.scenes.scenes[i];
        if (!aligned(sc.parameters, alignof(RemoteParameter))) return false;
        for (uint32_t k = 0; k < sc.nParameters; ++k) {
            const RemoteParameter& p = sc.parameters[k];
            if (!aligned(p.options, alignof(const char*))) return false;
        }
    }
    return true;
}

// Last write offset (exclusive) — the highest address written into the buffer.
// We walk the tree and find the farthest-end string or struct.
size_t LastWriteOffset(const Schema& s, const uint8_t* base) {
    size_t hi = sizeof(Schema);  // at least the Schema struct
    auto bump = [&](const void* p, size_t len) {
        if (!p) return;
        size_t end_off = static_cast<const uint8_t*>(p) - base + len;
        if (end_off > hi) hi = end_off;
    };

    bump(s.engineName,    std::strlen(s.engineName    ? s.engineName    : "") + 1);
    bump(s.engineVersion, std::strlen(s.engineVersion ? s.engineVersion : "") + 1);
    bump(s.pluginVersion, std::strlen(s.pluginVersion ? s.pluginVersion : "") + 1);
    bump(s.info,          std::strlen(s.info          ? s.info          : "") + 1);

    bump(s.channels.channels,
         s.channels.nChannels * sizeof(const char*));
    for (uint32_t i = 0; i < s.channels.nChannels; ++i)
        bump(s.channels.channels[i],
             std::strlen(s.channels.channels[i]) + 1);

    bump(s.scenes.scenes,
         s.scenes.nScenes * sizeof(RemoteParameters));
    for (uint32_t i = 0; i < s.scenes.nScenes; ++i) {
        const RemoteParameters& sc = s.scenes.scenes[i];
        bump(sc.name, std::strlen(sc.name ? sc.name : "") + 1);
        bump(sc.parameters,
             sc.nParameters * sizeof(RemoteParameter));
        for (uint32_t k = 0; k < sc.nParameters; ++k) {
            const RemoteParameter& p = sc.parameters[k];
            bump(p.group,       std::strlen(p.group       ? p.group       : "") + 1);
            bump(p.displayName, std::strlen(p.displayName ? p.displayName : "") + 1);
            bump(p.key,         std::strlen(p.key         ? p.key         : "") + 1);
            if (p.type == RS_PARAMETER_TEXT)
                bump(p.defaults.text.defaultValue,
                     std::strlen(p.defaults.text.defaultValue
                                    ? p.defaults.text.defaultValue
                                    : "") +
                         1);
            bump(p.options, p.nOptions * sizeof(const char*));
            for (uint32_t o = 0; o < p.nOptions; ++o)
                bump(p.options[o], std::strlen(p.options[o]) + 1);
        }
    }
    return hi;
}

}  // namespace

// ---- data-correctness tests ------------------------------------------------

TEST_CASE("Empty JSON produces zeroed Schema") {
    rs::schema::Serializer ser;
    json empty = json::object();

    size_t sz = ser.Measure(empty);
    REQUIRE(sz > 0);
    REQUIRE(sz >= sizeof(Schema));

    std::vector<uint8_t> buf(sz);
    Schema* s = ser.Write(empty, buf.data(), sz);
    REQUIRE(s != nullptr);

    CHECK(s->engineName == nullptr);
    CHECK(s->engineVersion == nullptr);
    CHECK(s->pluginVersion == nullptr);
    CHECK(s->info == nullptr);
    CHECK(s->channels.nChannels == 0u);
    CHECK(s->channels.channels == nullptr);
    CHECK(s->scenes.nScenes == 0u);
    CHECK(s->scenes.scenes == nullptr);
}

TEST_CASE("Top-level strings are preserved") {
    rs::schema::Serializer ser;
    auto j = MakeMinimalJson();
    size_t sz = ser.Measure(j);

    std::vector<uint8_t> buf(sz);
    Schema* s = ser.Write(j, buf.data(), sz);
    REQUIRE(s != nullptr);

    CHECK(s->engineName != nullptr);
    CHECK(std::strcmp(s->engineName, "TestEngine") == 0);
    CHECK(s->engineVersion != nullptr);
    CHECK(std::strcmp(s->engineVersion, "5.3") == 0);
    CHECK(s->pluginVersion != nullptr);
    CHECK(std::strcmp(s->pluginVersion, "2.0") == 0);
    CHECK(s->info != nullptr);
    CHECK(std::strcmp(s->info, "Integration test") == 0);
}

TEST_CASE("Channels array") {
    rs::schema::Serializer ser;
    auto j = MakeMinimalJson();
    j["channels"] = json::array({"X", "Y", "Z"});

    size_t sz = ser.Measure(j);
    std::vector<uint8_t> buf(sz);
    Schema* s = ser.Write(j, buf.data(), sz);
    REQUIRE(s != nullptr);

    REQUIRE(s->channels.nChannels == 3u);
    CHECK(std::strcmp(s->channels.channels[0], "X") == 0);
    CHECK(std::strcmp(s->channels.channels[1], "Y") == 0);
    CHECK(std::strcmp(s->channels.channels[2], "Z") == 0);
}

TEST_CASE("Empty channels array") {
    rs::schema::Serializer ser;
    auto j = MakeMinimalJson();
    j["channels"] = json::array();

    size_t sz = ser.Measure(j);
    std::vector<uint8_t> buf(sz);
    Schema* s = ser.Write(j, buf.data(), sz);
    REQUIRE(s != nullptr);

    CHECK(s->channels.nChannels == 0u);
    CHECK(s->channels.channels == nullptr);
}

TEST_CASE("Scenes count and names") {
    rs::schema::Serializer ser;
    auto j = MakeMinimalJson();
    j["scenes"] = json::array({
        {{"name", "A"}, {"hash", 1ULL}, {"parameters", json::array()}},
        {{"name", "B"}, {"hash", 2ULL}, {"parameters", json::array()}},
    });

    size_t sz = ser.Measure(j);
    std::vector<uint8_t> buf(sz);
    Schema* s = ser.Write(j, buf.data(), sz);
    REQUIRE(s != nullptr);

    REQUIRE(s->scenes.nScenes == 2u);
    CHECK(std::strcmp(s->scenes.scenes[0].name, "A") == 0);
    CHECK(s->scenes.scenes[0].hash == 1ULL);
    CHECK(std::strcmp(s->scenes.scenes[1].name, "B") == 0);
    CHECK(s->scenes.scenes[1].hash == 2ULL);
}

TEST_CASE("Empty scenes array") {
    rs::schema::Serializer ser;
    auto j = MakeMinimalJson();
    j["scenes"] = json::array();

    size_t sz = ser.Measure(j);
    std::vector<uint8_t> buf(sz);
    Schema* s = ser.Write(j, buf.data(), sz);
    REQUIRE(s != nullptr);

    CHECK(s->scenes.nScenes == 0u);
    CHECK(s->scenes.scenes == nullptr);
}

TEST_CASE("NUMBER parameter fields") {
    rs::schema::Serializer ser;
    auto j = MakeMinimalJson();
    // Keep only the NUMBER parameter from the minimal fixture
    auto& params = j["scenes"][0]["parameters"];
    params = json::array({params[0]});

    size_t sz = ser.Measure(j);
    std::vector<uint8_t> buf(sz);
    Schema* s = ser.Write(j, buf.data(), sz);
    REQUIRE(s != nullptr);

    const RemoteParameter& p = s->scenes.scenes[0].parameters[0];
    CHECK(p.type == RS_PARAMETER_NUMBER);
    CHECK(std::strcmp(p.group, "Transform") == 0);
    CHECK(std::strcmp(p.displayName, "Speed") == 0);
    CHECK(std::strcmp(p.key, "speed") == 0);
    CHECK(p.defaults.number.min == 0.0f);
    CHECK(p.defaults.number.max == 100.0f);
    CHECK(p.defaults.number.step == 1.0f);
    CHECK(p.defaults.number.defaultValue == 50.0f);
    CHECK(p.nOptions == 0u);
    CHECK(p.options == nullptr);
}

TEST_CASE("TEXT parameter default value") {
    rs::schema::Serializer ser;
    auto j = MakeMinimalJson();
    auto& params = j["scenes"][0]["parameters"];
    // Keep the TEXT parameter
    params = json::array({params[1]});

    size_t sz = ser.Measure(j);
    std::vector<uint8_t> buf(sz);
    Schema* s = ser.Write(j, buf.data(), sz);
    REQUIRE(s != nullptr);

    const RemoteParameter& p = s->scenes.scenes[0].parameters[0];
    CHECK(p.type == RS_PARAMETER_TEXT);
    CHECK(p.defaults.text.defaultValue != nullptr);
    CHECK(std::strcmp(p.defaults.text.defaultValue, "hello") == 0);
}

TEST_CASE("Parameter options array") {
    rs::schema::Serializer ser;
    auto j = MakeMinimalJson();
    auto& params = j["scenes"][0]["parameters"];
    params = json::array({params[1]});  // TEXT param with options ["A","B","C"]

    size_t sz = ser.Measure(j);
    std::vector<uint8_t> buf(sz);
    Schema* s = ser.Write(j, buf.data(), sz);
    REQUIRE(s != nullptr);

    const RemoteParameter& p = s->scenes.scenes[0].parameters[0];
    REQUIRE(p.nOptions == 3u);
    CHECK(std::strcmp(p.options[0], "A") == 0);
    CHECK(std::strcmp(p.options[1], "B") == 0);
    CHECK(std::strcmp(p.options[2], "C") == 0);
}

TEST_CASE("Empty parameter options array") {
    rs::schema::Serializer ser;
    auto j = MakeMinimalJson();
    auto& params = j["scenes"][0]["parameters"];
    auto& opts = params[1]["options"];
    opts = json::array();

    size_t sz = ser.Measure(j);
    std::vector<uint8_t> buf(sz);
    Schema* s = ser.Write(j, buf.data(), sz);
    REQUIRE(s != nullptr);

    const RemoteParameter& p = s->scenes.scenes[0].parameters[1];
    CHECK(p.nOptions == 0u);
    CHECK(p.options == nullptr);
}

TEST_CASE("DMX fields are preserved") {
    rs::schema::Serializer ser;
    auto j = MakeMinimalJson();
    auto& params = j["scenes"][0]["parameters"];
    params = json::array({params[1]});  // dmxOffset=5, dmxType=RS_DMX_16_BE, flags=3

    size_t sz = ser.Measure(j);
    std::vector<uint8_t> buf(sz);
    Schema* s = ser.Write(j, buf.data(), sz);
    REQUIRE(s != nullptr);

    const RemoteParameter& p = s->scenes.scenes[0].parameters[0];
    CHECK(p.dmxOffset == 5);
    CHECK(p.dmxType == RS_DMX_16_BE);
    CHECK(p.flags == 3u);
}

TEST_CASE("All pointers inside buffer") {
    rs::schema::Serializer ser;
    auto j = MakeMinimalJson();
    size_t sz = ser.Measure(j);

    std::vector<uint8_t> buf(sz);
    Schema* s = ser.Write(j, buf.data(), sz);
    REQUIRE(s != nullptr);

    CHECK(AllPointersInBuffer(*s, buf.data(), buf.data() + sz));
}

TEST_CASE("All strings null-terminated within buffer") {
    rs::schema::Serializer ser;
    auto j = MakeMinimalJson();
    size_t sz = ser.Measure(j);

    std::vector<uint8_t> buf(sz);
    Schema* s = ser.Write(j, buf.data(), sz);
    REQUIRE(s != nullptr);

    CHECK(AllStringsTerminated(*s, buf.data(), buf.data() + sz));
}

TEST_CASE("All array pointers aligned") {
    rs::schema::Serializer ser;
    auto j = MakeMinimalJson();
    size_t sz = ser.Measure(j);

    std::vector<uint8_t> buf(sz);
    Schema* s = ser.Write(j, buf.data(), sz);
    REQUIRE(s != nullptr);

    CHECK(AllArrayPointersAligned(*s));
}

TEST_CASE("No overflow: last written offset within buffer") {
    rs::schema::Serializer ser;
    auto j = MakeMinimalJson();
    size_t sz = ser.Measure(j);

    std::vector<uint8_t> buf(sz);
    Schema* s = ser.Write(j, buf.data(), sz);
    REQUIRE(s != nullptr);

    size_t last = LastWriteOffset(*s, buf.data());
    CHECK(last <= sz);
}

TEST_CASE("Measure returns same value on repeated calls") {
    rs::schema::Serializer ser;
    auto j = MakeMinimalJson();

    CHECK(ser.Measure(j) == ser.Measure(j));
}

TEST_CASE("Measure then Write on two separate Serializer instances") {
    auto j = MakeMinimalJson();

    rs::schema::Serializer s1;
    size_t sz = s1.Measure(j);

    rs::schema::Serializer s2;
    size_t sz2 = s2.Measure(j);
    REQUIRE(sz == sz2);

    std::vector<uint8_t> buf(sz);
    Schema* s = s2.Write(j, buf.data(), sz);
    REQUIRE(s != nullptr);
    CHECK(std::strcmp(s->engineName, "TestEngine") == 0);
}

TEST_CASE("Write with nullptr buffer returns nullptr") {
    rs::schema::Serializer ser;
    auto j = MakeMinimalJson();
    size_t sz = ser.Measure(j);

    Schema* s = ser.Write(j, nullptr, sz);
    CHECK(s == nullptr);
}

TEST_CASE("Write with undersized buffer returns nullptr") {
    rs::schema::Serializer ser;
    auto j = MakeMinimalJson();
    size_t sz = ser.Measure(j);
    REQUIRE(sz > 0);

    std::vector<uint8_t> buf(sz - 1);
    Schema* s = ser.Write(j, buf.data(), sz - 1);
    CHECK(s == nullptr);
}

TEST_CASE("Missing optional fields result in nullptr / zero") {
    rs::schema::Serializer ser;
    json j = json::object();
    j["scenes"] = json::array();
    j["channels"] = json::array();

    // Omit engineName, engineVersion, pluginVersion, info entirely.
    size_t sz = ser.Measure(j);
    std::vector<uint8_t> buf(sz);
    Schema* s = ser.Write(j, buf.data(), sz);
    REQUIRE(s != nullptr);

    CHECK(s->engineName == nullptr);
    CHECK(s->engineVersion == nullptr);
    CHECK(s->pluginVersion == nullptr);
    CHECK(s->info == nullptr);
    CHECK(s->channels.nChannels == 0u);
    CHECK(s->scenes.nScenes == 0u);
}

TEST_CASE("Very long strings do not truncate") {
    rs::schema::Serializer ser;
    auto j = MakeMinimalJson();
    std::string long_name(2000, 'K');
    j["engineName"] = long_name;

    size_t sz = ser.Measure(j);
    std::vector<uint8_t> buf(sz);
    Schema* s = ser.Write(j, buf.data(), sz);
    REQUIRE(s != nullptr);

    CHECK(std::strlen(s->engineName) == 2000u);
    CHECK(std::strcmp(s->engineName, long_name.c_str()) == 0);
}

TEST_CASE("Many scenes and parameters scale linearly in buffer size") {
    rs::schema::Serializer ser;
    json j = json::object();
    j["engineName"] = "Big";
    j["engineVersion"] = "1";
    j["pluginVersion"] = "2";
    j["info"] = "";
    j["channels"] = json::array();

    json scenes = json::array();
    for (int i = 0; i < 10; ++i) {
        json scene;
        scene["name"] = "Scene_" + std::to_string(i);
        scene["hash"] = uint64_t(i);

        json params = json::array();
        for (int k = 0; k < 20; ++k) {
            json param;
            param["group"] = "G";
            param["displayName"] = "D";
            param["key"] = "k_" + std::to_string(k);
            param["type"] = RS_PARAMETER_NUMBER;
            param["min"] = 0.0;
            param["max"] = 1.0;
            param["step"] = 0.1;
            param["defaultValue"] = 0.5;
            param["options"] = json::array();
            param["dmxOffset"] = -1;
            param["dmxType"] = RS_DMX_DEFAULT;
            param["flags"] = 0u;
            params.push_back(param);
        }
        scene["parameters"] = params;
        scenes.push_back(scene);
    }
    j["scenes"] = scenes;

    size_t sz = ser.Measure(j);
    std::vector<uint8_t> buf(sz);
    Schema* s = ser.Write(j, buf.data(), sz);
    REQUIRE(s != nullptr);

    REQUIRE(s->scenes.nScenes == 10u);
    CHECK(s->scenes.scenes[9].nParameters == 20u);
    CHECK(AllPointersInBuffer(*s, buf.data(), buf.data() + sz));
}

TEST_CASE("Schema pointer equals buffer start when alignment is zero") {
    rs::schema::Serializer ser;
    auto j = MakeMinimalJson();
    size_t sz = ser.Measure(j);
    std::vector<uint8_t> buf(sz);
    Schema* s = ser.Write(j, buf.data(), sz);
    REQUIRE(s != nullptr);

    // On x64 the buffer is already 8-byte aligned; Schema starts at byte 0.
    CHECK(reinterpret_cast<uint8_t*>(s) == buf.data());
}

TEST_CASE("Explicit empty strings become nullptr") {
    rs::schema::Serializer ser;
    auto j = MakeMinimalJson();
    j["engineName"] = "";
    j["info"] = "";

    size_t sz = ser.Measure(j);
    std::vector<uint8_t> buf(sz);
    Schema* s = ser.Write(j, buf.data(), sz);
    REQUIRE(s != nullptr);

    CHECK(s->engineName == nullptr);
    CHECK(s->info == nullptr);
}

TEST_CASE("Scene parameter count is zero when no parameters") {
    rs::schema::Serializer ser;
    auto j = MakeMinimalJson();
    j["scenes"][0]["parameters"] = json::array();

    size_t sz = ser.Measure(j);
    std::vector<uint8_t> buf(sz);
    Schema* s = ser.Write(j, buf.data(), sz);
    REQUIRE(s != nullptr);

    const RemoteParameters& sc = s->scenes.scenes[0];
    CHECK(sc.nParameters == 0u);
    CHECK(sc.parameters == nullptr);
}