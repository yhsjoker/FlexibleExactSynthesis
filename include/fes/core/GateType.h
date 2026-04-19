#ifndef FES_CORE_GATETYPE_H
#define FES_CORE_GATETYPE_H

#include <string>
#include <vector>
#include <cstdint>

namespace fes {

    struct GateType {
        std::string name;       // 门的名字 (如 "AND2", "MUX")
        int numInputs;          // 输入端口数
        uint64_t truthTable;    // 逻辑真值表 (LSB对应输入全是0的情况)
        
        // === 物理参数 (默认为0，PONO算法会用到) ===
        double staticPower;     // 静态功耗 (单位如 uW)
        double area;            // 面积
        double delay;           // 延迟

        GateType(std::string n, int in, uint64_t tt, double sp = 0.0, double a = 0.0, double d = 0.0)
            : name(n), numInputs(in), truthTable(tt), staticPower(sp), area(a), delay(d) {}
    };
} // namespace fes

#endif // FES_CORE_GATETYPE_H