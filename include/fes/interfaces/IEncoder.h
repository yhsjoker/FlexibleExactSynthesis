#ifndef FES_INTERFACES_IENCODER_H
#define FES_INTERFACES_IENCODER_H

#include "ISolver.h"
#include "../core/Specification.h"
#include "../core/GateType.h"
#include "../core/CircuitGraph.h"

namespace fes {

    class IEncoder {
    public:
        virtual ~IEncoder() = default;

        /**
         * @brief 核心编码方法
         * 1. 建立电路拓扑变量
         * 2. 建立逻辑约束 (保证真值表正确)
         * 3. 建立功耗约束 (Pattern-based Optimization)
         * * @param solver 目标求解器
         * @param spec 输入规范
         * @param library 门库
         * @return bool 编码是否成功 (有些 Spec 可能无法用该库实现)
         */
        virtual bool encode(ISolver* solver, 
                            const Specification& spec, 
                            const std::vector<GateType>& library) = 0;

        /**
         * @brief 解码方法
         * 当 solve() 返回 SAT/OPTIMAL 后，调用此方法重建电路
         */
        virtual CircuitGraph decode(ISolver* solver) = 0;
        
        // 获取本次编码使用的门数量 (用于迭代加深)
        virtual int getNumGates() const = 0;
        
        // 设置门数量 (用于外部控制循环)
        virtual void setNumGates(int n) = 0;
    };

} // namespace fes

#endif // FES_INTERFACES_IENCODER_H