#include "fes/encoders/PatternEncoder.h"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <vector>
#include <map>

namespace fes {

    PatternEncoder::PatternEncoder(int initialGates) : numGates_(initialGates) {}

    // ==========================================
    // 内部静态辅助函数 (Internal Static Helpers)
    // ==========================================
    
    /**
     * @brief 添加 "Exactly One" (N选1) 约束
     * 语义: 给定一组变量，其中必须有且仅有一个为 True
     */
    static void addExactOne(ISolver* solver, const std::vector<Lit>& lits) {
        if (lits.empty()) return;

        // 1. At least one (至少一个) -> Clause: (L1 v L2 v ... v Ln)
        solver->addClause(lits);

        // 2. At most one (至多一个) -> Pairwise mutex: (-Li v -Lj) for all i != j
        // 朴素编码 O(N^2)。对于 Exact Synthesis 中较小的 N (门扇入通常 <= 4)，这是高效的。
        for (size_t i = 0; i < lits.size(); ++i) {
            for (size_t j = i + 1; j < lits.size(); ++j) {
                solver->addClause({~lits[i], ~lits[j]});
            }
        }
    }

    /**
     * @brief 逻辑电路模拟器 (Logic Simulator)
     * 用于 Functionality 阶段：给定确定的 PI 值，计算内部节点的布尔值
     * 注意：这只处理 Boolean Logic，不处理 Probability
     */
    static std::vector<Lit> simulateCircuit(
        ISolver* solver, 
        const std::vector<Lit>& pi_lits, 
        int numGates,
        const std::vector<GateType>& library,
        std::map<PatternEncoder::SelectionKey, Lit>& sel_vars,
        std::map<PatternEncoder::TypeKey, Lit>& type_vars
    ) {
        std::vector<Lit> node_outputs = pi_lits; // 0..numInputs-1 是 PI

        int numInputs = (int)pi_lits.size();
        
        // 遍历每一个待综合的门 (拓扑序)
        for (int i = 0; i < numGates; ++i) {
            int gateId = numInputs + i;
            int maxInputs = 0;
            for(const auto& g : library) maxInputs = std::max(maxInputs, g.numInputs);

            // 1. 确定当前门每个 Input Slot 的值 (SSV MUX 逻辑)
            // Input_k = OR( Sel_src & Val_src )
            std::vector<Lit> input_slot_values;
            
            for (int k = 0; k < maxInputs; ++k) {
                std::vector<Lit> candidates;
                
                // 遍历所有可能的 Source (0 .. gateId-1)
                for (int src = 0; src < gateId; ++src) {
                    PatternEncoder::SelectionKey key{gateId, k, src};
                    // 如果该连接变量不存在(不可能发生但为了安全)，跳过
                    if (sel_vars.find(key) == sel_vars.end()) continue;
                    
                    Lit sel = sel_vars[key];
                    Lit src_val = node_outputs[src];
                    
                    // tmp = sel AND src_val
                    Lit tmp = solver->newVar();
                    solver->addAnd(tmp, sel, src_val);
                    candidates.push_back(tmp);
                }

                // Slot_Val = OR(candidates)
                Lit slot_val = solver->newVar();
                if (candidates.empty()) {
                    Lit zero = solver->newVar();
                    solver->addClause({~zero}); // Const False
                    slot_val = zero;
                } else if (candidates.size() == 1) {
                    slot_val = candidates[0];
                } else {
                    Lit accum = candidates[0];
                    for (size_t c = 1; c < candidates.size(); ++c) {
                        Lit new_accum = solver->newVar();
                        solver->addOr(new_accum, accum, candidates[c]);
                        accum = new_accum;
                    }
                    slot_val = accum;
                }
                input_slot_values.push_back(slot_val);
            }

            // 2. 根据门类型确定输出值
            // Out = (Type_1 & Res_1) | (Type_2 & Res_2) ...
            std::vector<Lit> type_results;
            
            for (int t = 0; t < (int)library.size(); ++t) {
                PatternEncoder::TypeKey tKey{gateId, t};
                Lit type_sel = type_vars[tKey];
                
                Lit logic_res = solver->newVar();
                
                // 准备该类型门所需的输入
                int needed_inputs = library[t].numInputs;
                std::vector<Lit> logic_inputs;
                for(int inp=0; inp < needed_inputs; ++inp) {
                    logic_inputs.push_back(input_slot_values[inp]);
                }
                
                // 调用 Solver 生成具体的逻辑门约束 (CNF/ITE)
                solver->addGate(logic_res, logic_inputs, library[t]);
                
                // masking: res = type_sel AND logic_res
                Lit masked_res = solver->newVar();
                solver->addAnd(masked_res, type_sel, logic_res);
                type_results.push_back(masked_res);
            }
            
            // 最终 Gate 输出是所有类型结果的 OR
            Lit gate_final_out = solver->newVar();
            if (type_results.size() == 1) {
                gate_final_out = type_results[0];
            } else {
                Lit accum = type_results[0];
                for (size_t tr = 1; tr < type_results.size(); ++tr) {
                    Lit new_accum = solver->newVar();
                    solver->addOr(new_accum, accum, type_results[tr]);
                    accum = new_accum;
                }
                gate_final_out = accum;
            }
            
            node_outputs.push_back(gate_final_out);
        }
        return node_outputs;
    }

    // ==========================================
    // 类成员函数实现
    // ==========================================

    bool PatternEncoder::encode(ISolver* solver, 
                                const Specification& spec, 
                                const std::vector<GateType>& library) 
    {
        // 0. 状态清理
        sel_vars_.clear();
        type_vars_.clear();

        int numInputs = spec.numInputs;

        // 1. 创建基础变量 (Variables Allocation)
        createVariables(solver, library, numInputs);

        // 2. 编码拓扑约束 (Topology)
        // 确保每个输入插槽连接且仅连接一个源
        encodeTopology(solver, numInputs);

        // 3. 编码类型约束 (Type Consistency)
        // 确保每个门是且仅是库中的一种类型
        encodeTypeConsistency(solver, library);

        // 4. 编码逻辑一致性 (Functionality)
        // 确保生成的电路满足 Truth Table
        encodeFunctionality(solver, spec, library);

        // 5. 编码功耗概率 (PONO Probability Propagation)
        // 构建 SMT 实数网络，传播概率并计算 Cost
        encodeProbability(solver, spec, library);
        
        return true;
    }

    void PatternEncoder::createVariables(ISolver* solver, const std::vector<GateType>& lib, int numInputs) {
        int maxInputsPerGate = 0;
        for(const auto& g : lib) maxInputsPerGate = std::max(maxInputsPerGate, g.numInputs);

        for (int i = 0; i < numGates_; ++i) {
            int gateId = numInputs + i;
            
            // A. 分配 SSV 选择变量: Sel[gate][slot][source]
            for (int k = 0; k < maxInputsPerGate; ++k) {
                for (int src = 0; src < gateId; ++src) {
                    Lit l = solver->newVar();
                    SelectionKey key{gateId, k, src};
                    sel_vars_[key] = l;
                }
            }

            // B. 分配类型选择变量: Type[gate][libIdx]
            for (int t = 0; t < (int)lib.size(); ++t) {
                Lit l = solver->newVar();
                TypeKey key{gateId, t};
                type_vars_[key] = l;
            }
        }
    }

    void PatternEncoder::encodeTopology(ISolver* solver, int numInputs) {
        for (int i = 0; i < numGates_; ++i) {
            int gateId = numInputs + i;
            
            // 我们通过遍历 map 来通过 inputIdx 分组
            // 更优化的做法是在 createVariables 时记录 maxInputs，但遍历 map 更加通用
            std::map<int, std::vector<Lit>> slot_groups;
            
            for (const auto& kv : sel_vars_) {
                if (kv.first.gateId == gateId) {
                    slot_groups[kv.first.inputIdx].push_back(kv.second);
                }
            }

            // 对每个插槽施加 ExactlyOne 约束
            for (const auto& kv : slot_groups) {
                addExactOne(solver, kv.second);
            }
        }
    }

    void PatternEncoder::encodeTypeConsistency(ISolver* solver, const std::vector<GateType>& lib) {
        // 查找最小的 GateId (通常是 numInputs)
        int minGateId = 999999;
        if (!type_vars_.empty()) minGateId = type_vars_.begin()->first.gateId;
        else return;

        for (int i = 0; i < numGates_; ++i) {
            int gateId = minGateId + i;
            
            std::vector<Lit> t_lits;
            for (const auto& kv : type_vars_) {
                if (kv.first.gateId == gateId) {
                    t_lits.push_back(kv.second);
                }
            }
            // 每个门必须选一种类型
            addExactOne(solver, t_lits);
        }
    }

    void PatternEncoder::encodeFunctionality(ISolver* solver, const Specification& spec, const std::vector<GateType>& lib) {
        int numInputs = spec.numInputs;
        // 如果真值表大小支持，这里应该处理所有 minterms
        // 对于 numInputs <= 4，minterms = 16，非常快
        int numMinterms = 1 << numInputs;

        for (int m = 0; m < numMinterms; ++m) {
            // 2.1 准备 PI (Primary Inputs)
            // 将当前 minterm 的二进制位转换为常量 Literal
            std::vector<Lit> pi_lits;
            for (int bit = 0; bit < numInputs; ++bit) {
                bool val = (m >> bit) & 1;
                Lit l = solver->newVar();
                if (val) solver->addClause({l});
                else     solver->addClause({~l});
                pi_lits.push_back(l);
            }

            // 2.2 模拟电路 (Logic Simulation)
            std::vector<Lit> nodes = simulateCircuit(solver, pi_lits, numGates_, lib, sel_vars_, type_vars_);
            
            // 2.3 约束输出 (Output Constraint)
            // 假设单输出，取最后一个门
            Lit output_gate = nodes.back();
            
            // 从 Spec 获取期望值
            // 假设 truthTable[0] 存储的是 output 0
            bool expected = (spec.truthTable[0] >> m) & 1;
            
            if (expected) solver->addClause({output_gate});
            else          solver->addClause({~output_gate});
        }
    }

    void PatternEncoder::encodeProbability(ISolver* solver, const Specification& spec, const std::vector<GateType>& lib) {
        // 1. 初始化输入概率 (Input Probabilities)
        if (spec.inputProbabilities.empty()) {
            std::vector<double> defaultProbs(spec.numInputs, 0.5);
            solver->initProbabilities(defaultProbs);
        } else {
            // 使用 Spec 中指定的概率 (如 A=0.1, B=0.9)
            solver->initProbabilities(spec.inputProbabilities);
        }

        int numInputs = spec.numInputs;
        int maxInputsPerGate = 0;
        for(const auto& g : lib) maxInputsPerGate = std::max(maxInputsPerGate, g.numInputs);

        // 2. 遍历每个门，构建概率传播网络 (Probability Network)
        // 这一步将逻辑结构 (sel_vars, type_vars) 映射到实数域 (Z3 Real Expr)
        for (int i = 0; i < numGates_; ++i) {
            int gateId = numInputs + i;

            // A. 收集该门的 Type Lits
            std::vector<Lit> typeLits;
            for (int t = 0; t < (int)lib.size(); ++t) {
                TypeKey key{gateId, t};
                typeLits.push_back(type_vars_[key]);
            }

            // B. 收集该门的 Selection Lits (按 Input Slot 分组)
            std::map<int, std::vector<Lit>> selLitsMap;
            for (int k = 0; k < maxInputsPerGate; ++k) {
                std::vector<Lit> srcs;
                // 按 sourceId 顺序收集，保证与 addGateProbability 内部的 prob 索引对齐
                for (int src = 0; src < gateId; ++src) {
                    SelectionKey key{gateId, k, src};
                    // 必须确保 sel_vars_ 中存在这个 key
                    if (sel_vars_.count(key)) {
                        srcs.push_back(sel_vars_[key]);
                    }
                }
                selLitsMap[k] = srcs;
            }

            // C. 调用 Solver 构建概率约束
            // Solver 内部会根据 Type 和 Selection 构建:
            // p_out = Sum( Type_i * Prob_Func_i( p_in_0, p_in_1... ) )
            solver->addGateProbability(gateId, typeLits, selLitsMap, lib);
        }
    }

    CircuitGraph PatternEncoder::decode(ISolver* solver) {
        // 查找最小 gateId
        int minGateId = 999999;
        if (!sel_vars_.empty()) minGateId = sel_vars_.begin()->first.gateId;
        
        int numInputs = minGateId;
        CircuitGraph graph(numInputs, 1); 

        // 添加 PI
        for(int i=0; i<numInputs; ++i) graph.addInput(i);

        // 重建 Gates
        for (int i = 0; i < numGates_; ++i) {
            int gateId = numInputs + i;
            
            // 1. 解码类型
            std::string typeName = "UNKNOWN";
            for (const auto& kv : type_vars_) {
                if (kv.first.gateId == gateId) {
                    if (solver->getModelValue(kv.second)) {
                        // 如果有库的引用，可以查名字，这里用 Index 代理
                        typeName = "GateType_" + std::to_string(kv.first.typeIdx); 
                        break;
                    }
                }
            }

            // 2. 解码连接
            std::vector<int> fanins;
            // 假设最大 6 输入插槽
            for (int k = 0; k < 6; ++k) {
                for (int src = 0; src < gateId; ++src) {
                    SelectionKey key{gateId, k, src};
                    if (sel_vars_.count(key)) {
                        if (solver->getModelValue(sel_vars_[key])) {
                            fanins.push_back(src);
                            break; // 该插槽已找到源
                        }
                    }
                }
            }
            graph.addGate(gateId, typeName, fanins);
        }

        graph.setOutputs({numInputs + numGates_ - 1});
        return graph;
    }

} // namespace fes