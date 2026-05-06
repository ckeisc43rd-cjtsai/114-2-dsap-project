#pragma once

#include <cstddef>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace filtrox {

struct XmpAttribute {
    std::string name;
    std::string value;
};

struct ModulePatch {
    std::string operation;
    std::vector<XmpAttribute> attributes;
};

struct XmpModule {
    std::string operation;
    std::vector<XmpAttribute> attributes;
};

struct PatchSummary {
    std::vector<std::string> patched;
    std::vector<std::string> inserted;
    std::vector<std::string> skipped;
};

std::vector<ModulePatch> parse_config(std::istream& input);
std::vector<ModulePatch> parse_config_file(const std::string& path);
std::vector<XmpModule> extract_history_modules(const std::string& document);

std::string read_text_file(const std::string& path);
void write_text_file(const std::string& path, const std::string& content);

class XmpPatcher {
public:
    explicit XmpPatcher(std::string document);

    PatchSummary apply(const std::vector<ModulePatch>& patches);

    const std::string& document() const;

private:
    struct TagMatch {
        std::size_t start;
        std::size_t end;
        std::string tag;
    };

    std::string document_;

    std::optional<TagMatch> find_operation_tag(const std::string& operation) const;
    bool patch_operation(const ModulePatch& patch);
    bool insert_operation(const ModulePatch& patch);
};

} // namespace filtrox
