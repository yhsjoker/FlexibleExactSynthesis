#ifndef FES_CORE_TYPES_H
#define FES_CORE_TYPES_H

#include <cstdint>
#include <iostream>

namespace fes {

    // ----------------------------------------------------------------------
    // LUT configuration. The rewriter, library loader, benchmark extractor,
    // and ABC pre-map flows all derive from this single constant. Switching
    // the project to 6-input LUTs is a one-line change (set kLutMaxInputs=6).
    // LutTruthTable is 64 bits wide so the truth-table bitmap stays
    // overflow-safe for K up to 6 (2^6 = 64 rows).
    // ----------------------------------------------------------------------
    constexpr int kLutMaxInputs          = 4;
    constexpr int kLutTruthTableRows     = 1 << kLutMaxInputs;          // 2^K
    constexpr int kLutTruthTableHexDigits =
        (kLutTruthTableRows + 3) / 4;                                   // nibbles

    using LutTruthTable = std::uint64_t;

    // Truth-table "all ones" mask, sized to exactly kLutTruthTableRows bits.
    // Written as a function so the shift is well-defined even if a future
    // tuning raises K to the full width of LutTruthTable.
    constexpr LutTruthTable kLutTruthTableAllOnes =
        (kLutTruthTableRows >= 64)
            ? ~LutTruthTable{0}
            : ((LutTruthTable{1} << kLutTruthTableRows) - 1);

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