#include "xmp_patch.hpp"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <variant>

namespace filtrox {
namespace {

std::string trim(std::string value) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char c) {
        return !is_space(static_cast<unsigned char>(c));
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [&](char c) {
        return !is_space(static_cast<unsigned char>(c));
    }).base(), value.end());
    return value;
}

std::string unquote(std::string value) {
    value = trim(std::move(value));
    if (value.size() >= 2) {
        const char first = value.front();
        const char last = value.back();
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            return value.substr(1, value.size() - 2);
        }
    }
    return value;
}

bool is_name_boundary(const std::string& text, std::size_t pos) {
    return pos == 0 || std::isspace(static_cast<unsigned char>(text[pos - 1])) != 0;
}

bool is_attr_name_char(char c) {
    const auto uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) != 0 || c == '_' || c == '-' || c == ':';
}

std::string normalize_darktable_attr(const std::string& key) {
    if (key.find(':') != std::string::npos) {
        return key;
    }
    return "darktable:" + key;
}

void set_patch_attr(ModulePatch& patch, std::string name, std::string value) {
    name = normalize_darktable_attr(std::move(name));
    if (name == "darktable:operation") {
        patch.operation = std::move(value);
        return;
    }

    for (auto& attr : patch.attributes) {
        if (attr.name == name) {
            attr.value = std::move(value);
            return;
        }
    }
    patch.attributes.push_back({std::move(name), std::move(value)});
}

enum class JsonType {
    Null,
    Bool,
    Number,
    String,
    Array,
    Object,
};

struct JsonValue {
    JsonType type = JsonType::Null;
    bool bool_value = false;
    std::string text;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object;

    const JsonValue* get(const std::string& key) const {
        if (type != JsonType::Object) {
            return nullptr;
        }
        for (const auto& item : object) {
            if (item.first == key) {
                return &item.second;
            }
        }
        return nullptr;
    }

    bool is_scalar() const {
        return type == JsonType::Bool || type == JsonType::Number || type == JsonType::String;
    }

    std::string scalar_string() const {
        if (type == JsonType::Bool) {
            return bool_value ? "1" : "0";
        }
        return text;
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string input)
        : input_(std::move(input)) {
    }

    JsonValue parse() {
        skip_ws();
        JsonValue value = parse_value();
        skip_ws();
        if (pos_ != input_.size()) {
            throw std::runtime_error("unexpected trailing JSON content");
        }
        return value;
    }

private:
    std::string input_;
    std::size_t pos_ = 0;

    void skip_ws() {
        while (pos_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[pos_])) != 0) {
            ++pos_;
        }
    }

    bool consume(char expected) {
        skip_ws();
        if (pos_ < input_.size() && input_[pos_] == expected) {
            ++pos_;
            return true;
        }
        return false;
    }

    void expect(char expected) {
        if (!consume(expected)) {
            throw std::runtime_error(std::string("expected JSON character: ") + expected);
        }
    }

    JsonValue parse_value() {
        skip_ws();
        if (pos_ >= input_.size()) {
            throw std::runtime_error("unexpected end of JSON");
        }
        const char c = input_[pos_];
        if (c == '{') {
            return parse_object();
        }
        if (c == '[') {
            return parse_array();
        }
        if (c == '"') {
            JsonValue value;
            value.type = JsonType::String;
            value.text = parse_string();
            return value;
        }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c)) != 0) {
            JsonValue value;
            value.type = JsonType::Number;
            value.text = parse_number();
            return value;
        }
        if (input_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            JsonValue value;
            value.type = JsonType::Bool;
            value.bool_value = true;
            return value;
        }
        if (input_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            JsonValue value;
            value.type = JsonType::Bool;
            value.bool_value = false;
            return value;
        }
        if (input_.compare(pos_, 4, "null") == 0) {
            pos_ += 4;
            return JsonValue{};
        }
        throw std::runtime_error("invalid JSON value");
    }

    JsonValue parse_object() {
        expect('{');
        JsonValue value;
        value.type = JsonType::Object;
        skip_ws();
        if (consume('}')) {
            return value;
        }
        while (true) {
            skip_ws();
            if (pos_ >= input_.size() || input_[pos_] != '"') {
                throw std::runtime_error("expected JSON object key");
            }
            std::string key = parse_string();
            expect(':');
            value.object.push_back({std::move(key), parse_value()});
            if (consume('}')) {
                break;
            }
            expect(',');
        }
        return value;
    }

    JsonValue parse_array() {
        expect('[');
        JsonValue value;
        value.type = JsonType::Array;
        skip_ws();
        if (consume(']')) {
            return value;
        }
        while (true) {
            value.array.push_back(parse_value());
            if (consume(']')) {
                break;
            }
            expect(',');
        }
        return value;
    }

    std::string parse_string() {
        expect('"');
        std::string out;
        while (pos_ < input_.size()) {
            const char c = input_[pos_++];
            if (c == '"') {
                return out;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= input_.size()) {
                throw std::runtime_error("unterminated JSON escape");
            }
            const char escaped = input_[pos_++];
            switch (escaped) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u':
                    if (pos_ + 4 > input_.size()) {
                        throw std::runtime_error("invalid JSON unicode escape");
                    }
                    out.push_back('?');
                    pos_ += 4;
                    break;
                default:
                    throw std::runtime_error("invalid JSON escape");
            }
        }
        throw std::runtime_error("unterminated JSON string");
    }

    std::string parse_number() {
        const std::size_t start = pos_;
        if (input_[pos_] == '-') {
            ++pos_;
        }
        while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_])) != 0) {
            ++pos_;
        }
        if (pos_ < input_.size() && input_[pos_] == '.') {
            ++pos_;
            while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_])) != 0) {
                ++pos_;
            }
        }
        if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) {
                ++pos_;
            }
            while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_])) != 0) {
                ++pos_;
            }
        }
        return input_.substr(start, pos_ - start);
    }
};

std::string strip_json_fence(std::string text) {
    const std::string fence = "```";
    const std::size_t start = text.find(fence);
    if (start == std::string::npos) {
        return text;
    }
    std::size_t content_start = text.find('\n', start + fence.size());
    if (content_start == std::string::npos) {
        return text;
    }
    ++content_start;
    const std::size_t end = text.find(fence, content_start);
    if (end == std::string::npos) {
        return text;
    }
    return text.substr(content_start, end - content_start);
}

const JsonValue* find_modules_object(const JsonValue& root) {
    if (root.type != JsonType::Object) {
        return nullptr;
    }
    if (const JsonValue* modules = root.get("modules")) {
        return modules->type == JsonType::Object ? modules : nullptr;
    }
    if (const JsonValue* params = root.get("params")) {
        if (const JsonValue* modules = params->get("modules")) {
            return modules->type == JsonType::Object ? modules : nullptr;
        }
    }
    return root.type == JsonType::Object ? &root : nullptr;
}

enum class FieldType {
    Float32,
    Int32,
};

struct FieldSpec {
    std::string name;
    FieldType type;
    std::size_t count = 1;
};

const std::vector<FieldSpec>* module_spec(const std::string& op) {
    static const std::vector<FieldSpec> exposure = {
        {"mode", FieldType::Int32}, {"black", FieldType::Float32}, {"exposure", FieldType::Float32},
        {"deflicker_percentile", FieldType::Float32}, {"deflicker_target_level", FieldType::Float32},
        {"compensate_exposure_bias", FieldType::Int32}, {"compensate_hilite_pres", FieldType::Int32},
    };
    static const std::vector<FieldSpec> sigmoid = {
        {"middle_grey_contrast", FieldType::Float32}, {"contrast_skewness", FieldType::Float32},
        {"display_white_target", FieldType::Float32}, {"display_black_target", FieldType::Float32},
        {"color_processing", FieldType::Int32}, {"hue_preservation", FieldType::Float32},
        {"red_inset", FieldType::Float32}, {"red_rotation", FieldType::Float32},
        {"green_inset", FieldType::Float32}, {"green_rotation", FieldType::Float32},
        {"blue_inset", FieldType::Float32}, {"blue_rotation", FieldType::Float32},
        {"purity", FieldType::Float32}, {"base_primaries", FieldType::Int32},
    };
    static const std::vector<FieldSpec> toneequal = {
        {"noise", FieldType::Float32}, {"ultra_deep_blacks", FieldType::Float32},
        {"deep_blacks", FieldType::Float32}, {"blacks", FieldType::Float32},
        {"shadows", FieldType::Float32}, {"midtones", FieldType::Float32},
        {"highlights", FieldType::Float32}, {"whites", FieldType::Float32},
        {"speculars", FieldType::Float32}, {"blending", FieldType::Float32},
        {"smoothing", FieldType::Float32}, {"feathering", FieldType::Float32},
        {"quantization", FieldType::Float32}, {"contrast_boost", FieldType::Float32},
        {"exposure_boost", FieldType::Float32}, {"details", FieldType::Int32},
        {"method", FieldType::Int32}, {"iterations", FieldType::Int32},
    };
    static const std::vector<FieldSpec> temperature = {
        {"red", FieldType::Float32}, {"green", FieldType::Float32}, {"blue", FieldType::Float32},
        {"various", FieldType::Float32}, {"preset", FieldType::Int32},
    };
    static const std::vector<FieldSpec> diffuse = {
        {"iterations", FieldType::Int32}, {"sharpness", FieldType::Float32},
        {"radius", FieldType::Int32}, {"regularization", FieldType::Float32},
        {"variance_threshold", FieldType::Float32}, {"anisotropy_first", FieldType::Float32},
        {"anisotropy_second", FieldType::Float32}, {"anisotropy_third", FieldType::Float32},
        {"anisotropy_fourth", FieldType::Float32}, {"threshold", FieldType::Float32},
        {"first", FieldType::Float32}, {"second", FieldType::Float32},
        {"third", FieldType::Float32}, {"fourth", FieldType::Float32},
        {"radius_center", FieldType::Int32},
    };
    static const std::vector<FieldSpec> hazeremoval = {
        {"strength", FieldType::Float32}, {"distance", FieldType::Float32},
        {"slope", FieldType::Float32}, {"saturation", FieldType::Float32},
        {"unbound", FieldType::Int32}, {"iterations", FieldType::Int32},
    };
    static const std::vector<FieldSpec> vignette = {
        {"scale", FieldType::Float32}, {"falloff_scale", FieldType::Float32},
        {"brightness", FieldType::Float32}, {"saturation", FieldType::Float32},
        {"center", FieldType::Float32, 2}, {"autoratio", FieldType::Int32},
        {"whratio", FieldType::Float32}, {"shape", FieldType::Float32},
        {"dithering", FieldType::Int32}, {"unbound", FieldType::Int32},
    };
    static const std::vector<FieldSpec> grain = {
        {"channel", FieldType::Int32}, {"scale", FieldType::Float32},
        {"strength", FieldType::Float32}, {"midtones", FieldType::Float32},
    };
    static const std::vector<FieldSpec> colorbalancergb = {
        {"shadows_Y", FieldType::Float32}, {"shadows_C", FieldType::Float32}, {"shadows_H", FieldType::Float32},
        {"midtones_Y", FieldType::Float32}, {"midtones_C", FieldType::Float32}, {"midtones_H", FieldType::Float32},
        {"highlights_Y", FieldType::Float32}, {"highlights_C", FieldType::Float32}, {"highlights_H", FieldType::Float32},
        {"global_Y", FieldType::Float32}, {"global_C", FieldType::Float32}, {"global_H", FieldType::Float32},
        {"shadows_weight", FieldType::Float32}, {"white_fulcrum", FieldType::Float32}, {"highlights_weight", FieldType::Float32},
        {"chroma_shadows", FieldType::Float32}, {"chroma_highlights", FieldType::Float32},
        {"chroma_global", FieldType::Float32}, {"chroma_midtones", FieldType::Float32},
        {"saturation_global", FieldType::Float32}, {"saturation_highlights", FieldType::Float32},
        {"saturation_midtones", FieldType::Float32}, {"saturation_shadows", FieldType::Float32},
        {"hue_angle", FieldType::Float32}, {"brilliance_global", FieldType::Float32},
        {"brilliance_highlights", FieldType::Float32}, {"brilliance_midtones", FieldType::Float32},
        {"brilliance_shadows", FieldType::Float32}, {"mask_grey_fulcrum", FieldType::Float32},
        {"vibrance", FieldType::Float32}, {"grey_fulcrum", FieldType::Float32},
        {"contrast", FieldType::Float32}, {"saturation_formula", FieldType::Int32},
    };
    static const std::vector<FieldSpec> colorequal = {
        {"threshold", FieldType::Float32}, {"smoothing_hue", FieldType::Float32},
        {"contrast", FieldType::Float32}, {"white_level", FieldType::Float32},
        {"chroma_size", FieldType::Float32}, {"param_size", FieldType::Float32},
        {"use_filter", FieldType::Int32},
        {"sat_red", FieldType::Float32}, {"sat_orange", FieldType::Float32},
        {"sat_yellow", FieldType::Float32}, {"sat_green", FieldType::Float32},
        {"sat_cyan", FieldType::Float32}, {"sat_blue", FieldType::Float32},
        {"sat_lavender", FieldType::Float32}, {"sat_magenta", FieldType::Float32},
        {"hue_red", FieldType::Float32}, {"hue_orange", FieldType::Float32},
        {"hue_yellow", FieldType::Float32}, {"hue_green", FieldType::Float32},
        {"hue_cyan", FieldType::Float32}, {"hue_blue", FieldType::Float32},
        {"hue_lavender", FieldType::Float32}, {"hue_magenta", FieldType::Float32},
        {"bright_red", FieldType::Float32}, {"bright_orange", FieldType::Float32},
        {"bright_yellow", FieldType::Float32}, {"bright_green", FieldType::Float32},
        {"bright_cyan", FieldType::Float32}, {"bright_blue", FieldType::Float32},
        {"bright_lavender", FieldType::Float32}, {"bright_magenta", FieldType::Float32},
        {"hue_shift", FieldType::Float32},
    };

    if (op == "exposure") return &exposure;
    if (op == "sigmoid") return &sigmoid;
    if (op == "toneequal") return &toneequal;
    if (op == "temperature") return &temperature;
    if (op == "diffuse") return &diffuse;
    if (op == "hazeremoval") return &hazeremoval;
    if (op == "vignette") return &vignette;
    if (op == "grain") return &grain;
    if (op == "colorbalancergb") return &colorbalancergb;
    if (op == "colorequal") return &colorequal;
    return nullptr;
}

bool uses_hex_by_default(const std::string& op) {
    return op == "exposure" || op == "sigmoid" || op == "toneequal" || op == "temperature" ||
           op == "diffuse" || op == "hazeremoval" || op == "vignette" || op == "grain";
}

const JsonValue* object_get(const JsonValue& object, const std::string& key) {
    return object.get(key);
}

float json_float(const JsonValue* value) {
    if (!value) {
        return 0.0F;
    }
    if (value->type == JsonType::String) {
        return std::stof(value->text);
    }
    if (value->type == JsonType::Number) {
        return std::stof(value->text);
    }
    if (value->type == JsonType::Bool) {
        return value->bool_value ? 1.0F : 0.0F;
    }
    return 0.0F;
}

std::int32_t json_int(const JsonValue* value) {
    if (!value) {
        return 0;
    }
    if (value->type == JsonType::Bool) {
        return value->bool_value ? 1 : 0;
    }
    if (value->type == JsonType::String || value->type == JsonType::Number) {
        return static_cast<std::int32_t>(std::stod(value->text));
    }
    return 0;
}

void append_u32_le(std::vector<unsigned char>& out, std::uint32_t value) {
    out.push_back(static_cast<unsigned char>(value & 0xffU));
    out.push_back(static_cast<unsigned char>((value >> 8U) & 0xffU));
    out.push_back(static_cast<unsigned char>((value >> 16U) & 0xffU));
    out.push_back(static_cast<unsigned char>((value >> 24U) & 0xffU));
}

void append_i32_le(std::vector<unsigned char>& out, std::int32_t value) {
    append_u32_le(out, static_cast<std::uint32_t>(value));
}

void append_f32_le(std::vector<unsigned char>& out, float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    append_u32_le(out, bits);
}

std::vector<unsigned char> build_raw_params(const std::string& op, const JsonValue& params) {
    const auto* spec = module_spec(op);
    if (!spec || params.type != JsonType::Object) {
        return {};
    }

    std::vector<unsigned char> raw;
    for (const auto& field : *spec) {
        const JsonValue* value = object_get(params, field.name);
        for (std::size_t i = 0; i < field.count; ++i) {
            const JsonValue* item = value;
            if (value && value->type == JsonType::Array) {
                item = i < value->array.size() ? &value->array[i] : nullptr;
            }
            if (field.type == FieldType::Float32) {
                append_f32_le(raw, json_float(item));
            } else {
                append_i32_le(raw, json_int(item));
            }
        }
    }
    return raw;
}

std::string hex_encode(const std::vector<unsigned char>& bytes) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (unsigned char byte : bytes) {
        out << std::setw(2) << static_cast<int>(byte);
    }
    return out.str();
}

std::string base64_encode(const std::vector<unsigned char>& bytes) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::uint32_t a = bytes[i];
        const std::uint32_t b = (i + 1 < bytes.size()) ? bytes[i + 1] : 0;
        const std::uint32_t c = (i + 2 < bytes.size()) ? bytes[i + 2] : 0;
        const std::uint32_t triple = (a << 16U) | (b << 8U) | c;
        out.push_back(alphabet[(triple >> 18U) & 0x3fU]);
        out.push_back(alphabet[(triple >> 12U) & 0x3fU]);
        out.push_back(i + 1 < bytes.size() ? alphabet[(triple >> 6U) & 0x3fU] : '=');
        out.push_back(i + 2 < bytes.size() ? alphabet[triple & 0x3fU] : '=');
    }
    return out;
}

std::string dt_encode_gz(const std::vector<unsigned char>& raw) {
    uLongf bound = compressBound(static_cast<uLong>(raw.size()));
    std::vector<unsigned char> compressed(bound);
    const int rc = compress2(
        compressed.data(),
        &bound,
        raw.data(),
        static_cast<uLong>(raw.size()),
        Z_BEST_COMPRESSION
    );
    if (rc != Z_OK || bound == 0) {
        throw std::runtime_error("zlib compression failed");
    }
    compressed.resize(bound);
    const int factor = std::min(static_cast<int>(raw.size() / compressed.size()) + 1, 99);
    std::ostringstream prefix;
    prefix << "gz" << std::setw(2) << std::setfill('0') << factor;
    return prefix.str() + base64_encode(compressed);
}

std::optional<std::string> build_params_string(const std::string& op, const JsonValue& module) {
    if (const JsonValue* params_gz = module.get("params_gz")) {
        if (params_gz->type == JsonType::String && params_gz->text.rfind("gz", 0) == 0) {
            return params_gz->text;
        }
    }
    if (const JsonValue* params_hex = module.get("params_hex")) {
        if (params_hex->type == JsonType::String) {
            std::string value = trim(params_hex->text);
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return value;
        }
    }

    const JsonValue* params = module.get("params");
    if (!params || params->type != JsonType::Object) {
        return std::nullopt;
    }

    std::vector<unsigned char> raw = build_raw_params(op, *params);
    if (raw.empty() && !module_spec(op)) {
        return std::nullopt;
    }

    if (const JsonValue* expected = module.get("_expected_bytes")) {
        if (expected->type == JsonType::Number && json_int(expected) > 0 &&
            raw.size() != static_cast<std::size_t>(json_int(expected))) {
            return std::nullopt;
        }
    }

    std::string format;
    if (const JsonValue* force = module.get("format")) {
        if (force->type == JsonType::String) {
            format = force->text;
        }
    }
    if (format == "hex" || (format.empty() && uses_hex_by_default(op))) {
        return hex_encode(raw);
    }
    return dt_encode_gz(raw);
}

ModulePatch module_patch_from_json(const std::string& op, const JsonValue& module) {
    ModulePatch patch;
    patch.operation = op;
    if (module.type != JsonType::Object) {
        return patch;
    }

    for (const auto& item : module.object) {
        const std::string& key = item.first;
        const JsonValue& value = item.second;
        if (key == "params" || key == "params_hex" || key == "params_gz" ||
            key == "format" || key == "_expected_bytes") {
            continue;
        }
        if (value.is_scalar()) {
            set_patch_attr(patch, key, value.scalar_string());
        }
    }

    if (std::optional<std::string> params = build_params_string(op, module)) {
        set_patch_attr(patch, "params", *params);
    }
    return patch;
}

bool looks_like_json(const std::string& text, const std::string& path) {
    if (path.size() >= 5 && path.substr(path.size() - 5) == ".json") {
        return true;
    }
    for (char c : text) {
        if (std::isspace(static_cast<unsigned char>(c)) == 0) {
            return c == '{' || c == '[';
        }
    }
    return false;
}

std::optional<std::pair<std::size_t, std::size_t>> attr_value_range(
    const std::string& tag,
    const std::string& attr
) {
    std::size_t pos = 0;
    while ((pos = tag.find(attr, pos)) != std::string::npos) {
        if (!is_name_boundary(tag, pos)) {
            pos += attr.size();
            continue;
        }

        std::size_t cursor = pos + attr.size();
        while (cursor < tag.size() && std::isspace(static_cast<unsigned char>(tag[cursor])) != 0) {
            ++cursor;
        }
        if (cursor >= tag.size() || tag[cursor] != '=') {
            pos += attr.size();
            continue;
        }

        ++cursor;
        while (cursor < tag.size() && std::isspace(static_cast<unsigned char>(tag[cursor])) != 0) {
            ++cursor;
        }
        if (cursor >= tag.size() || (tag[cursor] != '"' && tag[cursor] != '\'')) {
            pos += attr.size();
            continue;
        }

        const char quote = tag[cursor];
        const std::size_t value_start = cursor + 1;
        const std::size_t value_end = tag.find(quote, value_start);
        if (value_end == std::string::npos) {
            return std::nullopt;
        }
        return std::make_pair(value_start, value_end);
    }
    return std::nullopt;
}

std::vector<XmpAttribute> parse_tag_attributes(const std::string& tag) {
    std::vector<XmpAttribute> attrs;
    std::size_t cursor = tag.find("<rdf:li");
    if (cursor == std::string::npos) {
        return attrs;
    }
    cursor += 7;

    while (cursor < tag.size()) {
        while (cursor < tag.size() && std::isspace(static_cast<unsigned char>(tag[cursor])) != 0) {
            ++cursor;
        }
        if (cursor >= tag.size() || tag[cursor] == '>' || tag[cursor] == '/') {
            break;
        }

        const std::size_t name_start = cursor;
        while (cursor < tag.size() && is_attr_name_char(tag[cursor])) {
            ++cursor;
        }
        if (name_start == cursor) {
            ++cursor;
            continue;
        }

        std::string name = tag.substr(name_start, cursor - name_start);
        while (cursor < tag.size() && std::isspace(static_cast<unsigned char>(tag[cursor])) != 0) {
            ++cursor;
        }
        if (cursor >= tag.size() || tag[cursor] != '=') {
            continue;
        }

        ++cursor;
        while (cursor < tag.size() && std::isspace(static_cast<unsigned char>(tag[cursor])) != 0) {
            ++cursor;
        }
        if (cursor >= tag.size() || (tag[cursor] != '"' && tag[cursor] != '\'')) {
            continue;
        }

        const char quote = tag[cursor++];
        const std::size_t value_start = cursor;
        const std::size_t value_end = tag.find(quote, value_start);
        if (value_end == std::string::npos) {
            break;
        }

        attrs.push_back({std::move(name), tag.substr(value_start, value_end - value_start)});
        cursor = value_end + 1;
    }

    return attrs;
}

std::optional<std::string> attr_value(const std::string& tag, const std::string& attr) {
    const auto range = attr_value_range(tag, attr);
    if (!range) {
        return std::nullopt;
    }
    return tag.substr(range->first, range->second - range->first);
}

std::string replace_or_insert_attr(std::string tag, const std::string& attr, const std::string& value) {
    const auto range = attr_value_range(tag, attr);
    if (range) {
        tag.replace(range->first, range->second - range->first, value);
        return tag;
    }

    std::size_t insert_pos = tag.rfind("/>");
    if (insert_pos == std::string::npos) {
        insert_pos = tag.rfind('>');
    }
    if (insert_pos == std::string::npos) {
        return tag;
    }

    tag.insert(insert_pos, " " + attr + "=\"" + value + "\"");
    return tag;
}

std::optional<std::pair<std::size_t, std::size_t>> history_seq_range(const std::string& document) {
    const std::size_t history_pos = document.find("<darktable:history");
    if (history_pos == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t seq_pos = document.find("<rdf:Seq>", history_pos);
    if (seq_pos == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t close_pos = document.find("</rdf:Seq>", seq_pos);
    if (close_pos == std::string::npos) {
        return std::nullopt;
    }
    return std::make_pair(seq_pos, close_pos);
}

std::size_t next_history_num(const std::string& document) {
    std::size_t next = 0;
    std::size_t pos = 0;
    while ((pos = document.find("darktable:num=", pos)) != std::string::npos) {
        const std::size_t quote_pos = document.find('"', pos);
        if (quote_pos == std::string::npos) {
            pos += 14;
            continue;
        }
        const std::size_t end_quote = document.find('"', quote_pos + 1);
        if (end_quote == std::string::npos) {
            break;
        }
        try {
            const auto value = static_cast<std::size_t>(
                std::stoul(document.substr(quote_pos + 1, end_quote - quote_pos - 1))
            );
            next = std::max(next, value + 1);
        } catch (...) {
        }
        pos = end_quote + 1;
    }
    return next;
}

void set_history_end(std::string& document, std::size_t value) {
    const std::string attr = "darktable:history_end=";
    const std::size_t pos = document.find(attr);
    if (pos == std::string::npos) {
        return;
    }

    const std::size_t quote_pos = document.find('"', pos);
    if (quote_pos == std::string::npos) {
        return;
    }
    const std::size_t end_quote = document.find('"', quote_pos + 1);
    if (end_quote == std::string::npos) {
        return;
    }
    document.replace(quote_pos + 1, end_quote - quote_pos - 1, std::to_string(value));
}

std::optional<std::size_t> history_seq_close_position(const std::string& document) {
    const auto range = history_seq_range(document);
    if (!range) {
        return std::nullopt;
    }
    return range->second;
}

std::optional<std::string> first_history_item_template(const std::string& document) {
    const auto range = history_seq_range(document);
    if (!range) {
        return std::nullopt;
    }

    const std::size_t start = document.find("<rdf:li", range->first);
    if (start == std::string::npos) {
        return std::nullopt;
    }
    if (start > range->second) {
        return std::nullopt;
    }
    const std::size_t end = document.find('>', start);
    if (end == std::string::npos || end > range->second) {
        return std::nullopt;
    }
    return document.substr(start, end - start + 1);
}

void apply_attrs(std::string& tag, const ModulePatch& patch) {
    for (const auto& attr : patch.attributes) {
        if (attr.name == "darktable:operation") {
            continue;
        }
        tag = replace_or_insert_attr(tag, attr.name, attr.value);
    }
}

} // namespace

std::vector<ModulePatch> parse_config(std::istream& input) {
    std::vector<ModulePatch> patches;
    std::string line;

    while (std::getline(input, line)) {
        line = trim(std::move(line));
        if (line.empty() || line.front() == '#') {
            continue;
        }

        std::istringstream row(line);
        ModulePatch patch;
        row >> patch.operation;
        if (patch.operation.empty()) {
            continue;
        }

        std::string token;
        while (row >> token) {
            const std::size_t eq = token.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            const std::string key = token.substr(0, eq);
            const std::string value = unquote(token.substr(eq + 1));
            set_patch_attr(patch, key, value);
        }

        patches.push_back(std::move(patch));
    }

    return patches;
}

std::vector<ModulePatch> parse_config_file(const std::string& path) {
    std::string text = read_text_file(path);
    if (!looks_like_json(text, path)) {
        std::istringstream input(text);
        return parse_config(input);
    }

    JsonParser parser(strip_json_fence(std::move(text)));
    const JsonValue root = parser.parse();
    const JsonValue* modules = find_modules_object(root);
    if (!modules || modules->type != JsonType::Object) {
        throw std::runtime_error("json config must be an object or contain a modules object");
    }

    std::vector<ModulePatch> patches;
    for (const auto& item : modules->object) {
        if (item.second.type == JsonType::Object) {
            patches.push_back(module_patch_from_json(item.first, item.second));
        }
    }
    return patches;
}

std::vector<XmpModule> extract_history_modules(const std::string& document) {
    std::vector<XmpModule> modules;
    const auto range = history_seq_range(document);
    if (!range) {
        return modules;
    }

    std::size_t pos = range->first;
    while ((pos = document.find("<rdf:li", pos)) != std::string::npos && pos < range->second) {
        const std::size_t end = document.find('>', pos);
        if (end == std::string::npos || end > range->second) {
            break;
        }

        const std::string tag = document.substr(pos, end - pos + 1);
        XmpModule module;
        module.attributes = parse_tag_attributes(tag);
        for (const auto& attr : module.attributes) {
            if (attr.name == "darktable:operation") {
                module.operation = attr.value;
                break;
            }
        }
        if (!module.operation.empty()) {
            modules.push_back(std::move(module));
        }

        pos = end + 1;
    }

    return modules;
}

std::string read_text_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("failed to open input file: " + path);
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

void write_text_file(const std::string& path, const std::string& content) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("failed to open output file: " + path);
    }
    file << content;
}

XmpPatcher::XmpPatcher(std::string document)
    : document_(std::move(document)) {
}

PatchSummary XmpPatcher::apply(const std::vector<ModulePatch>& patches) {
    PatchSummary summary;
    for (const auto& patch : patches) {
        if (patch.operation.empty()) {
            summary.skipped.push_back("<empty>");
            continue;
        }
        if (patch_operation(patch)) {
            summary.patched.push_back(patch.operation);
        } else if (insert_operation(patch)) {
            summary.inserted.push_back(patch.operation);
        } else {
            summary.skipped.push_back(patch.operation);
        }
    }
    return summary;
}

const std::string& XmpPatcher::document() const {
    return document_;
}

std::optional<XmpPatcher::TagMatch> XmpPatcher::find_operation_tag(
    const std::string& operation
) const {
    const auto range = history_seq_range(document_);
    if (!range) {
        return std::nullopt;
    }

    std::size_t pos = range->first;
    while ((pos = document_.find("<rdf:li", pos)) != std::string::npos && pos < range->second) {
        const std::size_t end = document_.find('>', pos);
        if (end == std::string::npos || end > range->second) {
            return std::nullopt;
        }

        const std::string tag = document_.substr(pos, end - pos + 1);
        const auto tag_operation = attr_value(tag, "darktable:operation");
        if (tag_operation && *tag_operation == operation) {
            return TagMatch{pos, end + 1, tag};
        }

        pos = end + 1;
    }
    return std::nullopt;
}

bool XmpPatcher::patch_operation(const ModulePatch& patch) {
    const auto match = find_operation_tag(patch.operation);
    if (!match) {
        return false;
    }

    std::string tag = match->tag;
    apply_attrs(tag, patch);
    document_.replace(match->start, match->end - match->start, tag);
    return true;
}

bool XmpPatcher::insert_operation(const ModulePatch& patch) {
    const auto insert_pos = history_seq_close_position(document_);
    const auto template_tag = first_history_item_template(document_);
    if (!insert_pos || !template_tag) {
        return false;
    }

    const std::size_t num = next_history_num(document_);
    std::string tag = *template_tag;
    tag = replace_or_insert_attr(tag, "darktable:num", std::to_string(num));
    tag = replace_or_insert_attr(tag, "darktable:operation", patch.operation);
    tag = replace_or_insert_attr(tag, "darktable:multi_name", "");
    tag = replace_or_insert_attr(tag, "darktable:multi_name_hand_edited", "0");
    tag = replace_or_insert_attr(tag, "darktable:multi_priority", "0");
    apply_attrs(tag, patch);

    document_.insert(*insert_pos, "\n     " + trim(std::move(tag)) + "\n");
    set_history_end(document_, num + 1);
    return true;
}

} // namespace filtrox
