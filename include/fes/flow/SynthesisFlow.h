#ifndef FES_FLOW_SYNTHESISFLOW_H
#define FES_FLOW_SYNTHESISFLOW_H

#include <string>
#include <vector>
#include <tuple>
#include "../core/GateType.h"
#include "../core/Specification.h"
#include "../core/CircuitGraph.h"
#include "fes/utils/InnovusVerifier.h"
#include <unordered_map>

namespace fes {

    // 存储 ABC 分析结果
    struct AbcStats {
        int gates = 0;
        double area = 0.0;
        double power = 0.0;
        bool valid = false;
    };

    struct SynthesisResult {
        std::string hexFunc;
        bool success;
        
        // PONO 内部数据
        int ponoGates;
        double internalCost; // PONO 估算的 Cost
        double runtimeMs;

        // ABC 对 PONO 结果的评估 (Optimized)
        AbcStats optStats;

        // ABC 原生 Baseline 的评估 (用于对比)
        AbcStats baselineStats;
        
        std::string errorMsg;
    };

    class SynthesisFlow {
    private:
        std::vector<GateType> library_;
        std::string abcPath_;
        std::string libPath_;
        InnovusVerifier verifier_;

    public:
        SynthesisFlow(const std::vector<GateType>& lib, 
                      const std::string& abcPath, 
                      const std::string& libPath,
                      const std::string& pythonScriptPath);

        // 运行单个任务
        SynthesisResult run(const std::string& hexFunc, 
                            const std::vector<double>& inputProbs, 
                            const std::string& probTag,           // [新增] 概率标签 (例如 "_20_50_80_20")
                            const std::string& outputDir,
                            int maxGates = 10,                    // [修改] 默认值放宽到 10，适配多输入
                            bool enablePhysicalEval = false);     // [新增] 物理评估开关

        // 批量处理并写入 CSV
        void runBatch(const std::string& inputFile, 
                      const std::string& outputCsv, 
                      const std::string& outputDir,
                      bool enablePhysicalEval = false);           // [新增] 批量物理评估开关
        
        bool buildABCLocalLibraryFromTopCsv(
            const std::string& topCsvPath,
            const std::string& outputDir);


            void debugSingleHexCase(const std::string& hexFunc); 

    private:
        // [新增] 递归生成多输入概率组合的辅助函数
        void generateProbPatterns(int numInputs, 
                                  const std::vector<double>& levels, 
                                  std::vector<double>& current, 
                                  std::vector<std::vector<double>>& results);

        void runAbcToGenerateBaseline(const std::string& hexFunc, const std::string& outBlifPath);
        
        // 1. 准备规格：处理 Hex 长度、补齐概率
        Specification buildSpecification(const std::string& hexFunc, const std::vector<double>& inputProbs);

        // 2. Phase 1: 使用 SAT (Kissat) 寻找最小门数
        // 返回 -1 表示失败
        int runMinimizationPhase(const Specification& spec, int maxGates);

        // 3. Phase 2: 使用 OMT (Z3) 进行结构优化
        // 返回值: <是否成功, 电路图, 内部Cost>
        std::tuple<bool, CircuitGraph, double> runOptimizationPhase(const Specification& spec, int numGates);

        // 4. 后处理：保存文件、打印 Log、验证、评估
        void processResults(SynthesisResult& res, 
                            const CircuitGraph& graph, 
                            const std::string& outputDir);

        // 运行 ABC 评估 BLIF 文件 (对应 Python: analyze_blif_file)
        AbcStats evaluateBlif(const std::string& blifFile);
        
        // 运行 ABC Baseline (对应 Python: analyze_abc_baseline)
        AbcStats runAbcBaseline(const std::string& hexFunc);

        // 通用命令执行与解析
        AbcStats runAbcCommand(const std::string& cmdScript);
        
        bool verify(const std::string& hexFunc, const class CircuitGraph& graph);
    
        std::vector<std::string> loadTopHexFuncsFromCsv(const std::string& topCsvPath);
        std::string probVectorToTag(const std::vector<double>& probs) const;
        std::string buildRawBlifFromHexFunc(const std::string& hexFunc) const;
        bool runSingleABCLocalCase(
            const std::string& hexFunc,
            const std::vector<double>& inputProbs,
            const std::string& outputDir,
            std::ofstream& csvOut);
        int countNamesInBlif(const std::string& blifPath) const;

        bool evaluateCubeMatch(const std::string& cube, int mask, int nInputs) const;
double evaluateNamesNodeTruth(const std::vector<std::string>& sop, int mask, int nInputs) const;
uint16_t computeHexFromBlifFragment(const std::string& blifContent) const;
bool validateBlifImplementsHex(const std::string& blifContent, const std::string& expectedHex) const;
   
};

}
#endif