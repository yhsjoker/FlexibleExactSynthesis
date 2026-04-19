#ifndef FES_SOLVER_OMT_HEURISTIC_SOLVER_H
#define FES_SOLVER_OMT_HEURISTIC_SOLVER_H

#include "../interfaces/ISolver.h"
#include <z3++.h>
#include <vector>
#include <string>
#include <map>
#include <memory>

namespace fes {

class OmtHeuristicSolver : public ISolver {
public:
    OmtHeuristicSolver();
    ~OmtHeuristicSolver() override = default;

    Lit newVar() override;
    void addClause(const std::vector<Lit>& lits) override;

    void addGate(Lit out, const std::vector<Lit>& inputs, const GateType& gate) override;
    
    void addAnd(Lit out, Lit in1, Lit in2) override;
    void addOr(Lit out, Lit in1, Lit in2) override;
    void addXor(Lit out, Lit in1, Lit in2) override;
    void addNot(Lit out, Lit in) override;
    void addMux(Lit out, Lit sel, Lit inT, Lit inF) override;

    // ==========================================
    // 极速软约束接口 (MaxSAT Engine)
    // ==========================================
    void addOptimizationGoal(Lit condition, double weight) override;

    // 记录环境的初始输入概率
    void initProbabilities(const std::vector<double>& probs) override {
        node_expected_probs_ = probs; 
    }
    
    void addGateProbability(
        int gateId, 
        const std::vector<Lit>& typeLits, 
        const std::map<int, std::vector<Lit>>& selLitsMap,
        const std::vector<GateType>& lib
    ) override;

    void setTimeLimit(unsigned int limitMs) override;

    SolveStatus solve() override;
    bool getModelValue(Lit l) override;
    double getOptimizationResult() override;

private:
    z3::context context_;
    z3::optimize optimizer_;
    std::unique_ptr<z3::model> model_;
    std::vector<z3::expr> lit_exprs_;
    
    // 存储当前变体的输入概率，用于计算环境期望
    std::vector<double> node_expected_probs_;

    z3::expr lit2expr(Lit l);
};

} // namespace fes

#endif // FES_SOLVER_OMT_HEURISTIC_SOLVER_H