#ifndef FES_INTERFACES_ISOLVER_H
#define FES_INTERFACES_ISOLVER_H

#include "../core/Types.h"
#include "../core/GateType.h"
#include <vector>
#include <map>

namespace fes {
    enum class SolveMode {
        OPTIMIZE,           // 使用 Z3 内置的 optimizer.minimize()
        ITERATIVE           // 使用 while 循环手动添加 total_cost < current 约束
    };

    class ISolver {
    protected:
        SolveMode current_mode_ = SolveMode::OPTIMIZE;
    public:
        virtual ~ISolver() = default;

        // ==========================================
        // 1. 基础构建 (Basic Building Blocks)
        // ==========================================
        
        // 创建一个新的布尔变量
        virtual Lit newVar() = 0;

        // 添加 CNF 子句 (底层约束，所有Solver必须支持)
        virtual void addClause(const std::vector<Lit>& lits) = 0;

        // ==========================================
        // 2. 通用逻辑构建 (Universal Logic)
        // ==========================================
        
        /**
         * @brief 添加任意逻辑门约束 (核心接口)
         * Solver 必须根据 GateType 中的 truthTable 自动生成约束 (CNF 或 ITE)
         * @param out 输出变量
         * @param inputs 输入变量列表 (长度必须等于 gate.numInputs)
         * @param gate 门定义 (包含真值表)
         */
        virtual void addGate(Lit out, const std::vector<Lit>& inputs, const GateType& gate) = 0;

        // ==========================================
        // 3. 常用门快捷接口 (Shortcuts)
        // ==========================================
        // 这些接口存在是为了性能优化。
        // Z3Solver 可以用原生 API 实现它们以获得更快速度。
        // KissatSolver 可以通过硬编码的 CNF 模板实现它们。
        
        virtual void addAnd(Lit out, Lit in1, Lit in2) = 0;
        virtual void addOr(Lit out, Lit in1, Lit in2) = 0;
        virtual void addXor(Lit out, Lit in1, Lit in2) = 0;
        virtual void addNot(Lit out, Lit in) = 0;
        
        // 多路选择器: out = sel ? inT : inF
        virtual void addMux(Lit out, Lit sel, Lit inT, Lit inF) = 0;

        // ==========================================
        // 4. 优化与 SMT 接口 (Optimization)
        // ==========================================

        /**
         * @brief 添加最小化目标 (软约束)
         * 语义: Minimize sum( condition ? weight : 0 )
         * 用于 PONO 算法中的动态功耗优化。
         * SAT Solver 实现时可以选择忽略此函数。
         */
        virtual void addOptimizationGoal(Lit condition, double weight) = 0;

        // ==========================================
        // 5. 求解与查询 (Solving & Result)
        // ==========================================

        // 执行求解
        virtual SolveStatus solve() = 0;

        // 获取结果：查询变量 l 在模型中的值 (True/False)
        virtual bool getModelValue(Lit l) = 0;
        
        // 获取优化后的总代价值 (仅 Z3 有效)
        virtual double getOptimizationResult() = 0;

        void setSolveMode(SolveMode mode){
            current_mode_ = mode;
        };

        // ==========================================
        // 6. PONO 专用接口 (Probability Propagation)
        // ==========================================
        
        /**
         * @brief 初始化输入概率
         * @param probs 输入端口的信号概率 (0.0 - 1.0)
         */
        virtual void initProbabilities(const std::vector<double>& probs) {}

        /**
         * @brief 添加门的概率约束
         * 计算 gateId 的输出概率，并将其翻转率加入 Cost
         * * @param gateId 当前门的 ID
         * @param typeLits 类型选择变量 (One-hot)
         * @param selLitsMap 连接选择变量 (Key: InputSlot, Value: vector of source lits)
         * @param lib 门库
         */
        virtual void addGateProbability(
            int gateId, 
            const std::vector<Lit>& typeLits, 
            const std::map<int, std::vector<Lit>>& selLitsMap,
            const std::vector<GateType>& lib
        ) {}

        virtual void setTimeLimit(unsigned int limitMs) {}
    };

} // namespace fes

#endif // FES_INTERFACES_ISOLVER_H