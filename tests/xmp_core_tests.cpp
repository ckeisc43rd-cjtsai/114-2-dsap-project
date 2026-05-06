#include "xmp_patch.hpp"

#include <cassert>
#include <optional>
#include <sstream>
#include <string>

namespace {

const char* kSampleXmp = R"(<?xml version="1.0" encoding="UTF-8"?>
<x:xmpmeta>
 <rdf:RDF>
  <rdf:Description darktable:history_end="2">
   <darktable:history>
    <rdf:Seq>
     <rdf:li
      darktable:num="0"
      darktable:operation="exposure"
      darktable:enabled="1"
      darktable:modversion="7"
      darktable:params="old_exposure"
      darktable:multi_name=""
      darktable:multi_name_hand_edited="0"
      darktable:multi_priority="0"/>
     <rdf:li darktable:num="1" darktable:operation="sigmoid" darktable:enabled="1" darktable:modversion="3" darktable:params="old_sigmoid"/>
    </rdf:Seq>
   </darktable:history>
  </rdf:Description>
 </rdf:RDF>
</x:xmpmeta>
)";

std::optional<std::string> attr_value(
    const std::vector<filtrox::XmpAttribute>& attrs,
    const std::string& name
) {
    for (const auto& attr : attrs) {
        if (attr.name == name) {
            return attr.value;
        }
    }
    return std::nullopt;
}

const filtrox::ModulePatch* find_patch(
    const std::vector<filtrox::ModulePatch>& patches,
    const std::string& operation
) {
    for (const auto& patch : patches) {
        if (patch.operation == operation) {
            return &patch;
        }
    }
    return nullptr;
}

void test_patch_existing_operation() {
    std::istringstream config("exposure enabled=0 modversion=8 params=new_exposure\n");
    filtrox::XmpPatcher patcher(kSampleXmp);
    const auto summary = patcher.apply(filtrox::parse_config(config));
    const auto& out = patcher.document();

    assert(summary.patched.size() == 1);
    assert(summary.patched[0] == "exposure");
    assert(out.find("darktable:operation=\"exposure\"") != std::string::npos);
    assert(out.find("darktable:enabled=\"0\"") != std::string::npos);
    assert(out.find("darktable:modversion=\"8\"") != std::string::npos);
    assert(out.find("darktable:params=\"new_exposure\"") != std::string::npos);
}

void test_insert_missing_operation() {
    std::istringstream config("temperature enabled=1 modversion=4 params=010203\n");
    filtrox::XmpPatcher patcher(kSampleXmp);
    const auto summary = patcher.apply(filtrox::parse_config(config));
    const auto& out = patcher.document();

    assert(summary.inserted.size() == 1);
    assert(summary.inserted[0] == "temperature");
    assert(out.find("darktable:operation=\"temperature\"") != std::string::npos);
    assert(out.find("darktable:num=\"2\"") != std::string::npos);
    assert(out.find("darktable:history_end=\"3\"") != std::string::npos);
}

void test_extract_modules_from_sample() {
    const auto modules = filtrox::extract_history_modules(kSampleXmp);

    assert(modules.size() == 2);
    assert(modules[0].operation == "exposure");
    assert(*attr_value(modules[0].attributes, "darktable:num") == "0");
    assert(*attr_value(modules[0].attributes, "darktable:params") == "old_exposure");
    assert(modules[1].operation == "sigmoid");
    assert(*attr_value(modules[1].attributes, "darktable:modversion") == "3");
}

void test_parse_multiple_rows() {
    std::istringstream config(R"(
# prototype filter
exposure enabled=1 params=abcdef
sigmoid modversion=3 darktable:blendop_version=14 params="0102"
)");
    const auto patches = filtrox::parse_config(config);

    assert(patches.size() == 2);
    assert(patches[0].operation == "exposure");
    assert(*attr_value(patches[0].attributes, "darktable:enabled") == "1");
    assert(*attr_value(patches[0].attributes, "darktable:params") == "abcdef");
    assert(patches[1].operation == "sigmoid");
    assert(*attr_value(patches[1].attributes, "darktable:modversion") == "3");
    assert(*attr_value(patches[1].attributes, "darktable:blendop_version") == "14");
    assert(*attr_value(patches[1].attributes, "darktable:params") == "0102");
}

void test_extract_modules_from_hem_example() {
    const std::string hem = filtrox::read_text_file("examples/hem_sample.xmp");
    const auto modules = filtrox::extract_history_modules(hem);

    assert(modules.size() == 14);
    assert(modules[0].operation == "colorin");
    assert(modules[4].operation == "colorbalancergb");
    assert(*attr_value(modules[4].attributes, "darktable:num") == "4");
    assert(*attr_value(modules[4].attributes, "darktable:enabled") == "1");
    assert(attr_value(modules[4].attributes, "darktable:blendop_params").has_value());
    assert(modules[13].operation == "grain");
}

void test_json_config_generates_darktable_params() {
    const auto patches = filtrox::parse_config_file("examples/prototype_filter.json");

    assert(patches.size() == 10);
    const auto* colorbalance = find_patch(patches, "colorbalancergb");
    const auto* exposure = find_patch(patches, "exposure");
    const auto* temperature = find_patch(patches, "temperature");
    const auto* diffuse = find_patch(patches, "diffuse");
    const auto* hazeremoval = find_patch(patches, "hazeremoval");

    assert(colorbalance);
    assert(attr_value(colorbalance->attributes, "darktable:params")->rfind("gz", 0) == 0);
    assert(exposure);
    assert(
        *attr_value(exposure->attributes, "darktable:params") ==
        "000000008fc2753c" "cdcccc3d" "00004842" "000080c0" "00000000" "00000000"
    );
    assert(temperature);
    assert(
        *attr_value(temperature->attributes, "darktable:params") ==
        "0ad7833f" "5c8f823f" "48e17a3f" "0000807f" "02000000"
    );
    assert(diffuse);
    assert(attr_value(diffuse->attributes, "darktable:params").has_value());
    assert(hazeremoval);
    assert(attr_value(hazeremoval->attributes, "darktable:params").has_value());
}

} // namespace

int main() {
    test_patch_existing_operation();
    test_insert_missing_operation();
    test_extract_modules_from_sample();
    test_parse_multiple_rows();
    test_extract_modules_from_hem_example();
    test_json_config_generates_darktable_params();
    return 0;
}
