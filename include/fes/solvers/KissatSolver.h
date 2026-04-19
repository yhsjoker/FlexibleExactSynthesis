#ifndef FES_SOLVERS_KISSATSOLVER_H
#define FES_SOLVERS_KISSATSOLVER_H

#include "../interfaces/ISolver.h"
#include <vector>
#include <map>
#include <chrono> // 【新增】用于时间计算

// 前向声明 Kissat 结构
struct kissat;

namespace fes {

    class KissatSolver : public ISolver {
    public:
        kissat* solver_;
        int max_var_index_ = 0;
        
        // 缓存模型结果
        std::vector<int> model_cache_; 

        // 【新增】超时控制状态结构
        struct TimeoutState {
            std::chrono::time_point<std::chrono::steady_clock> startTime;
            unsigned int limitMs = 0; // 0 表示不限制
        };
        TimeoutState timeout_state_;

        // 内部辅助
        int lit2int(Lit l);

    public:
        KissatSolver();
        ~KissatSolver() override;

        Lit newVar() override;
        void addClause(const std::vector<Lit>& lits) override;
        void addGate(Lit out, const std::vector<Lit>& inputs, const GateType& gate) override;

        // 快捷接口
        void addAnd(Lit out, Lit in1, Lit in2) override;
        void addOr(Lit out, Lit in1, Lit in2) override;
        void addXor(Lit out, Lit in1, Lit in2) override;
        void addNot(Lit out, Lit in) override;
        void addMux(Lit out, Lit sel, Lit inT, Lit inF) override;

        // PONO 接口 (Kissat 不支持优化，保持默认空实现即可，或显式忽略)
        void addOptimizationGoal(Lit condition, double weight) override;

        // 【新增】超时设置
        void setTimeLimit(unsigned int limitMs) override;

        SolveStatus solve() override;
        bool getModelValue(Lit l) override;
        double getOptimizationResult() override; 
    };

} // namespace fes

#endif // FES_SOLVERS_KISSATSOLVER_H