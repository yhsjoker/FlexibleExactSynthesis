#include "fes/solvers/Z3Solver.h"
#include <iostream>
#include <cmath>
#include <string>
#include <sstream>

namespace fes {

    // ==========================================
    // 构造与辅助函数
    // ==========================================

    Z3Solver::Z3Solver() 
        : optimizer_(context_), 
          total_cost_(context_.real_val(0)) 
    {
        // 配置 Z3 参数
        z3::params p(context_);
        p.set("priority", "box"); // box 策略通常比 pareto 更快收敛于单目标优化
        optimizer_.set(p);
    }

    z3::expr Z3Solver::lit2expr(Lit l) {
        if (l.var < 0 || l.var >= (int)vars_.size()) {
            std::ostringstream oss;
            oss << "Z3Solver: Variable index out of bounds: " << l.var 
                << " (size: " << vars_.size() << ")";
            throw std::runtime_error(oss.str());
        }
        
        z3::expr e = vars_[l.var];
        return l.isComplemented ? !e : e;
    }

    Lit Z3Solver::newVar() {
        int idx = (int)vars_.size();
        std::string name = "x" + std::to_string(idx);
        vars_.push_back(context_.bool_const(name.c_str()));
        return Lit(idx, false);
    }

    void Z3Solver::addClause(const std::vector<Lit>& lits) {
        if (lits.empty()) {
            optimizer_.add(context_.bool_val(false));
            return;
        }
        z3::expr_vector clause(context_);
        for (const auto& l : lits) {
            clause.push_back(lit2expr(l));
        }
        optimizer_.add(z3::mk_or(clause));
    }

    // ==========================================
    // 核心：通用门转换 (Truth Table -> Z3 Expr)
    // ==========================================

    void Z3Solver::addGate(Lit out, const std::vector<Lit>& inputs, const GateType& gate) {
        if (gate.name == "AND2" && inputs.size() == 2) { addAnd(out, inputs[0], inputs[1]); return; }
        if (gate.name == "OR2" && inputs.size() == 2) { addOr(out, inputs[0], inputs[1]); return; }
        if (gate.name == "XOR2" && inputs.size() == 2) { addXor(out, inputs[0], inputs[1]); return; }
        if (gate.name == "NOT" && inputs.size() == 1) { addNot(out, inputs[0]); return; }

        z3::expr_vector minterms(context_);
        int num_entries = 1 << gate.numInputs; 

        for (int i = 0; i < num_entries; ++i) {
            if ((gate.truthTable >> i) & 1) {
                z3::expr_vector term_lits(context_);
                for (int j = 0; j < gate.numInputs; ++j) {
                    bool input_must_be_one = (i >> j) & 1;
                    z3::expr input_var = lit2expr(inputs[j]);
                    
                    if (input_must_be_one) {
                        term_lits.push_back(input_var);
                    } else {
                        term_lits.push_back(!input_var);
                    }
                }
                minterms.push_back(z3::mk_and(term_lits));
            }
        }

        z3::expr logic = context_.bool_val(false); 
        if (!minterms.empty()) {
            logic = z3::mk_or(minterms); 
        }

        optimizer_.add(lit2expr(out) == logic);
    }

    void Z3Solver::addAnd(Lit out, Lit in1, Lit in2) { optimizer_.add(lit2expr(out) == (lit2expr(in1) && lit2expr(in2))); }
    void Z3Solver::addOr(Lit out, Lit in1, Lit in2) { optimizer_.add(lit2expr(out) == (lit2expr(in1) || lit2expr(in2))); }
    void Z3Solver::addXor(Lit out, Lit in1, Lit in2) { optimizer_.add(lit2expr(out) == (lit2expr(in1) != lit2expr(in2))); }
    void Z3Solver::addNot(Lit out, Lit in) { optimizer_.add(lit2expr(out) == (!lit2expr(in))); }
    void Z3Solver::addMux(Lit out, Lit sel, Lit inT, Lit inF) { optimizer_.add(lit2expr(out) == z3::ite(lit2expr(sel), lit2expr(inT), lit2expr(inF))); }

    // ==========================================
    // 功耗优化接口 (Optimization)
    // ==========================================

    void Z3Solver::addOptimizationGoal(Lit condition, double weight) {
        has_objective_ = true;
        z3::expr w_expr = context_.real_val(std::to_string(weight).c_str());
        z3::expr zero = context_.real_val(0);
        total_cost_ = total_cost_ + z3::ite(lit2expr(condition), w_expr, zero);
    }

    // ==========================================
    // ★ 求解与结果 (原生直接优化)
    // ==========================================

    SolveStatus Z3Solver::solve() {
        optimizer_.push();

        SolveStatus result = SolveStatus::UNKNOWN;

        // 直接向 Z3 下达 Minimize 指令，将所有压力交给底层的 NLRA 引擎
        if (has_objective_) {
            optimizer_.minimize(total_cost_);
        }

        z3::check_result res = optimizer_.check();

        if (res == z3::sat) {
            model_ = std::make_unique<z3::model>(optimizer_.get_model());
            result = has_objective_ ? SolveStatus::OPTIMAL : SolveStatus::SAT;
        } else if (res == z3::unsat) {
            result = SolveStatus::UNSAT;
        } else {
            // 当超时 (timeout) 触发返回 unknown 时，OMT 通常已经找到了一个局部最优的 SAT 解
            try {
                model_ = std::make_unique<z3::model>(optimizer_.get_model());
                result = SolveStatus::SAT; 
            } catch (...) {
                result = SolveStatus::UNKNOWN;
            }
        }

        optimizer_.pop();
        return result;
    }

    bool Z3Solver::getModelValue(Lit l) {
        if (!model_) throw std::runtime_error("Z3Solver: No model available. Call solve() first.");
        z3::expr val = model_->eval(lit2expr(l), true);
        return val.is_true();
    }

    double Z3Solver::getOptimizationResult() {
        if (!has_objective_ || !model_) return 0.0;
        
        z3::expr val = model_->eval(total_cost_, true);
        try {
            return std::stod(val.get_decimal_string(10)); 
        } catch (...) {
            return 0.0;
        }
    }

    // ==========================================
    // PONO 概率传播实现 (纯净符号化)
    // ==========================================

    void Z3Solver::initProbabilities(const std::vector<double>& probs) {
        probs_.clear();
        for (size_t i = 0; i < probs.size(); ++i) {
            probs_.insert({(int)i, context_.real_val(std::to_string(probs[i]).c_str())});
        }
    }

    void Z3Solver::setTimeLimit(unsigned int limitMs) {
        z3::params p(context_);
        p.set("timeout", limitMs);
        optimizer_.set(p);
    }

    void Z3Solver::addGateProbability(
        int gateId, 
        const std::vector<Lit>& typeLits, 
        const std::map<int, std::vector<Lit>>& selLitsMap,
        const std::vector<GateType>& lib
    ) {
        std::vector<z3::expr> input_probs;
        int maxInputs = (int)selLitsMap.size();

        for (int k = 0; k < maxInputs; ++k) {
            if (selLitsMap.find(k) == selLitsMap.end()) {
                 input_probs.push_back(context_.real_val(0)); 
                 continue;
            }

            const auto& sources = selLitsMap.at(k);
            z3::expr current_prob = context_.real_val(0);
            
            for (int src = 0; src < (int)sources.size(); ++src) {
                z3::expr sel = lit2expr(sources[src]);
                z3::expr p_src = probs_.at(src); 
                current_prob = z3::ite(sel, p_src, current_prob);
            }
            input_probs.push_back(current_prob);
        }

        z3::expr prob_out = context_.real_val(0);
        z3::expr gate_dynamic_cost = context_.real_val(0);
        z3::expr gate_static_cost = context_.real_val(0);
        z3::expr gate_area_cost = context_.real_val(0);

        for (int t = 0; t < (int)lib.size(); ++t) {
            z3::expr type_sel = lit2expr(typeLits[t]);
            const auto& gate = lib[t];
            
            int num_inputs = gate.numInputs;
            int num_entries = 1 << num_inputs;
            uint64_t tt = gate.truthTable;
            z3::expr p_logic = context_.real_val(0);

            for (int i = 0; i < num_entries; ++i) {
                if ((tt >> i) & 1) { 
                    z3::expr minterm_prob = context_.real_val(1);
                    for (int j = 0; j < num_inputs; ++j) {
                        bool input_val_is_1 = (i >> j) & 1;
                        z3::expr p_in = (j < (int)input_probs.size()) ? input_probs[j] : context_.real_val(0);
                        
                        if (input_val_is_1) {
                            minterm_prob = minterm_prob * p_in;
                        } else {
                            minterm_prob = minterm_prob * (1 - p_in);
                        }
                    }
                    p_logic = p_logic + minterm_prob;
                }
            }

            z3::expr alpha_t = 2 * p_logic * (1 - p_logic); 
            double load_factor = 1.0;

            z3::expr t_area = context_.real_val(std::to_string(gate.area).c_str());
            z3::expr t_static = context_.real_val(std::to_string(gate.staticPower).c_str());
            z3::expr t_lf = context_.real_val(std::to_string(load_factor * 20.0).c_str()); 
            
            z3::expr dynamic_p_t = alpha_t * t_area * t_lf;

            prob_out = z3::ite(type_sel, p_logic, prob_out);
            gate_dynamic_cost = z3::ite(type_sel, dynamic_p_t, gate_dynamic_cost);
            gate_static_cost = z3::ite(type_sel, t_static, gate_static_cost);
            gate_area_cost = z3::ite(type_sel, t_area, gate_area_cost);
        }

        probs_.insert({gateId, prob_out});

        double weight_leakage = 1.0;
        double weight_dynamic = 1.0;
        double weight_area = 5.0; 

        z3::expr wl = context_.real_val(std::to_string(weight_leakage).c_str());
        z3::expr wd = context_.real_val(std::to_string(weight_dynamic).c_str());
        z3::expr wa = context_.real_val(std::to_string(weight_area).c_str());

        total_cost_ = total_cost_ + (wl * gate_static_cost) + (wd * gate_dynamic_cost);
        has_objective_ = true; 
    }
        
    static double calculateStaticProb(const GateType& gate) {
        int num_entries = 1 << gate.numInputs;
        int ones = 0;
        for(int i=0; i<num_entries; ++i) {
            if((gate.truthTable >> i) & 1) ones++;
        }
        return (double)ones / (double)num_entries;
    }
} // namespace fes