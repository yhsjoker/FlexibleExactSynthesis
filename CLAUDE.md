# PONO Project Developer Guidelines (AI Assistant Constitution)

## Project Overview
This is a professional Electronic Design Automation (EDA) tool focusing on Power-Oriented Near-Optimal synthesis (PONO). The core workflow involves SMT-based sub-circuit library generation, logic rewriting, evaluation via Cadence Innovus, and generating experimental data for academic publication.

## Build & Test Commands
* **Build System:** CMake
* **Build Command:** `mkdir -p build && cd build && cmake .. && make -j$(nproc)`
* **Run Tests:** `cd build && ctest` (or run specific binary like `./build/pono_test`)

## C++ Coding Standards
* **Standard:** C++17
* **Style:** Follow Google C++ Style Guide.
* **Memory Management:** Strictly forbid raw pointers for ownership. Use `std::unique_ptr` and `std::shared_ptr`.
* **Magic Numbers:** DO NOT use hardcoded numbers for circuit structural parameters (like `4` for 4-input LUTs/gates). Use `constexpr`, `enum class`, or template parameters to allow easy scaling to 6-input or others.
* **Headers:** Always use `#pragma once` for include guards. Keep includes minimal to speed up compilation.

## EDA & Domain-Specific Rules (CRITICAL)
1. **Preserve Experimental Code (Academic Requirement):** This codebase is actively used for academic paper submissions. **DO NOT delete, overwrite, or disable any experiment-related code, benchmarking scripts, data parsers, or output generation logic.** All cleanup actions must explicitly bypass experimental infrastructure.
2. **Logic Equivalence:** Any logic rewriting, graph manipulation, or AST modification MUST preserve the original Boolean logic. A Combinational Equivalence Checking (CEC) step must back up structural changes.
3. **SMT Encoding for Gates:** When constructing SMT formulas for multi-input gates, strictly follow the project's encoding approach: **introduce a new variable to indicate pin activity**. DO NOT use the standard Knuth encoding.
4. **Truth Table Representation (`hex_func`):** Always ensure that the endianness and bit-order interpretation of hexadecimal truth tables are strictly consistent across all SMT, Graph, and rewriting modules.
5. **External Tools Integration:**
   * **Innovus:** TCL scripts generated for Innovus must cleanly handle process exits and error logs.
   * **Solvers & CEC:** For Combinational Equivalence Checking (CEC) and satisfiability, evaluate and select the MOST SUITABLE tool among Z3 (C++ API), Kissat (via DIMACS), OR ABC (via AIG/BLIF) based on the context (circuit size, existing data structures, performance needs). Do not implement all of them; just choose the best one and implement it well. Do not write custom SAT/SMT solving algorithms.
6. **Library Activity Pattern Generation (CRITICAL):**
   The library generation flow MUST support arbitrary per-pin switching activity vectors, not only uniform vectors where all pins share the same value.
   At minimum, preserve and support:
   - **Uniform sweep mode:** e.g. (0.1,0.1,0.1,0.1) to (0.9,0.9,0.9,0.9)
   - **Cartesian grid mode:** e.g. values from {0.2,0.4,0.6,0.8} enumerated across all pins in deterministic order
   The design MUST expose one clearly isolated function or strategy entry so users can manually customize pattern generation later without modifying the main pipeline.

7. **Backward Compatibility for Research Pipelines:**
   Any new CLI/config/library-generation refactor MUST preserve the current experimental behavior by default. Legacy behavior must remain reproducible via a compatibility mode or default config.

8. **Determinism & Reproducibility:**
   Any enumeration order for generated libraries, benchmarks, truth tables, and reports MUST be deterministic. If floating-point activity values are used, comparisons and serialization must use a documented tolerance and stable formatting.

9. **Benchmark Execution Robustness:**
   Timeout/failure cases MUST never be silently discarded. The evaluation pipeline should preserve partial results, produce resumable manifests, and support rerunning only failed or timed-out cases.

10. **Final CLI/Productization Scope Control:**
    During final encapsulation, do NOT perform broad logic rewrites to “clean up” algorithmic code unless necessary. Prioritize interface clarity, configuration management, deployment convenience, and documentation over destabilizing the core synthesis logic.