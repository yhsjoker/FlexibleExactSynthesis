#ifndef FES_CORE_SPECIFICATION_H
#define FES_CORE_SPECIFICATION_H

#include <vector>
#include <cstdint>
#include <utility>

namespace fes {

    class Specification {
    public:
        int numInputs;
        int numOutputs;
        
        // 1. 逻辑目标: 真值表
        std::vector<uint64_t> truthTable; 

        // 2. 功耗目标: 输入信号概率 (Input Signal Probabilities)
        // 存储每个输入端口为 1 的概率 (0.0 ~ 1.0)
        // 对应 PONO 论文中的 P = (p1, p2, ..., pn)
        std::vector<double> inputProbabilities;

        Specification(int in, int out);

        // 设置真值表
        void setTruthTable(uint64_t tt);

        // 设置输入信号概率
        void setInputProbabilities(const std::vector<double>& probs);
    };

} // namespace fes

#endif // FES_CORE_SPECIFICATION_H