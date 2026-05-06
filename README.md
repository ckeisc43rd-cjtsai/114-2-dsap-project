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

### Relation to the Course
This project is closely related to both data structures and advanced programming. The main connections are listed below.

1. **String and sequence processing**  
XMP generation involves a large amount of string handling, field assembly, and output ordering. Reducing unnecessary copies and avoiding inefficient concatenation strategies are important to performance.

2. **Data structure selection**  
If the generation logic frequently looks up fields, tags, or rules, the choice between `map`, `unordered_map`, and `vector` directly affects lookup cost, insertion behavior, and implementation complexity.

3. **Algorithm and complexity analysis**  
During refactoring, I need to identify repeated scans and redundant computations, then determine whether they can be replaced with more efficient approaches.

4. **Modular design and abstraction**  
Advanced programming is not only about making a program work. It also includes designing clear module boundaries, reducing coupling, and improving maintainability. In this project, I plan to separate the parser, data representation, XMP builder, and benchmark or testing components.

5. **Performance measurement as engineering practice**  
This project is not only about writing a working implementation. It is also about explaining why the C++ version is faster, and how the data structure and implementation choices contribute to that result.

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
```

Current local results:

- Unit tests: `1/1 tests passed`
- JSON-to-XMP output patches 10 modules: `colorbalancergb`, `colorequal`, `exposure`, `sigmoid`, `toneequal`, `temperature`, `diffuse`, `hazeremoval`, `vignette`, and `grain`
- The real `_HEM` XMP is parsed as 14 modules, with each module keeping its ordered darktable attributes.
- Correctness check: with the same JSON config, C++ output and `xmp_gen.py` output are byte-for-byte identical.
- JSON-to-XMP benchmark with 10000 iterations: C++ averages about `44.2681 us` per iteration, while the Python baseline averages about `552.201 us` per iteration. In this benchmark, the C++ version is about `12.5x` faster.
- Render benchmark: `run.sh` will compare the C++ parallel preview renderer with the current sequential Python/filtrox render path when `darktable-cli` is available.

### Difficulties
The hardest part so far is not writing C++ itself, but figuring out where the real bottleneck actually is. At the beginning, it was tempting to assume that XMP string patching was the main problem. After rereading the original repo, I realized the full pipeline has several possible bottlenecks: JSON parsing, struct packing, zlib/base64 encoding, XMP patching, Darktable startup time, and rendering multiple variations one by one.

I still have not fully confirmed the real problem yet. For now, the prototype is mainly a tool for checking different parts of the Python workflow and collecting more useful timing data.

### Next Steps
Next I want to use the benchmark results to decide what is actually worth optimizing.

1. Add more real filtrox JSON configs and XMP files, so the benchmarks are not based on only one sample.
2. Continue tracing the whole Python flow, including AI output handling, JSON-to-XMP generation, preview rendering, full-resolution rendering, file I/O, and repeated Darktable startup, to find where the real bottleneck is.

### Course Connection
This project connects to data structures and advanced programming in a fairly direct way.

The XMP file is basically a large structured string. The program has to scan it, find the right module, locate attributes, and replace only the correct value without damaging the rest of the metadata. That is a practical string and sequence processing problem.

The module data is stored with `vector` because order matters in darktable history. For future optimization, I plan to add a lookup structure such as `unordered_map<string, range>` so repeated module lookup can be reduced from repeated scans to table lookup.

The encoding part also relates to low-level data representation: 32-bit floats, 32-bit integers, byte order, compression, and base64. This is a good fit for advanced programming because correctness depends on matching the binary layout exactly.

Finally, the render workflow introduces concurrency. Rendering the three preview variations is independent work, so the C++ version can dispatch them to 2-3 workers while keeping full-resolution rendering explicit and separate.

---

## Final Report

### 專案說明
<!-- 完整描述你的專案做了什麼 -->

### 使用方式
<!-- 如何編譯、執行、使用你的程式 -->

### 與課程的關聯總結
<!-- 總結你的專題與進階程式設計及資料結構課程之間的關聯 -->
