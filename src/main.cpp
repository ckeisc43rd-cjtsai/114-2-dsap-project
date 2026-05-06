#include "xmp_patch.hpp"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace {

namespace fs = std::filesystem;

void print_usage(const char* program) {
    std::cerr
        << "Usage:\n"
        << "  " << program << " <input.xmp> <config.json|config.txt> <output.xmp>\n"
        << "  " << program << " --list-modules <input.xmp>\n"
        << "  " << program << " --benchmark <input.xmp> <config.json|config.txt> <iterations>\n\n"
        << "  " << program << " --render-preview <image> <base.xmp> <out-dir> <workers> <config1.json> [config2.json ...]\n"
        << "  " << program << " --render-full <image> <base.xmp> <config.json> <output.jpg>\n\n"
        << "Text config format:\n"
        << "  operation enabled=1 modversion=7 params=hex_or_gz_payload blendop_version=14\n";
}

void print_summary(const filtrox::PatchSummary& summary) {
    auto print_list = [](const std::string& label, const std::vector<std::string>& values) {
        if (values.empty()) {
            return;
        }
        std::cout << label << ": ";
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i != 0) {
                std::cout << ", ";
            }
            std::cout << values[i];
        }
        std::cout << '\n';
    };

    print_list("patched", summary.patched);
    print_list("inserted", summary.inserted);
    print_list("skipped", summary.skipped);
}

std::string shell_quote(const std::string& value) {
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

int run_command(const std::string& command) {
    const int rc = std::system(command.c_str());
    if (rc == -1) {
        return 127;
    }
#ifndef _WIN32
    if (WIFEXITED(rc)) {
        return WEXITSTATUS(rc);
    }
#endif
    return rc;
}

std::string env_or_default(const char* key, const std::string& fallback) {
    const char* value = std::getenv(key);
    if (value && *value) {
        return value;
    }
    return fallback;
}

std::string render_command(
    const std::string& image_path,
    const std::string& xmp_path,
    const std::string& output_path,
    bool preview,
    const std::string& config_dir
) {
    const std::string cli = env_or_default("DARKTABLE_CLI_PATH", "darktable-cli");

    const int preview_width = std::stoi(env_or_default("FILTROX_PREVIEW_WIDTH", "1200"));
    const int preview_height = std::stoi(env_or_default("FILTROX_PREVIEW_HEIGHT", "1200"));

    std::ostringstream command;
#ifndef _WIN32
    if (std::getenv("FILTROX_USE_XVFB")) {
        command << "xvfb-run -a ";
    }
#endif
    command
        << shell_quote(cli) << ' '
        << shell_quote(image_path) << ' '
        << shell_quote(xmp_path) << ' '
        << shell_quote(output_path);
    if (preview) {
        command << " --width " << preview_width << " --height " << preview_height;
    }
    command
        << " --core --configdir " << shell_quote(config_dir)
        << " --disable-opencl";
    return command.str();
}

filtrox::PatchSummary write_generated_xmp(
    const std::string& base_xmp_path,
    const std::string& config_path,
    const std::string& output_xmp_path
) {
    filtrox::XmpPatcher patcher(filtrox::read_text_file(base_xmp_path));
    const auto patches = filtrox::parse_config_file(config_path);
    const auto summary = patcher.apply(patches);
    filtrox::write_text_file(output_xmp_path, patcher.document());
    return summary;
}

struct RenderResult {
    std::string config_path;
    std::string xmp_path;
    std::string image_path;
    int exit_code = 0;
    long long elapsed_ms = 0;
};

RenderResult render_one(
    const std::string& input_image,
    const std::string& base_xmp,
    const std::string& config_path,
    const std::string& output_image,
    bool preview
) {
    const auto start = std::chrono::steady_clock::now();
    fs::path output_path(output_image);
    if (!output_path.parent_path().empty()) {
        fs::create_directories(output_path.parent_path());
    }
    const std::string output_xmp = output_path.replace_extension(".xmp").string();
    const std::string config_dir = (fs::temp_directory_path() / ("filtrox_dt_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()
    ))).string();
    fs::create_directories(config_dir);

    write_generated_xmp(base_xmp, config_path, output_xmp);
    const std::string command = render_command(input_image, output_xmp, output_image, preview, config_dir);
    const int rc = run_command(command);
    fs::remove_all(config_dir);
    const auto end = std::chrono::steady_clock::now();

    RenderResult result;
    result.config_path = config_path;
    result.xmp_path = output_xmp;
    result.image_path = output_image;
    result.exit_code = rc;
    result.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    return result;
}

std::vector<RenderResult> render_variations_parallel(
    const std::string& input_image,
    const std::string& base_xmp,
    const std::string& out_dir,
    int workers,
    const std::vector<std::string>& config_paths
) {
    fs::create_directories(out_dir);
    workers = std::max(1, std::min(workers, static_cast<int>(config_paths.size())));

    std::mutex mutex;
    std::size_t next = 0;
    std::vector<RenderResult> results(config_paths.size());
    std::vector<std::future<void>> futures;

    for (int worker = 0; worker < workers; ++worker) {
        futures.push_back(std::async(std::launch::async, [&]() {
            while (true) {
                std::size_t index = 0;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (next >= config_paths.size()) {
                        return;
                    }
                    index = next++;
                }
                const fs::path config(config_paths[index]);
                const fs::path output = fs::path(out_dir) /
                    ("variation_" + std::to_string(index + 1) + "_preview.jpg");
                results[index] = render_one(
                    input_image,
                    base_xmp,
                    config.string(),
                    output.string(),
                    true
                );
            }
        }));
    }

    for (auto& future : futures) {
        future.get();
    }
    return results;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "--list-modules") {
            const auto modules = filtrox::extract_history_modules(filtrox::read_text_file(argv[2]));
            for (const auto& module : modules) {
                std::cout << module.operation << " (" << module.attributes.size() << " attrs)\n";
                for (const auto& attr : module.attributes) {
                    std::cout << "  " << attr.name << "=" << attr.value << '\n';
                }
            }
            return 0;
        }

        if (argc == 5 && std::string(argv[1]) == "--benchmark") {
            const std::string input_path = argv[2];
            const std::string config_path = argv[3];
            const int iterations = std::stoi(argv[4]);
            if (iterations <= 0) {
                throw std::runtime_error("iterations must be positive");
            }

            const std::string source = filtrox::read_text_file(input_path);
            const auto patches = filtrox::parse_config_file(config_path);

            const auto start = std::chrono::steady_clock::now();
            std::size_t output_size = 0;
            for (int i = 0; i < iterations; ++i) {
                filtrox::XmpPatcher patcher(source);
                patcher.apply(patches);
                output_size += patcher.document().size();
            }
            const auto end = std::chrono::steady_clock::now();
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

            std::cout << "iterations: " << iterations << '\n';
            std::cout << "elapsed_us: " << elapsed.count() << '\n';
            std::cout << "avg_us: " << (elapsed.count() / static_cast<double>(iterations)) << '\n';
            std::cout << "output_bytes_checksum: " << output_size << '\n';
            return 0;
        }

        if (argc >= 7 && std::string(argv[1]) == "--render-preview") {
            const std::string input_image = argv[2];
            const std::string base_xmp = argv[3];
            const std::string out_dir = argv[4];
            const int workers = std::stoi(argv[5]);
            std::vector<std::string> configs;
            for (int i = 6; i < argc; ++i) {
                configs.emplace_back(argv[i]);
            }

            const auto start = std::chrono::steady_clock::now();
            const auto results = render_variations_parallel(input_image, base_xmp, out_dir, workers, configs);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start
            );
            for (const auto& result : results) {
                std::cout
                    << result.image_path
                    << " rc=" << result.exit_code
                    << " elapsed_ms=" << result.elapsed_ms
                    << " xmp=" << result.xmp_path
                    << '\n';
            }
            std::cout << "total_elapsed_ms: " << elapsed.count() << '\n';
            return 0;
        }

        if (argc == 6 && std::string(argv[1]) == "--render-full") {
            const auto result = render_one(argv[2], argv[3], argv[4], argv[5], false);
            std::cout
                << result.image_path
                << " rc=" << result.exit_code
                << " elapsed_ms=" << result.elapsed_ms
                << " xmp=" << result.xmp_path
                << '\n';
            return result.exit_code == 0 ? 0 : result.exit_code;
        }

        if (argc != 4) {
            print_usage(argv[0]);
            return 2;
        }

        const std::string input_path = argv[1];
        const std::string config_path = argv[2];
        const std::string output_path = argv[3];

        filtrox::XmpPatcher patcher(filtrox::read_text_file(input_path));
        const auto patches = filtrox::parse_config_file(config_path);
        const auto summary = patcher.apply(patches);
        filtrox::write_text_file(output_path, patcher.document());

        std::cout << "wrote: " << output_path << '\n';
        print_summary(summary);
        return 0;
    } catch (const std::exception& err) {
        std::cerr << "error: " << err.what() << '\n';
        return 1;
    }
}
