#include "fes/solvers/KissatSolver.h"
#include <iostream>
#include <vector>
#include <cmath>
#include <algorithm>

#ifdef HAS_KISSAT
extern "C" {
    #include "kissat.h"
}
#endif

namespace fes {

    // 回调函数
    static int kissat_terminate_callback(void* state) {
        auto* ts = static_cast<KissatSolver::TimeoutState*>(state);
        if (ts->limitMs == 0) return 0;
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - ts->startTime).count();
        return (elapsed > ts->limitMs) ? 1 : 0;
    }

    KissatSolver::KissatSolver() {
#ifdef HAS_KISSAT
        solver_ = kissat_init();
        timeout_state_.limitMs = 0;
#else
        solver_ = nullptr;
#endif
    }

    KissatSolver::~KissatSolver() {
#ifdef HAS_KISSAT
        if (solver_) kissat_release(solver_);
#endif
    }

    int KissatSolver::lit2int(Lit l) {
        int idx = l.var + 1;
        return l.isComplemented ? -idx : idx;
    }

    Lit KissatSolver::newVar() {
        int id = max_var_index_++;
        return Lit(id, false);
    }

    void KissatSolver::addClause(const std::vector<Lit>& lits) {
#ifdef HAS_KISSAT
        for (const auto& l : lits) kissat_add(solver_, lit2int(l));
        kissat_add(solver_, 0);
#endif
    }

    // ==========================================
    // ★ 关键修复：绝对通用的门逻辑转换 (Tseitin)
    // ==========================================
    void KissatSolver::addGate(Lit out, const std::vector<Lit>& inputs, const GateType& gate) {
        // 1. 简单门快捷路径 (可选)
        if (gate.name == "AND2" && inputs.size() == 2) { addAnd(out, inputs[0], inputs[1]); return; }
        if (gate.name == "OR2"  && inputs.size() == 2) { addOr(out, inputs[0], inputs[1]); return; }
        if (gate.name == "XOR2" && inputs.size() == 2) { addXor(out, inputs[0], inputs[1]); return; }
        if ((gate.name == "INV" || gate.name == "NOT") && inputs.size() == 1) { addNot(out, inputs[0]); return; }

        // 2. 通用算法
        // 确保遍历所有输入情况
        int num_entries = 1 << gate.numInputs; 

        for (int i = 0; i < num_entries; ++i) {
            bool expected_out = (gate.truthTable >> i) & 1;

            // 构建冲突子句: (Inputs == i) => (Out == expected)
            // 等价于: NOT(Inputs == i) OR (Out == expected)
            
            std::vector<Lit> clause;
            for (int j = 0; j < gate.numInputs; ++j) {
                // j 对应 inputs[j]
                bool input_val = (i >> j) & 1;
                
                // 如果当前模式下 Input[j] 为 1，则子句中加入 ~Input[j] (即 Input[j] 必须为 0 才能跳过此约束)
                if (input_val) {
                    clause.push_back(~inputs[j]);
                } else {
                    clause.push_back(inputs[j]);
                }
            }
            // 结果约束
            if (expected_out) clause.push_back(out);
            else              clause.push_back(~out);

            addClause(clause);
        }
    }

    // 快捷实现保持不变
    void KissatSolver::addAnd(Lit out, Lit in1, Lit in2) {
        addClause({~in1, ~in2, out}); addClause({in1, ~out}); addClause({in2, ~out});
    }
    void KissatSolver::addOr(Lit out, Lit in1, Lit in2) {
        addClause({~in1, out}); addClause({~in2, out}); addClause({in1, in2, ~out});
    }
    void KissatSolver::addXor(Lit out, Lit in1, Lit in2) {
        addClause({in1, in2, ~out}); addClause({~in1, ~in2, ~out});
        addClause({in1, ~in2, out}); addClause({~in1, in2, out});
    }
    void KissatSolver::addNot(Lit out, Lit in) {
        addClause({in, out}); addClause({~in, ~out});
    }
    void KissatSolver::addMux(Lit out, Lit sel, Lit inT, Lit inF) {
        GateType mux("MUX", 3, 0xCA, 0);
        addGate(out, {sel, inT, inF}, mux);
    }
    void KissatSolver::addOptimizationGoal(Lit c, double w) { (void)c; (void)w; }

    void KissatSolver::setTimeLimit(unsigned int limitMs) {
#ifdef HAS_KISSAT
        timeout_state_.limitMs = limitMs;
        timeout_state_.startTime = std::chrono::steady_clock::now();
        kissat_set_terminate(solver_, &timeout_state_, kissat_terminate_callback);
#endif
    }

    SolveStatus KissatSolver::solve() {
#ifdef HAS_KISSAT
        if (timeout_state_.limitMs > 0) timeout_state_.startTime = std::chrono::steady_clock::now();
        int res = kissat_solve(solver_);
        if (res == 10) {
            model_cache_.resize(max_var_index_ + 1);
            for (int i = 0; i < max_var_index_; ++i) {
                int val = kissat_value(solver_, i + 1);
                model_cache_[i] = (val > 0) ? 1 : 0;
            }
            return SolveStatus::SAT;
        } else if (res == 20) return SolveStatus::UNSAT;
        return SolveStatus::UNKNOWN;
#else
        return SolveStatus::UNKNOWN;
#endif
    }

    bool KissatSolver::getModelValue(Lit l) {
        if (l.var >= (int)model_cache_.size()) return false;
        bool val = (model_cache_[l.var] == 1);
        return l.isComplemented ? !val : val;
    }
    double KissatSolver::getOptimizationResult() { return 0.0; }
}