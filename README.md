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

### 目前進度
<!-- 完成了什麼 -->

### 遇到的困難
<!-- 遇到什麼問題、如何解決或打算如何解決 -->

### 下一步計畫
<!-- 接下來要做什麼 -->

### 與課程的關聯
<!-- 到目前為止，你的實作中哪些部分與課程內容有關？關係是什麼？ -->

---

## Final Report

### 專案說明
<!-- 完整描述你的專案做了什麼 -->

### 使用方式
<!-- 如何編譯、執行、使用你的程式 -->

### 與課程的關聯總結
<!-- 總結你的專題與進階程式設計及資料結構課程之間的關聯 -->