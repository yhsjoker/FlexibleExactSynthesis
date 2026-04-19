#include "fes/solvers/OmtHeuristicSolver.h"
#include <iostream>
#include <cmath>
#include <stdexcept>

namespace fes {

OmtHeuristicSolver::OmtHeuristicSolver() 
    : optimizer_(context_) 
{
    z3::params p(context_);
    p.set("priority", "box"); // 恢复 box 优先级，对 MaxSAT 软约束求解极快
    p.set("timeout", 10000u); 
    optimizer_.set(p);
}

void OmtHeuristicSolver::setTimeLimit(unsigned int limitMs) {
    z3::params p(context_);
    p.set("timeout", limitMs);
    optimizer_.set(p);
}

Lit OmtHeuristicSolver::newVar() {
    int var_id = (int)lit_exprs_.size();
    std::string name = "x_" + std::to_string(var_id);
    lit_exprs_.push_back(context_.bool_const(name.c_str()));
    return Lit(var_id, false);
}

z3::expr OmtHeuristicSolver::lit2expr(Lit l) {
    if (l.var >= (int)lit_exprs_.size()) throw std::runtime_error("Index out of bounds");
    z3::expr e = lit_exprs_[l.var];
    return l.isComplemented ? !e : e; 
}

void OmtHeuristicSolver::addClause(const std::vector<Lit>& lits) {
    if (lits.empty()) {
        optimizer_.add(context_.bool_val(false));
        return;
    }
    z3::expr clause = context_.bool_val(false);
    for (auto l : lits) clause = clause || lit2expr(l);
    optimizer_.add(clause);
}

void OmtHeuristicSolver::addAnd(Lit out, Lit in1, Lit in2) { optimizer_.add(lit2expr(out) == (lit2expr(in1) && lit2expr(in2))); }
void OmtHeuristicSolver::addOr(Lit out, Lit in1, Lit in2) { optimizer_.add(lit2expr(out) == (lit2expr(in1) || lit2expr(in2))); }
void OmtHeuristicSolver::addXor(Lit out, Lit in1, Lit in2) { optimizer_.add(lit2expr(out) == (lit2expr(in1) ^ lit2expr(in2))); }
void OmtHeuristicSolver::addNot(Lit out, Lit in) { optimizer_.add(lit2expr(out) == !lit2expr(in)); }
void OmtHeuristicSolver::addMux(Lit out, Lit sel, Lit inT, Lit inF) { optimizer_.add(lit2expr(out) == z3::ite(lit2expr(sel), lit2expr(inT), lit2expr(inF))); }

void OmtHeuristicSolver::addGate(Lit out, const std::vector<Lit>& inputs, const GateType& gate) {
    int num_inputs = gate.numInputs;
    int num_entries = 1 << num_inputs;
    uint64_t tt = gate.truthTable;

    z3::expr logic_expr = context_.bool_val(false);
    bool logic_created = false;

    for (int i = 0; i < num_entries; ++i) {
        if ((tt >> i) & 1) {
            z3::expr minterm = context_.bool_val(true);
            for (int j = 0; j < num_inputs; ++j) {
                bool bit_set = (i >> j) & 1;
                z3::expr in_expr = lit2expr(inputs[j]);
                minterm = minterm && (bit_set ? in_expr : !in_expr);
            }
            if (!logic_created) { logic_expr = minterm; logic_created = true; } 
            else { logic_expr = logic_expr || minterm; }
        }
    }
    
    if (!logic_created) optimizer_.add(!lit2expr(out)); 
    else optimizer_.add(lit2expr(out) == logic_expr);
}

// ==========================================
// 软约束接口
// ==========================================
void OmtHeuristicSolver::addOptimizationGoal(Lit condition, double weight) {
    // 放大权重以保留浮点精度
    int int_weight = static_cast<int>(weight * 1000); 
    if (int_weight > 0) {
        optimizer_.add(!lit2expr(condition), int_weight);
    }
}

// ====================================================================
// PONO 核心：基于统计期望的数据驱动功耗预计算
// ====================================================================
void OmtHeuristicSolver::addGateProbability(
    int gateId, 
    const std::vector<Lit>& typeLits, 
    const std::map<int, std::vector<Lit>>& selLitsMap,
    const std::vector<GateType>& lib
) {
    // 1. 提取当前环境的平均输入概率期望 E(P)
    double avg_p = 0.5;
    if (!node_expected_probs_.empty()) {
        double sum = 0;
        for (double p : node_expected_probs_) sum += p;
        avg_p = sum / node_expected_probs_.size();
    }

    // 2. 遍历库中所有的门类型，计算其在此环境概率下的真实预期成本
    for (size_t t = 0; t < lib.size(); ++t) {
        Lit sel = typeLits[t];
        const GateType& gate = lib[t];
        int num_inputs = gate.numInputs;
        
        double p_out = 0.0;

        // 处理常量门 (CONST0 / CONST1)
        if (num_inputs == 0) {
            p_out = (gate.truthTable & 1) ? 1.0 : 0.0;
        } 
        // 处理普通逻辑门：通过真值表和输入期望计算理论输出概率
        else {
            for (int i = 0; i < (1 << num_inputs); ++i) {
                if ((gate.truthTable >> i) & 1) {
                    double term = 1.0;
                    for (int j = 0; j < num_inputs; ++j) {
                        term *= ((i >> j) & 1) ? avg_p : (1.0 - avg_p);
                    }
                    p_out += term;
                }
            }
        }

        // 计算此门的翻转率
        double alpha = 2.0 * p_out * (1.0 - p_out);
        
        // =========================================================
        // 基于纯净物理模型的成本估算 (使用真实的 staticPower)
        // =========================================================
        
        // 1. 动态功耗：翻转率 * 负载电容(由面积代理) * 频率与电压权重
        // 这里的 dynamic_weight (例如 40.0) 是一个平衡因子，模拟 V^2 * f。
        // 你可以调整这个值：值越大，Z3 越倾向于优化高频翻转；值越小，Z3 越倾向于压榨面积和漏电。
        double dynamic_weight = 40.0; 
        double dyn_power = alpha * gate.area * dynamic_weight;
        
        // 2. 静态功耗：直接使用真实的物理参数
        double static_power = gate.staticPower; 
        
        // 3. 总功耗
        double total_cost = dyn_power + static_power;

        // 施加软约束：惩罚选择功耗大的门
        addOptimizationGoal(sel, total_cost);
    }
}

SolveStatus OmtHeuristicSolver::solve() {
    z3::check_result res = optimizer_.check();
    
    if (res == z3::sat) {
        model_ = std::make_unique<z3::model>(optimizer_.get_model());
        return SolveStatus::OPTIMAL;
    } 
    else if (res == z3::unknown) {
        try {
            model_ = std::make_unique<z3::model>(optimizer_.get_model());
            return SolveStatus::SAT; 
        } catch (...) {
            return SolveStatus::UNKNOWN;
        }
    } 
    return SolveStatus::UNSAT;
}

bool OmtHeuristicSolver::getModelValue(Lit l) {
    if (!model_) return false;
    return model_->eval(lit2expr(l), true).is_true();
}

double OmtHeuristicSolver::getOptimizationResult() {
    // 此时使用的是软约束罚分，外部仅作打印参考，返回 0 即可
    return 0.0;
}

} // namespace fes