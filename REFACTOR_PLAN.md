# PONO Project Refactoring Plan

**Goal:** Refactor the research prototype into a production-ready, open-source level EDA tool while STRICTLY PRESERVING all experimental validation capabilities for academic publication.

## Stage 1: Indexing, Logic Alignment & CEC Framework
- [x] Index the entire codebase and analyze the data flow from SMT sub-circuit generation -> Logic Rewriting -> Innovus Evaluation.
- [x] Identify and mentally isolate all experimental/benchmarking code to ensure it is protected during refactoring.
- [x] Verify the consistency of `hex_func` (truth table) interpretation across all modules. Report any endianness or bit-order mismatch risks.
- [x] Build a robust Combinational Equivalence Checking (CEC) module (`EquivalenceChecker`).
- [x] Analyze the verification context (current graph representation, performance requirements) and CHOOSE the most appropriate backend solver (Z3, Kissat, OR ABC).
- [x] Implement ONLY the selected optimal backend for the CEC module.
- [x] Hook the CEC module into the logic rewriting flow with a `--verify` debug flag.

## Stage 2: Architecture Cleanup & Generalization (De-magic)
- [x] Safely remove `OmtHeuristicSolver.cpp/h` and all its dangling references (ensure `Z3Solver.cpp` handles everything properly). **Ensure no experimental scripts break due to this removal.**
- [x] Refactor `InnovusBatchEvaluator`: Analyze the two redundant logic rewriting paths, merge them, and retain a single, clean interface aligned with the core pipeline.
- [x] Eradicate magic numbers: Refactor the hardcoded gate input limits (e.g., `4`) into configuration constants or templates to easily support scalable inputs (like 6-input generation).

## Stage 3: Algorithm Performance Optimization
- [ ] Implement an NPN (Negation-Permutation-Negation) equivalence class judgment module to filter out redundant functions before SMT solving.
- [ ] Introduce multi-threading (via `std::thread` or OpenMP) to parallelize the sub-circuit library generation.
- [ ] Ensure thread safety (e.g., using `std::mutex` or lock-free structures) when threads write to the shared library database.

## Stage 4: Industrial Library Parsing & Decoupling
- [ ] Develop a parser module to read 45nm standard cell libraries (e.g., Liberty `.lib` format).
- [ ] Extract real physical metrics (area, power, pin capacitance) to replace current hard-coded gate models.
- [ ] Apply the Strategy Pattern to logic rewriting rules and heuristic evaluation functions, separating volatile logic from the stable core engine.

## Stage 5: Professional Encapsulation (CLI & Config)
- [ ] Integrate a modern argument parsing library (like `CLI11` or `cxxopts`).
- [ ] Build a unified command-line interface (CLI) supporting intuitive commands (e.g., `pono --build-lib --size 6`, `pono --optimize <file>`).
- [ ] Implement a unified configuration manager that parses run-time parameters (SMT timeout, input size, library paths) from a single YAML/JSON config file.
- [ ] Verify that all original experimental validation workflows can be successfully triggered using the newly built CLI/Config system.