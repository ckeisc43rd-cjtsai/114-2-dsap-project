#include "xmp_patch.hpp"

#include <algorithm>
#include <cerrno>
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
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;

std::mutex timing_mutex;

bool truthy_env(const char* key) {
    const char* value = std::getenv(key);
    if (!value || !*value) {
        return false;
    }
    const std::string text(value);
    return text == "1" || text == "true" || text == "TRUE" ||
        text == "yes" || text == "YES" || text == "on" || text == "ON";
}

bool timing_enabled() {
    return truthy_env("FILTROX_TIMING");
}

bool reuse_configdir_enabled() {
    return truthy_env("FILTROX_REUSE_CONFIGDIR");
}

template <typename Start, typename End>
long long elapsed_us(Start start, End end) {
    return std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
}

void timing_log(const std::string& label, long long us) {
    if (!timing_enabled()) {
        return;
    }
    std::lock_guard<std::mutex> lock(timing_mutex);
    std::cerr << "[timing] " << label << ": " << us << " us\n";
}

void print_usage(const char* program) {
    std::cerr
        << "Usage:\n"
        << "  " << program << " <input.xmp> <config.json|config.txt> <output.xmp>\n"
        << "  " << program << " --list-modules <input.xmp>\n"
        << "  " << program << " --benchmark <input.xmp> <config.json|config.txt> <iterations>\n\n"
        << "  " << program << " --render-preview <image> <base.xmp> <out-dir> <workers> <config1.json> [config2.json ...]\n"
        << "  " << program << " --render-full <image> <base.xmp> <config.json> <output.jpg>\n\n"
        << "  " << program << " --render-batch-full <workers> <manifest.tsv>\n\n"
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

int run_process(const std::vector<std::string>& args) {
    if (args.empty()) {
        return 127;
    }
#ifdef _WIN32
    std::ostringstream command;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i != 0) {
            command << ' ';
        }
        command << shell_quote(args[i]);
    }
    const int rc = std::system(command.str().c_str());
    if (rc == -1) {
        return 127;
    }
    return rc;
#else
    const pid_t pid = fork();
    if (pid < 0) {
        return 127;
    }
    if (pid == 0) {
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return 127;
        }
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return status;
#endif
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

std::string make_temp_config_dir(const std::string& prefix) {
    const std::string path = (fs::temp_directory_path() / (prefix + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()
    ))).string();
    fs::create_directories(path);
    return path;
}

std::vector<std::string> render_args(
    const std::string& image_path,
    const std::string& xmp_path,
    const std::string& output_path,
    bool preview,
    const std::string& config_dir
) {
    const std::string cli = env_or_default("DARKTABLE_CLI_PATH", "darktable-cli");

    const int preview_width = std::stoi(env_or_default("FILTROX_PREVIEW_WIDTH", "900"));
    const int preview_height = std::stoi(env_or_default("FILTROX_PREVIEW_HEIGHT", "900"));

    std::vector<std::string> args;
#ifndef _WIN32
    if (std::getenv("FILTROX_USE_XVFB")) {
        args.push_back("xvfb-run");
        args.push_back("-a");
    }
#endif
    args.push_back(cli);
    args.push_back(image_path);
    args.push_back(xmp_path);
    args.push_back(output_path);
    if (preview) {
        args.push_back("--width");
        args.push_back(std::to_string(preview_width));
        args.push_back("--height");
        args.push_back(std::to_string(preview_height));
    }
    args.push_back("--core");
    args.push_back("--configdir");
    args.push_back(config_dir);
    args.push_back("--disable-opencl");
    return args;
}

filtrox::PatchSummary write_generated_xmp(
    const std::string& base_xmp_path,
    const std::string& config_path,
    const std::string& output_xmp_path
) {
    const auto t0 = std::chrono::steady_clock::now();
    std::string source = filtrox::read_text_file(base_xmp_path);
    const auto t1 = std::chrono::steady_clock::now();
    const auto patches = filtrox::parse_config_file(config_path);
    const auto t2 = std::chrono::steady_clock::now();
    filtrox::XmpPatcher patcher(std::move(source));
    const auto summary = patcher.apply(patches);
    const auto t3 = std::chrono::steady_clock::now();
    filtrox::write_text_file(output_xmp_path, patcher.document());
    const auto t4 = std::chrono::steady_clock::now();

    timing_log("cpp.xmp.read_base " + output_xmp_path, elapsed_us(t0, t1));
    timing_log("cpp.xmp.parse_config " + output_xmp_path, elapsed_us(t1, t2));
    timing_log("cpp.xmp.patch " + output_xmp_path, elapsed_us(t2, t3));
    timing_log("cpp.xmp.write " + output_xmp_path, elapsed_us(t3, t4));
    timing_log("cpp.xmp.total " + output_xmp_path, elapsed_us(t0, t4));
    return summary;
}

struct RenderResult {
    std::string config_path;
    std::string xmp_path;
    std::string image_path;
    int exit_code = 0;
    long long elapsed_ms = 0;
};

struct RenderJob {
    std::string input_image;
    std::string base_xmp;
    std::string config_path;
    std::string output_image;
};

struct ScopedDirectory {
    explicit ScopedDirectory(std::string path)
        : path(std::move(path)) {
    }

    ~ScopedDirectory() {
        if (!path.empty()) {
            fs::remove_all(path);
        }
    }

    std::string path;
};

RenderResult render_one(
    const std::string& input_image,
    const std::string& base_xmp,
    const std::string& config_path,
    const std::string& output_image,
    bool preview,
    const std::string& reusable_config_dir = ""
) {
    const auto start = std::chrono::steady_clock::now();
    fs::path output_path(output_image);
    if (!output_path.parent_path().empty()) {
        fs::create_directories(output_path.parent_path());
    }
    const std::string output_xmp = output_path.replace_extension(".xmp").string();
    const bool owns_config_dir = reusable_config_dir.empty();
    const std::string config_dir = owns_config_dir
        ? make_temp_config_dir("filtrox_dt_")
        : reusable_config_dir;
    fs::create_directories(config_dir);
    const auto after_setup = std::chrono::steady_clock::now();

    write_generated_xmp(base_xmp, config_path, output_xmp);
    const auto after_xmp = std::chrono::steady_clock::now();
    const auto args = render_args(input_image, output_xmp, output_image, preview, config_dir);
    const int rc = run_process(args);
    const auto after_render = std::chrono::steady_clock::now();
    if (owns_config_dir) {
        fs::remove_all(config_dir);
    }
    const auto end = std::chrono::steady_clock::now();

    timing_log("cpp.render.setup " + output_image, elapsed_us(start, after_setup));
    timing_log("cpp.render.xmp " + output_image, elapsed_us(after_setup, after_xmp));
    timing_log("cpp.render.darktable " + output_image, elapsed_us(after_xmp, after_render));
    timing_log("cpp.render.cleanup " + output_image, elapsed_us(after_render, end));
    timing_log("cpp.render.total " + output_image, elapsed_us(start, end));

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
    const bool reuse_configdir = reuse_configdir_enabled();

    for (int worker = 0; worker < workers; ++worker) {
        futures.push_back(std::async(std::launch::async, [&, reuse_configdir]() {
            const std::string worker_config_dir = reuse_configdir
                ? make_temp_config_dir("filtrox_dt_worker_")
                : "";
            ScopedDirectory cleanup(worker_config_dir);
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
                    true,
                    worker_config_dir
                );
            }
        }));
    }

    for (auto& future : futures) {
        future.get();
    }
    return results;
}

std::vector<std::string> split_tabs(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (start <= line.size()) {
        const std::size_t end = line.find('\t', start);
        if (end == std::string::npos) {
            fields.push_back(line.substr(start));
            break;
        }
        fields.push_back(line.substr(start, end - start));
        start = end + 1;
    }
    return fields;
}

std::vector<RenderJob> read_render_manifest(const std::string& manifest_path) {
    const auto start = std::chrono::steady_clock::now();
    std::ifstream file(manifest_path);
    if (!file) {
        throw std::runtime_error("failed to open render manifest: " + manifest_path);
    }

    std::vector<RenderJob> jobs;
    std::string line;
    std::size_t line_no = 0;
    while (std::getline(file, line)) {
        ++line_no;
        if (line.empty() || line.front() == '#') {
            continue;
        }

        const auto fields = split_tabs(line);
        if (fields.size() != 4) {
            throw std::runtime_error("invalid render manifest line " + std::to_string(line_no));
        }
        jobs.push_back({fields[0], fields[1], fields[2], fields[3]});
    }
    const auto end = std::chrono::steady_clock::now();
    timing_log("cpp.batch.read_manifest " + manifest_path, elapsed_us(start, end));
    return jobs;
}

std::vector<RenderResult> render_jobs_parallel(
    const std::vector<RenderJob>& jobs,
    int workers,
    bool preview
) {
    const auto start = std::chrono::steady_clock::now();
    if (jobs.empty()) {
        return {};
    }
    workers = std::max(1, std::min(workers, static_cast<int>(jobs.size())));

    std::mutex mutex;
    std::size_t next = 0;
    std::vector<RenderResult> results(jobs.size());
    std::vector<std::future<void>> futures;
    const bool reuse_configdir = reuse_configdir_enabled();

    for (int worker = 0; worker < workers; ++worker) {
        futures.push_back(std::async(std::launch::async, [&, reuse_configdir]() {
            const std::string worker_config_dir = reuse_configdir
                ? make_temp_config_dir("filtrox_dt_worker_")
                : "";
            ScopedDirectory cleanup(worker_config_dir);
            while (true) {
                std::size_t index = 0;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (next >= jobs.size()) {
                        return;
                    }
                    index = next++;
                }

                const auto& job = jobs[index];
                results[index] = render_one(
                    job.input_image,
                    job.base_xmp,
                    job.config_path,
                    job.output_image,
                    preview,
                    worker_config_dir
                );
            }
        }));
    }

    for (auto& future : futures) {
        future.get();
    }
    const auto end = std::chrono::steady_clock::now();
    timing_log("cpp.render_jobs_parallel.total jobs=" + std::to_string(jobs.size()), elapsed_us(start, end));
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

        if (argc == 4 && std::string(argv[1]) == "--render-batch-full") {
            const int workers = std::stoi(argv[2]);
            const auto jobs = read_render_manifest(argv[3]);
            const auto start = std::chrono::steady_clock::now();
            const auto results = render_jobs_parallel(jobs, workers, false);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start
            );

            int max_rc = 0;
            for (const auto& result : results) {
                std::cout
                    << result.image_path
                    << " rc=" << result.exit_code
                    << " elapsed_ms=" << result.elapsed_ms
                    << " xmp=" << result.xmp_path
                    << '\n';
                max_rc = std::max(max_rc, result.exit_code);
            }
            std::cout << "total_elapsed_ms: " << elapsed.count() << '\n';
            return max_rc == 0 ? 0 : max_rc;
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
