#include <iostream>
#include <vector>

// 1. 引入 Z3 头文件
#include <z3++.h>

// 2. 引入 Kissat 头文件 (如果有的话)
#ifdef HAS_KISSAT
extern "C" {
    #include "kissat.h"
}
#endif

void test_z3() {
    std::cout << "[INFO] Testing Z3 linkage..." << std::endl;
    try {
        z3::context c;
        z3::solver s(c);
        
        // 创建一个简单的逻辑: a AND b
        z3::expr a = c.bool_const("a");
        z3::expr b = c.bool_const("b");
        s.add(a && b);
        
        // 求解
        if (s.check() == z3::sat) {
            z3::model m = s.get_model();
            std::cout << "  [Z3] SAT Found! Model: \n" << m << std::endl;
            std::cout << "  [Z3] Version: " << Z3_get_full_version() << std::endl;
            std::cout << "  [Z3] Test PASSED." << std::endl;
        } else {
            std::cerr << "  [Z3] Unexpected UNSAT." << std::endl;
        }
    } catch (z3::exception& e) {
        std::cerr << "  [Z3] Exception: " << e.msg() << std::endl;
    }
    std::cout << "------------------------------------------------" << std::endl;
}

void test_kissat() {
    std::cout << "[INFO] Testing Kissat linkage..." << std::endl;
#ifdef HAS_KISSAT
    // 创建 Kissat 求解器实例
    kissat* solver = kissat_init();
    if (solver) {
        std::cout << "  [Kissat] Solver initialized successfully." << std::endl;
        
        // 添加子句: (1 OR 2)
        kissat_add(solver, 1);
        kissat_add(solver, 2);
        kissat_add(solver, 0); // 0 结尾

        // 添加子句: (-1) -> 强制 1 为 False
        kissat_add(solver, -1);
        kissat_add(solver, 0);

        // 求解
        int res = kissat_solve(solver);
        if (res == 10) { // 10 = SAT, 20 = UNSAT
            std::cout << "  [Kissat] SAT Found! (Expected)" << std::endl;
            // 验证: 1 应该是 -1 (False), 2 应该是 1 (True)
            int val1 = kissat_value(solver, 1);
            int val2 = kissat_value(solver, 2);
            std::cout << "  [Kissat] Var 1 value: " << val1 << std::endl;
            std::cout << "  [Kissat] Var 2 value: " << val2 << std::endl;
        } else {
            std::cout << "  [Kissat] Result code: " << res << std::endl;
        }

        kissat_release(solver);
        std::cout << "  [Kissat] Test PASSED." << std::endl;
    } else {
        std::cerr << "  [Kissat] Failed to init solver." << std::endl;
    }
#else
    std::cout << "  [Kissat] Skipped (HAS_KISSAT not defined)." << std::endl;
#endif
    std::cout << "------------------------------------------------" << std::endl;
}

int main() {
    std::cout << "=== FlexibleExactSynthesis Environment Test ===" << std::endl;
    
    test_z3();
    test_kissat();
    
    return 0;
}