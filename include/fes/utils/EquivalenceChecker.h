#pragma once

#include "../core/CircuitGraph.h"
#include "../core/GateType.h"
#include <cstdint>
#include <string>
#include <vector>

namespace fes {

    // Combinational Equivalence Checker (CEC).
    //
    // Backend: Z3 C++ API. The checker builds a miter AST directly from
    // `CircuitGraph` nodes, reusing the project-wide truth-table convention
    // (bit m of tt = output for minterm m; bit j of m = input j). A miter is
    // declared equivalent iff the XOR of reference and candidate outputs is
    // UNSAT under Z3.
    //
    // Design notes:
    //   * Ghost gates emitted by PatternEncoder ("UNKNOWN" / unresolved
    //     typeIdx) are modeled as constant 0, matching SynthesisFlow::verify
    //     and BlifWriter::write so CEC agrees with simulation.
    //   * The checker is cheap to construct — it holds only a reference to
    //     the gate library and builds a fresh Z3 context per call.
    class EquivalenceChecker {
    public:
        struct Result {
            bool equivalent = false;
            std::string message;
        };

        explicit EquivalenceChecker(const std::vector<GateType>& library);

        // Verify that `graph` implements the single-output truth table given
        // by `truthTable` over `numInputs` PIs. `numInputs` defaults to the
        // graph's own PI count when set to -1.
        Result verifyAgainstTruthTable(uint64_t truthTable,
                                       int numInputs,
                                       const CircuitGraph& graph) const;

        // Convenience wrapper: parses a hex string (same format used
        // throughout the pipeline) and defers to verifyAgainstTruthTable.
        Result verifyAgainstHex(const std::string& hexFunc,
                                const CircuitGraph& graph) const;

        // Miter-based equivalence between two graphs. Their primary-input
        // counts must match; primary outputs are compared positionally.
        Result areEquivalent(const CircuitGraph& lhs,
                             const CircuitGraph& rhs) const;

        // BLIF-level equivalence. Both BLIF texts must share the same set
        // of primary-input names (matched by name, not position) and the
        // same ordered `.outputs` list. Each .names SOP block is turned
        // directly into a Z3 expression using BLIF's standard cover
        // semantics (all cubes share the output polarity of the first
        // cube; empty cube set => constant 0). Used by the logic-rewrite
        // pipeline to prove that rewritten BLIFs still implement the
        // original Boolean function.
        Result areBlifContentsEquivalent(const std::string& blifA,
                                         const std::string& blifB) const;
        Result areBlifFilesEquivalent(const std::string& pathA,
                                      const std::string& pathB) const;

    private:
        const std::vector<GateType>& library_;

        // Returns library index encoded in a gateType string ("GateType_N"
        // or library gate name), or -1 for a ghost / unresolved gate.
        int resolveTypeIndex(const std::string& gateType) const;
    };

} // namespace fes
