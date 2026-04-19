#include "fes/utils/BlifWriter.h"
#include <fstream>
#include <iostream>
#include <bitset>

namespace fes {
    void BlifWriter::write(const std::string& filename, const std::string& modelName,
                           const CircuitGraph& graph, const std::vector<GateType>& library) {
        std::ofstream out(filename);
        if (!out.is_open()) {
            std::cerr << "[Error] Cannot open file " << filename << " for writing." << std::endl;
            return;
        }

        out << ".model " << modelName << "\n";
        
        // Inputs
        out << ".inputs";
        for (int id : graph.getInputs()) out << " " << graph.getNode(id).name;
        out << "\n";

        // Outputs
        out << ".outputs";
        for (int id : graph.getOutputs()) out << " " << graph.getNode(id).name;
        out << "\n";

        // Gates (.names implementation)
        for (const auto& pair : graph.getAllNodes()) {
            const Node& node = pair.second;
            if (node.type == NodeType::GATE) {
                
                // =========================================================
                // 💡 核心修复：安全解析门类型，处理 Z3 废弃的 UNKNOWN 幽灵门
                // =========================================================
                size_t last_underscore = node.gateType.find_last_of('_');
                
                // 情况 1: 如果是 Z3 废弃的门（"UNKNOWN"），或者格式不对没有下划线
                if (last_underscore == std::string::npos || last_underscore + 1 >= node.gateType.length()) {
                    // BLIF 语法黑魔法：没有任何输入、也没有真值表 on-set 的 .names 声明，等效于 Constant 0
                    out << ".names " << node.name << "\n";
                    continue; // 恒零门处理完毕，直接跳过，不去读取 fanins，防止触发越界
                }

                // 情况 2: 正常的逻辑门，安全提取数字索引
                int typeIdx = -1;
                try {
                    typeIdx = std::stoi(node.gateType.substr(last_underscore + 1));
                } catch (...) {
                    // 极端防御：如果 stoi 转换失败（比如截出来不是数字），也降级为 Const 0
                    out << ".names " << node.name << "\n";
                    continue;
                }

                // 安全校验：防止索引越出 library 的范围
                if (typeIdx < 0 || typeIdx >= (int)library.size()) {
                    out << ".names " << node.name << "\n";
                    continue;
                }

                // =========================================================
                // 以下为你原本的正常逻辑，完全保留
                // =========================================================
                const auto& realGate = library[typeIdx];
                int numInputs = realGate.numInputs;

                out << ".names";
                // 打印输入信号名
                for (int i = 0; i < numInputs && i < (int)node.fanins.size(); ++i) {
                    out << " " << graph.getNode(node.fanins[i]).name;
                }
                // 打印输出信号名
                out << " " << node.name << "\n";

                // 打印真值表 (On-set)
                int numEntries = 1 << numInputs;
                for (int i = 0; i < numEntries; ++i) {
                    if ((realGate.truthTable >> i) & 1) {
                        // i 的二进制表示即为输入组合
                        // 注意位序: i=1 (0...01) -> in[0]=1
                        for (int bit = 0; bit < numInputs; ++bit) {
                            out << ((i >> bit) & 1);
                        }
                        out << " 1\n";
                    }
                }
            }
        }
        out << ".end\n";
        out.close();
    }
}