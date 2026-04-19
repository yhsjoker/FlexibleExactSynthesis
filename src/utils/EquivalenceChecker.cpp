#include "fes/utils/EquivalenceChecker.h"

#include <z3++.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace fes {

    namespace {

        // Build a Z3 expression for each node of `graph`, rooted at the
        // supplied PI expressions. Returns one expression per primary output
        // (positional). Ghost gates (unresolved typeIdx) are modeled as
        // constant false — matches SynthesisFlow::verify and BlifWriter.
        std::vector<z3::expr> buildGraphExprs(
            z3::context& ctx,
            const CircuitGraph& graph,
            const std::vector<z3::expr>& pi_exprs,
            const std::vector<GateType>& library,
            const EquivalenceChecker& owner,
            int (EquivalenceChecker::*resolver)(const std::string&) const)
        {
            std::unordered_map<int, z3::expr> node_exprs;
            const auto& pis = graph.getInputs();
            if (pis.size() != pi_exprs.size()) {
                throw std::runtime_error(
                    "EquivalenceChecker: PI count mismatch between graph and "
                    "symbolic inputs.");
            }
            for (size_t i = 0; i < pis.size(); ++i) {
                node_exprs.emplace(pis[i], pi_exprs[i]);
            }

            const z3::expr ctx_false = ctx.bool_val(false);

            // Walk nodes in id order; CircuitGraph stores gates by increasing
            // id and PatternEncoder guarantees fanins precede their consumer.
            for (const auto& kv : graph.getAllNodes()) {
                const Node& node = kv.second;
                switch (node.type) {
                    case NodeType::INPUT:
                        // Already populated from pi_exprs.
                        break;
                    case NodeType::CONST0:
                        node_exprs.emplace(node.id, ctx_false);
                        break;
                    case NodeType::CONST1:
                        node_exprs.emplace(node.id, ctx.bool_val(true));
                        break;
                    case NodeType::GATE: {
                        int typeIdx = (owner.*resolver)(node.gateType);
                        if (typeIdx < 0 || typeIdx >= (int)library.size()) {
                            // Ghost / unresolved gate -> constant 0 sink.
                            node_exprs.emplace(node.id, ctx_false);
                            break;
                        }
                        const GateType& gate = library[typeIdx];
                        const int ni = gate.numInputs;
                        const int limit = std::min(ni, (int)node.fanins.size());

                        // Gather input expressions for this gate; missing
                        // fanins (shouldn't happen in a well-formed graph)
                        // default to false.
                        std::vector<z3::expr> gate_inputs;
                        gate_inputs.reserve(ni);
                        for (int k = 0; k < limit; ++k) {
                            auto it = node_exprs.find(node.fanins[k]);
                            if (it == node_exprs.end()) {
                                gate_inputs.push_back(ctx_false);
                            } else {
                                gate_inputs.push_back(it->second);
                            }
                        }
                        for (int k = limit; k < ni; ++k) {
                            gate_inputs.push_back(ctx_false);
                        }

                        // Build OR-of-minterms using the project convention:
                        //   bit i of truthTable is the output for minterm i,
                        //   bit j of i is input[j].
                        const int num_entries = 1 << ni;
                        z3::expr_vector minterms(ctx);
                        for (int i = 0; i < num_entries; ++i) {
                            if (!((gate.truthTable >> i) & 1ULL)) continue;
                            z3::expr_vector term(ctx);
                            for (int j = 0; j < ni; ++j) {
                                bool want_one = ((i >> j) & 1) != 0;
                                term.push_back(want_one ? gate_inputs[j]
                                                        : !gate_inputs[j]);
                            }
                            minterms.push_back(term.empty() ? ctx.bool_val(true)
                                                            : z3::mk_and(term));
                        }
                        z3::expr out_expr = minterms.empty()
                                            ? ctx_false
                                            : z3::mk_or(minterms);
                        node_exprs.emplace(node.id, out_expr);
                        break;
                    }
                }
            }

            std::vector<z3::expr> outputs;
            outputs.reserve(graph.getOutputs().size());
            for (int out_id : graph.getOutputs()) {
                auto it = node_exprs.find(out_id);
                if (it == node_exprs.end()) {
                    outputs.push_back(ctx_false);
                } else {
                    outputs.push_back(it->second);
                }
            }
            return outputs;
        }

        z3::expr truthTableToExpr(
            z3::context& ctx,
            uint64_t truthTable,
            const std::vector<z3::expr>& pi_exprs)
        {
            const int ni = (int)pi_exprs.size();
            const int num_entries = 1 << ni;
            z3::expr_vector minterms(ctx);
            for (int i = 0; i < num_entries; ++i) {
                if (!((truthTable >> i) & 1ULL)) continue;
                z3::expr_vector term(ctx);
                for (int j = 0; j < ni; ++j) {
                    bool want_one = ((i >> j) & 1) != 0;
                    term.push_back(want_one ? pi_exprs[j] : !pi_exprs[j]);
                }
                minterms.push_back(term.empty() ? ctx.bool_val(true)
                                                : z3::mk_and(term));
            }
            if (minterms.empty()) return ctx.bool_val(false);
            return z3::mk_or(minterms);
        }

        uint64_t parseHex(const std::string& h) {
            uint64_t r = 0;
            std::stringstream ss;
            ss << std::hex << h;
            ss >> r;
            return r;
        }

    } // namespace

    EquivalenceChecker::EquivalenceChecker(
        const std::vector<GateType>& library)
        : library_(library) {}

    int EquivalenceChecker::resolveTypeIndex(
        const std::string& gateType) const
    {
        if (gateType.empty()) return -1;

        // PatternEncoder emits "GateType_<idx>". Strip the trailing number.
        size_t last_underscore = gateType.find_last_of('_');
        if (last_underscore != std::string::npos
            && last_underscore + 1 < gateType.length())
        {
            try {
                int idx = std::stoi(gateType.substr(last_underscore + 1));
                if (idx >= 0 && idx < (int)library_.size()) return idx;
            } catch (...) {
                // Fall through: try name-based resolution.
            }
        }

        // Fallback: gateType may be a direct library name (e.g. "AND2"),
        // which keeps this checker usable from hand-built graphs too.
        for (size_t i = 0; i < library_.size(); ++i) {
            if (library_[i].name == gateType) return (int)i;
        }
        return -1;
    }

    EquivalenceChecker::Result EquivalenceChecker::verifyAgainstTruthTable(
        uint64_t truthTable,
        int numInputs,
        const CircuitGraph& graph) const
    {
        Result r;
        int ni = (numInputs < 0) ? (int)graph.getInputs().size() : numInputs;
        if (ni <= 0 || ni > 16) {
            r.message = "EquivalenceChecker: unsupported numInputs " +
                        std::to_string(ni);
            return r;
        }
        if (graph.getOutputs().size() != 1) {
            r.message = "EquivalenceChecker: truth-table check requires a "
                        "single primary output.";
            return r;
        }

        z3::context ctx;
        std::vector<z3::expr> pi_exprs;
        pi_exprs.reserve(ni);
        for (int i = 0; i < ni; ++i) {
            std::string name = "pi_" + std::to_string(i);
            pi_exprs.push_back(ctx.bool_const(name.c_str()));
        }

        std::vector<z3::expr> graph_outs;
        try {
            graph_outs = buildGraphExprs(ctx, graph, pi_exprs, library_, *this,
                                         &EquivalenceChecker::resolveTypeIndex);
        } catch (const std::exception& e) {
            r.message = std::string("buildGraphExprs failed: ") + e.what();
            return r;
        }

        z3::expr spec_expr = truthTableToExpr(ctx, truthTable, pi_exprs);
        z3::expr miter = spec_expr != graph_outs.front();

        z3::solver solver(ctx);
        solver.add(miter);
        auto res = solver.check();
        if (res == z3::unsat) {
            r.equivalent = true;
            r.message = "EQUIVALENT";
        } else if (res == z3::sat) {
            z3::model m = solver.get_model();
            std::stringstream ss;
            ss << "MISMATCH on assignment: ";
            for (int i = 0; i < ni; ++i) {
                bool v = m.eval(pi_exprs[i], true).is_true();
                ss << "pi_" << i << "=" << (v ? '1' : '0');
                if (i + 1 < ni) ss << ", ";
            }
            r.message = ss.str();
        } else {
            r.message = "Z3 returned UNKNOWN during CEC";
        }
        return r;
    }

    EquivalenceChecker::Result EquivalenceChecker::verifyAgainstHex(
        const std::string& hexFunc,
        const CircuitGraph& graph) const
    {
        const int ni = (int)graph.getInputs().size();
        return verifyAgainstTruthTable(parseHex(hexFunc), ni, graph);
    }

    // ---------------- BLIF-level CEC helpers ----------------------------

    namespace {

        struct BlifGate {
            std::vector<std::string> inputs;
            std::string output;
            // Cubes preserved verbatim: (pattern, outputChar). Pattern length
            // must match inputs.size(); outputChar is '0' or '1'.
            std::vector<std::pair<std::string, char>> cubes;
        };

        struct ParsedBlif {
            std::vector<std::string> primaryInputs;
            std::vector<std::string> primaryOutputs;
            std::vector<BlifGate> gates;  // In declaration order.
            std::string error;            // Empty on success.
        };

        // Append continuation lines terminated by `\` so each logical line
        // can be parsed as one statement. Mirrors readLineSafe in
        // InnovusBatchEvaluator.
        bool readLogicalLine(std::istream& is, std::string& out) {
            if (!std::getline(is, out)) return false;
            while (!out.empty()) {
                size_t last = out.find_last_not_of(" \r\n\t");
                if (last != std::string::npos && out[last] == '\\') {
                    out.erase(last);
                    std::string next;
                    if (std::getline(is, next)) {
                        out += ' ' + next;
                        continue;
                    }
                }
                break;
            }
            return true;
        }

        ParsedBlif parseBlif(const std::string& text) {
            ParsedBlif pb;
            std::stringstream is(text);
            std::string line;

            auto collectTokens = [](const std::string& s,
                                    std::vector<std::string>& out) {
                std::stringstream ss(s);
                std::string tok;
                while (ss >> tok) {
                    if (tok == "\\" || tok.empty()) continue;
                    out.push_back(tok);
                }
            };

            BlifGate* currentGate = nullptr;

            while (readLogicalLine(is, line)) {
                // Strip leading whitespace.
                size_t first = line.find_first_not_of(" \t\r\n");
                if (first == std::string::npos) continue;
                if (line[first] == '#') continue;
                std::string s = line.substr(first);

                if (s.compare(0, 7, ".inputs") == 0) {
                    collectTokens(s.substr(7), pb.primaryInputs);
                    currentGate = nullptr;
                } else if (s.compare(0, 8, ".outputs") == 0) {
                    collectTokens(s.substr(8), pb.primaryOutputs);
                    currentGate = nullptr;
                } else if (s.compare(0, 6, ".names") == 0) {
                    std::vector<std::string> toks;
                    collectTokens(s.substr(6), toks);
                    if (toks.empty()) {
                        pb.error = ".names with no signals";
                        return pb;
                    }
                    BlifGate g;
                    g.output = toks.back();
                    toks.pop_back();
                    g.inputs = std::move(toks);
                    pb.gates.push_back(std::move(g));
                    currentGate = &pb.gates.back();
                } else if (s[0] == '.') {
                    // Any other directive (.model, .end, .latch, .subckt, ...)
                    // ends the current .names block. Latches/subckts in
                    // combinational CEC would be an error.
                    if (s.compare(0, 6, ".latch") == 0 ||
                        s.compare(0, 7, ".subckt") == 0) {
                        pb.error = std::string("unsupported construct: ") +
                                   s.substr(0, s.find_first_of(" \t"));
                        return pb;
                    }
                    currentGate = nullptr;
                } else {
                    // Cube line for the current .names block.
                    if (!currentGate) continue;  // Stray content; ignore.
                    std::stringstream cs(s);
                    std::string cube, outTok;
                    cs >> cube;
                    if (!(cs >> outTok)) {
                        // Constant cube: "1" or "0" alone (zero-input node).
                        if (!currentGate->inputs.empty()) {
                            pb.error = "malformed cube line";
                            return pb;
                        }
                        if (cube == "0" || cube == "1") {
                            currentGate->cubes.emplace_back("", cube[0]);
                        }
                        continue;
                    }
                    if (outTok.size() != 1 ||
                        (outTok[0] != '0' && outTok[0] != '1')) {
                        pb.error = "cube output must be 0 or 1";
                        return pb;
                    }
                    if (cube.size() != currentGate->inputs.size()) {
                        pb.error = "cube width mismatch";
                        return pb;
                    }
                    currentGate->cubes.emplace_back(cube, outTok[0]);
                }
            }

            return pb;
        }

        // Translate a single .names block into a Z3 expression using the
        // pre-computed expressions for each of the gate's fanins. BLIF
        // convention: all cubes share the same output polarity; empty cube
        // set => the node is constant 0.
        z3::expr blifGateToExpr(
            z3::context& ctx,
            const BlifGate& g,
            const std::vector<z3::expr>& input_exprs)
        {
            const z3::expr ctx_false = ctx.bool_val(false);
            if (g.cubes.empty()) return ctx_false;

            char polarity = g.cubes.front().second;
            z3::expr_vector covered(ctx);
            for (const auto& pr : g.cubes) {
                const std::string& cube = pr.first;
                char outc = pr.second;
                if (outc != polarity) {
                    // Mixed polarity is not legal BLIF; skip defensively.
                    continue;
                }
                z3::expr_vector term(ctx);
                for (size_t j = 0; j < cube.size(); ++j) {
                    char c = cube[j];
                    if (c == '-') continue;
                    if (c == '1') term.push_back(input_exprs[j]);
                    else if (c == '0') term.push_back(!input_exprs[j]);
                }
                covered.push_back(term.empty() ? ctx.bool_val(true)
                                               : z3::mk_and(term));
            }
            z3::expr on_set = covered.empty() ? ctx_false : z3::mk_or(covered);
            return (polarity == '1') ? on_set : !on_set;
        }

        // Build an expression per primary output of `pb`, given a PI-name ->
        // Z3 expression map (shared across both sides of the miter). Returns
        // false and populates `error` if a referenced net is undefined.
        bool buildBlifOutputExprs(
            z3::context& ctx,
            const ParsedBlif& pb,
            const std::unordered_map<std::string, z3::expr>& pi_map,
            std::vector<z3::expr>& out_exprs,
            std::string& error)
        {
            std::unordered_map<std::string, z3::expr> net_expr;
            net_expr.reserve(pi_map.size() + pb.gates.size());
            for (const auto& kv : pi_map) net_expr.emplace(kv.first, kv.second);

            for (const auto& g : pb.gates) {
                std::vector<z3::expr> ins;
                ins.reserve(g.inputs.size());
                for (const auto& in_name : g.inputs) {
                    auto it = net_expr.find(in_name);
                    if (it == net_expr.end()) {
                        error = "undefined net referenced: " + in_name;
                        return false;
                    }
                    ins.push_back(it->second);
                }
                net_expr.emplace(g.output, blifGateToExpr(ctx, g, ins));
            }

            out_exprs.clear();
            out_exprs.reserve(pb.primaryOutputs.size());
            for (const auto& po : pb.primaryOutputs) {
                auto it = net_expr.find(po);
                if (it == net_expr.end()) {
                    // BLIF allows a PO to be driven only by being declared;
                    // treat as constant 0 (matches ABC's implicit sink).
                    out_exprs.push_back(ctx.bool_val(false));
                } else {
                    out_exprs.push_back(it->second);
                }
            }
            return true;
        }

    } // namespace

    EquivalenceChecker::Result EquivalenceChecker::areBlifContentsEquivalent(
        const std::string& blifA,
        const std::string& blifB) const
    {
        Result r;
        ParsedBlif a = parseBlif(blifA);
        if (!a.error.empty()) {
            r.message = "Parse failure on LHS BLIF: " + a.error;
            return r;
        }
        ParsedBlif b = parseBlif(blifB);
        if (!b.error.empty()) {
            r.message = "Parse failure on RHS BLIF: " + b.error;
            return r;
        }

        // Primary inputs: unordered match by name. Missing PIs on either
        // side are treated as free variables (shared with the other side)
        // so the miter still proves function equivalence even when one
        // pass sweeps a dangling input.
        std::vector<std::string> pi_names = a.primaryInputs;
        for (const auto& name : b.primaryInputs) {
            if (std::find(pi_names.begin(), pi_names.end(), name) ==
                pi_names.end()) {
                pi_names.push_back(name);
            }
        }

        // Primary outputs: compared positionally after sorting by name so
        // the two sides line up even if the rewriter reorders .outputs.
        if (a.primaryOutputs.size() != b.primaryOutputs.size()) {
            r.message = "PO count mismatch (" +
                        std::to_string(a.primaryOutputs.size()) + " vs " +
                        std::to_string(b.primaryOutputs.size()) + ")";
            return r;
        }

        z3::context ctx;
        std::unordered_map<std::string, z3::expr> pi_map;
        pi_map.reserve(pi_names.size());
        for (const auto& name : pi_names) {
            pi_map.emplace(name, ctx.bool_const(name.c_str()));
        }

        std::vector<z3::expr> a_outs, b_outs;
        std::string err;
        if (!buildBlifOutputExprs(ctx, a, pi_map, a_outs, err)) {
            r.message = "LHS build failed: " + err;
            return r;
        }
        if (!buildBlifOutputExprs(ctx, b, pi_map, b_outs, err)) {
            r.message = "RHS build failed: " + err;
            return r;
        }

        // Pair outputs by name. Each side's ordered list is the source of
        // truth; we match A's i-th PO against B's same-named PO.
        std::unordered_map<std::string, size_t> b_index;
        for (size_t i = 0; i < b.primaryOutputs.size(); ++i) {
            b_index.emplace(b.primaryOutputs[i], i);
        }

        z3::expr_vector disagreements(ctx);
        for (size_t i = 0; i < a.primaryOutputs.size(); ++i) {
            auto it = b_index.find(a.primaryOutputs[i]);
            if (it == b_index.end()) {
                r.message = "PO '" + a.primaryOutputs[i] +
                            "' missing from RHS BLIF";
                return r;
            }
            disagreements.push_back(a_outs[i] != b_outs[it->second]);
        }

        z3::expr miter = disagreements.empty()
                         ? ctx.bool_val(false)
                         : z3::mk_or(disagreements);

        z3::solver solver(ctx);
        solver.add(miter);
        auto res = solver.check();
        if (res == z3::unsat) {
            r.equivalent = true;
            r.message = "EQUIVALENT";
        } else if (res == z3::sat) {
            z3::model m = solver.get_model();
            std::stringstream ss;
            ss << "MISMATCH on assignment: ";
            bool first = true;
            for (const auto& kv : pi_map) {
                bool v = m.eval(kv.second, true).is_true();
                if (!first) ss << ", ";
                ss << kv.first << "=" << (v ? '1' : '0');
                first = false;
            }
            r.message = ss.str();
        } else {
            r.message = "Z3 returned UNKNOWN during BLIF CEC";
        }
        return r;
    }

    EquivalenceChecker::Result EquivalenceChecker::areBlifFilesEquivalent(
        const std::string& pathA,
        const std::string& pathB) const
    {
        Result r;
        auto slurp = [](const std::string& p, std::string& out) -> bool {
            std::ifstream ifs(p);
            if (!ifs.is_open()) return false;
            std::stringstream ss;
            ss << ifs.rdbuf();
            out = ss.str();
            return true;
        };
        std::string a, b;
        if (!slurp(pathA, a)) {
            r.message = "Cannot open LHS BLIF: " + pathA;
            return r;
        }
        if (!slurp(pathB, b)) {
            r.message = "Cannot open RHS BLIF: " + pathB;
            return r;
        }
        return areBlifContentsEquivalent(a, b);
    }

    EquivalenceChecker::Result EquivalenceChecker::areEquivalent(
        const CircuitGraph& lhs,
        const CircuitGraph& rhs) const
    {
        Result r;
        if (lhs.getInputs().size() != rhs.getInputs().size()) {
            r.message = "EquivalenceChecker: PI count mismatch between graphs.";
            return r;
        }
        if (lhs.getOutputs().size() != rhs.getOutputs().size()) {
            r.message = "EquivalenceChecker: PO count mismatch between graphs.";
            return r;
        }

        const int ni = (int)lhs.getInputs().size();
        z3::context ctx;
        std::vector<z3::expr> pi_exprs;
        pi_exprs.reserve(ni);
        for (int i = 0; i < ni; ++i) {
            std::string name = "pi_" + std::to_string(i);
            pi_exprs.push_back(ctx.bool_const(name.c_str()));
        }

        std::vector<z3::expr> lhs_outs;
        std::vector<z3::expr> rhs_outs;
        try {
            lhs_outs = buildGraphExprs(ctx, lhs, pi_exprs, library_, *this,
                                       &EquivalenceChecker::resolveTypeIndex);
            rhs_outs = buildGraphExprs(ctx, rhs, pi_exprs, library_, *this,
                                       &EquivalenceChecker::resolveTypeIndex);
        } catch (const std::exception& e) {
            r.message = std::string("buildGraphExprs failed: ") + e.what();
            return r;
        }

        z3::expr_vector disagreements(ctx);
        for (size_t i = 0; i < lhs_outs.size(); ++i) {
            disagreements.push_back(lhs_outs[i] != rhs_outs[i]);
        }
        z3::expr miter = disagreements.empty()
                         ? ctx.bool_val(false)
                         : z3::mk_or(disagreements);

        z3::solver solver(ctx);
        solver.add(miter);
        auto res = solver.check();
        if (res == z3::unsat) {
            r.equivalent = true;
            r.message = "EQUIVALENT";
        } else if (res == z3::sat) {
            z3::model m = solver.get_model();
            std::stringstream ss;
            ss << "MISMATCH on assignment: ";
            for (int i = 0; i < ni; ++i) {
                bool v = m.eval(pi_exprs[i], true).is_true();
                ss << "pi_" << i << "=" << (v ? '1' : '0');
                if (i + 1 < ni) ss << ", ";
            }
            r.message = ss.str();
        } else {
            r.message = "Z3 returned UNKNOWN during CEC";
        }
        return r;
    }

} // namespace fes
