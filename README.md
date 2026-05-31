# Filtrox XMP Core Refactor

## Proposal Report

### Motivation and Goals
This project is based on [filtrox](https://github.com/jx06T/filtrox), a project I previously developed and presented in the SYSTEX Young Turing Program, where it received an second place award. It is a project focusing on the automatic generation of image filters. XMP sidecar files defined by darktable are used as a configuration to image editing. For this DSAP project, I want to revisit one of its core components: the XMP generation pipeline. My goal is to redesign and reimplement that logic in C++.

The original version already works correctly, but its performance can still be improved, especially in workloads involving repeated string processing, rule assembly, and large amounts of metadata handling. By rewriting the core logic in C++, I want to improve execution speed while keeping the output behavior consistent with the original implementation.

The main goals of this project are:

1. Analyze the bottlenecks in filtrox's XMP generation workflow.
2. Reimplement the core logic in C++ to reduce runtime overhead and unnecessary memory usage.
3. Preserve correctness while making the new version measurable and comparable against the original one.

This topic allows me to extend a previous real project while applying concepts from advanced programming and data structures in a practical refactoring task.

### Competitor Analysis
Before developing this project, we investigated the current status of two existing products in the market:

| | Perfect Corp YouCam | imagen-ai |
|---|---|---|
| **Function** | AI Color Correction | Applies a color grading profile to all images |
| **Input** | Any image | A set of images and a standard color profile |
| **Disadvantage** | Removes all stylistic elements, not customizable | Requires professional color grading expertise |

**Goal:** Create a product combining AI's low learning curve with the flexibility of professional color grading.

### Expected Features
The project is expected to include the following features:

1. Read input data and build the intermediate structures required for XMP generation.
2. Reimplement the core XMP generation workflow from filtrox with output compatible with the original version.
3. Optimize string concatenation, field lookup, and template assembly.
4. Provide basic test cases to verify that the refactored version produces correct results.
5. Provide benchmark results or runtime comparisons to show the improvement from the C++ implementation.

If time permits, I would also like to add:

1. more modules that enhance filter performance
2. A cleaner rule interface for future extension.

### Technologies
- C++
- STL (`vector`, `string`, `unordered_map`, `map`)
- CMake or g++ for building
- Git and GitHub for version control and submission
- Testing and performance measurement tools such as `chrono`, profiling tools, or custom benchmarks

### Timeline
1. Before Week 7: finalize the project topic, complete the proposal README, and review the original filtrox XMP generation flow.
2. Week 8-9: break down the original logic and define the input format, data flow, and core data structures.
3. Week 10-11: complete the first working version of the C++ XMP generation core.
4. Week 12-13: add tests, fix behavioral differences, and optimize bottlenecks.
5. Week 14: complete benchmarking and summarize comparisons with the original version.
6. Week 15: finish the final report, demo video, and final cleanup.
---
## Prototype Report

### Progress
I have finished the first working C++ prototype. At first I only planned to rewrite the XMP patching part, but after rereading `filtrox/src/xmp_gen.py`, I changed the prototype to follow the real flow more closely: JSON config in, darktable module params generated as binary payloads, then XMP output.

What is working now:

1. Built an `xmp_core` C++ library and kept the CLI separate from the core logic.
2. Added `XmpModule`, `XmpAttribute`, `ModulePatch`, and `PatchSummary`, so each darktable module is stored as `module -> ordered attributes`.
3. Added JSON config input with the same `modules -> params` shape used by `xmp_gen.py`.
4. Reimplemented module parameter encoding in C++: float / int / array fields are packed into 32-bit binary, then encoded as either hex or `gzNN` + base64.
5. Supported patching existing darktable operations and inserting missing operations into the history list.
6. Added tests for text config parsing, JSON config parsing, XMP module extraction, XMP patching, and generated darktable payloads.
7. Added benchmark modes for JSON-to-XMP generation.
8. Added a Darktable render driver. Preview renders are low-res first, and the three variations can be rendered in parallel with 2-3 workers. Full-res rendering is only done through an explicit export/save command.

Main files:

- `src/xmp_patch.hpp` / `src/xmp_patch.cpp`: XMP generation and patching core
- `src/main.cpp`: CLI, benchmarks, and render driver
- `tests/xmp_core_tests.cpp`: unit tests
- `scripts/benchmark_python.py`: Python baseline for JSON-to-XMP generation
- `scripts/benchmark_render_python.py`: Python/filtrox render baseline
- `scripts/benchmark_filtrox_compare.py`: end-to-end comparison between the original `../filtrox` Python logic and the current C++ implementation in this project
- `examples/hem_sample.xmp`: real XMP copied from the `_HEM5577_20260303_115540` filtrox session
- `examples/hem_sample.jpeg`: test image for render experiments
- `examples/prototype_filter.json`: JSON config in the same style as `xmp_gen.py`
- `examples/variations/variation_1.json`, `variation_2.json`, `variation_3.json`: three render variation configs
- `CMakeLists.txt`: build setup

Build and test:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Example commands:

```bash
build/filtrox_xmp examples/hem_sample.xmp examples/prototype_filter.json build/hem_output.xmp
build/filtrox_xmp --list-modules examples/hem_sample.xmp
build/filtrox_xmp --benchmark examples/hem_sample.xmp examples/prototype_filter.json 10000
python3 scripts/benchmark_python.py examples/hem_sample.xmp examples/prototype_filter.json 10000
build/filtrox_xmp --render-preview examples/hem_sample.jpeg examples/hem_sample.xmp build/render_cpp_preview 3 examples/variations/variation_1.json examples/variations/variation_2.json examples/variations/variation_3.json
build/filtrox_xmp --render-full examples/hem_sample.jpeg examples/hem_sample.xmp examples/variations/variation_1.json build/export_full.jpg
python3 scripts/benchmark_render_python.py examples/hem_sample.jpeg examples/hem_sample.xmp build/render_python_preview examples/variations/variation_1.json examples/variations/variation_2.json examples/variations/variation_3.json
python3 scripts/benchmark_filtrox_compare.py --iterations 10000 --json-out build/benchmark_compare/xmp.json
python3 scripts/benchmark_filtrox_compare.py --render --workers 1 2 3 --darktable-cli /Applications/darktable.app/Contents/MacOS/darktable-cli --json-out build/benchmark_compare/render.json
```

The compare benchmark has two modes:

1. Default mode compares only JSON-to-XMP generation. This isolates the core algorithm and avoids darktable startup/render variance.
2. `--render` mode additionally compares the original sequential Python preview render path with the current C++ preview renderer. Pass multiple values to `--workers`, such as `--workers 1 2 3`, to measure how much parallel rendering helps. This mode requires `darktable-cli`.

Current local results:

- Unit tests: `1/1 tests passed`
- JSON-to-XMP output patches 10 modules: `colorbalancergb`, `colorequal`, `exposure`, `sigmoid`, `toneequal`, `temperature`, `diffuse`, `hazeremoval`, `vignette`, and `grain`
- The real `_HEM` XMP is parsed as 14 modules, with each module keeping its ordered darktable attributes.
- Correctness check: with the same JSON config, C++ output and `xmp_gen.py` output are byte-for-byte identical.
- JSON-to-XMP benchmark with 10000 iterations: C++ averages about 44.2681 us per iteration, while the Python baseline averages about 552.201 us per iteration. In this benchmark, the C++ version is about 12.5x faster.
- Render benchmark: `run.sh` will compare the C++ parallel preview renderer with the current sequential Python/filtrox render path when `darktable-cli` is available.

### Difficulties
The hardest part so far is not writing C++ itself, but figuring out where the real bottleneck actually is. At the beginning, it was tempting to assume that XMP string patching was the main problem. After rereading the original repo, I realized the full pipeline has several possible bottlenecks: JSON parsing, struct packing, zlib/base64 encoding, XMP patching, Darktable startup time, and rendering multiple variations one by one.

I still have not fully confirmed the real problem yet. For now, the prototype is mainly a tool for checking different parts of the Python workflow and collecting more useful timing data.

### Next Steps
Next I want to use the benchmark results to decide what is actually worth optimizing.

1. Add more real filtrox JSON configs and XMP files, so the benchmarks are not based on only one sample.
2. Continue tracing the whole Python flow, including AI output handling, JSON-to-XMP generation, preview rendering, full-resolution rendering, file I/O, and repeated Darktable startup, to find where the real bottleneck is.

---

## Final Report

### Project Description
This project is the final project for the DSAP (Data Structures and Advanced Programming) course. It focuses on a core refactoring of [filtrox](https://github.com/jx06T/filtrox), an automatic image filter generation project I previously developed. The original XMP file generation logic was fully implemented in Python. While functionally correct, it suffered from performance bottlenecks when handling extensive string concatenation and binary parameter packing (e.g., packing floats and integers into 32-bit structures and converting them to Hex or Base64).

To address this issue, this project reimplements the core **JSON-to-XMP generation engine** and the **Darktable render controller** in C++, achieving the following goals:
1. **Full Compatibility and Correctness**: The XMP files generated by the C++ version are byte-for-byte identical to those from the original Python version, accurately supporting over ten Darktable modules (including `colorbalancergb`, `toneequal`, `diffuse`, etc.).
2. **Significant Generation Performance Optimization**: By leveraging C++ memory management and high-performance string processing, the execution time for pure XMP generation was reduced from ~423 us to ~41 us per iteration, achieving an **over 10x speedup**.
3. **Parallel Rendering Support**: Implemented a mechanism that utilizes multiple workers to call `darktable-cli` concurrently for preview output. This breaks the sequential execution limits of the original Python version when rendering multiple filter variations, significantly reducing the overall waiting time for rendering.
4. **Algorithmic and Structural Comparison (Course Objective)**: As part of the core goals, we conducted a practical performance analysis on the rendering workflow. We compared the original sequential processing algorithm against a newly implemented parallel dispatching algorithm using C++ worker threads. Furthermore, we analyzed the performance differences between Python's dynamic string manipulations and C++'s structured binary packing (using `std::vector` and raw byte buffers). These comparisons demonstrated how appropriate algorithms and data structures drastically reduce multi-image rendering times and memory overhead.

### Usage
The project is built using CMake.

**1. Build the Project**
```bash
cmake -S . -B build
cmake --build build
```

**2. Run Unit Tests**
```bash
ctest --test-dir build --output-on-failure
```

**3. Basic Commands**
Using the compiled `filtrox_xmp` executable, you can directly patch XMP files and render images:
```bash
# List modules inside an XMP file
./build/filtrox_xmp --list-modules examples/hem_sample.xmp

# Generate the corresponding XMP file based on a JSON config
./build/filtrox_xmp examples/hem_sample.xmp examples/prototype_filter.json build/hem_output.xmp

# Render multiple preview images in parallel based on variation JSON configs (3 is the number of workers)
./build/filtrox_xmp --render-preview examples/hem_sample.jpeg examples/hem_sample.xmp build/render_cpp_preview 3 examples/variations/variation_1.json examples/variations/variation_2.json examples/variations/variation_3.json
```

**4. Run Benchmarks**
The project provides a Python script to compare performance with the original implementation (ensure `darktable-cli` is installed on your system):
```bash
# Compare XMP generation performance and parallel rendering performance with multiple workers
python3 scripts/benchmark_filtrox_compare.py \
  --iterations 100 \
  --render \
  --workers 1 2 4 8 \
  --darktable-cli /Applications/darktable.app/Contents/MacOS/darktable-cli
```

### Benchmark Results
We conducted XMP generation and parallel rendering performance tests using 10 complete filter modules (including `colorbalancergb`, `colorequal`, `exposure`, `sigmoid`, `toneequal`, `temperature`, `diffuse`, `hazeremoval`, `vignette`, `grain`). The results prove that the C++ implementation not only significantly improves XMP processing speed (approx. 10x speedup), but also substantially reduces the multi-image rendering time of Darktable through parallelization:

```bash
❯ python3 scripts/benchmark_filtrox_compare.py \
  --iterations 100 \
  --render \
  --workers 1 2 3 4 5 6 7 8 9 10\
  --darktable-cli /Applications/darktable.app/Contents/MacOS/darktable-cli \
  --json-out build/benchmark_compare/render.json
XMP-only benchmark
  original Python avg: 423.494 us
  current C++ avg:     41.470 us
  speedup:             10.21x
  byte-identical:      True

Render benchmark
  original Python sequential preview: 11133 ms
  current C++ preview workers=1: 11085 ms (1.00x)
  current C++ preview workers=2: 9434 ms (1.18x)
  current C++ preview workers=3: 8698 ms (1.28x)
  current C++ preview workers=4: 8167 ms (1.36x)
  current C++ preview workers=5: 9375 ms (1.19x)
  current C++ preview workers=6: 7340 ms (1.52x)
  current C++ preview workers=7: 7213 ms (1.54x)
  current C++ preview workers=8: 7163 ms (1.55x)
  current C++ preview workers=9: 6976 ms (1.60x)
  current C++ preview workers=10: 7330 ms (1.52x)

wrote JSON report: /Users/cjtsai/ntu/114-2/DSAP/fp/114-2-dsap-project/build/benchmark_compare/render.json
```

