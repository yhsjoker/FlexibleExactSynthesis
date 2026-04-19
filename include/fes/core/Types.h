#ifndef FES_CORE_TYPES_H
#define FES_CORE_TYPES_H

#include <iostream>

namespace fes {

    // 变量索引类型
    using VarIndex = int;

    // 文字 (Literal): 变量 + 正负号
    struct Lit {
        VarIndex var;
        bool isComplemented; // true 表示取反 (NOT)

        Lit() : var(-1), isComplemented(false) {}
        Lit(VarIndex v, bool c) : var(v), isComplemented(c) {}

        // 取反操作符 ~l
        Lit operator~() const {
            return {var, !isComplemented};
        }

        bool operator==(const Lit& other) const {
            return var == other.var && isComplemented == other.isComplemented;
        }
        
        bool operator!=(const Lit& other) const {
            return !(*this == other);
        }
        
        // 用于 std::map / std::set 的比较
        bool operator<(const Lit& other) const {
            if (var != other.var) return var < other.var;
            return isComplemented < other.isComplemented;
        }
    };

    // 求解状态枚举
    enum class SolveStatus {
        SAT,        // 可行
        UNSAT,      // 无解
        OPTIMAL,    // 最优解 (仅 Z3 optimize 模式)
        UNKNOWN     // 未知/超时
    };

    // 打印辅助
    inline std::ostream& operator<<(std::ostream& os, const Lit& l) {
        return os << (l.isComplemented ? "-" : "") << "v" << l.var;
    }

} // namespace fes

#endif // FES_CORE_TYPES_H