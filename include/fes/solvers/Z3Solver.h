#ifndef FES_SOLVERS_Z3SOLVER_H
#define FES_SOLVERS_Z3SOLVER_H

#include "../interfaces/ISolver.h"
#include <z3++.h>
#include <vector>
#include <memory>

namespace fes {
    class Z3Solver : public ISolver {
    private:
        z3::context context_;
        z3::optimize optimizer_; // 支持最小化目标
        
        // 变量映射: VarIndex(int) -> z3::expr
        std::vector<z3::expr> vars_;
        
        // 目标函数累加器
        z3::expr total_cost_;
        bool has_objective_ = false;

        // 求解后的模型缓存
        std::unique_ptr<z3::model> model_;

        // 内部辅助: Lit 转 z3::expr
        z3::expr lit2expr(Lit l);

        std::map<int, z3::expr> probs_;

    public:
        Z3Solver();
        ~Z3Solver() override = default;

        // ISolver 接口实现
        Lit newVar() override;
        void addClause(const std::vector<Lit>& lits) override;
        
        void addGate(Lit out, const std::vector<Lit>& inputs, const GateType& gate) override;

        // 快捷接口覆盖 (使用 Z3 原生 API 加速)
        void addAnd(Lit out, Lit in1, Lit in2) override;
        void addOr(Lit out, Lit in1, Lit in2) override;
        void addXor(Lit out, Lit in1, Lit in2) override;
        void addNot(Lit out, Lit in) override;
        void addMux(Lit out, Lit sel, Lit inT, Lit inF) override;

        void addOptimizationGoal(Lit condition, double weight) override;
        SolveStatus solve() override;
        bool getModelValue(Lit l) override;
        double getOptimizationResult() override;

        void initProbabilities(const std::vector<double>& probs) override;

        void setTimeLimit(unsigned int limitMs) override;
        
        void addGateProbability(
            int gateId, 
            const std::vector<Lit>& typeLits, 
            const std::map<int, std::vector<Lit>>& selLitsMap,
            const std::vector<GateType>& lib
        ) override;
    };

} // namespace fes

#endif // FES_SOLVERS_Z3SOLVER_H