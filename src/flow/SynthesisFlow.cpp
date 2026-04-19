#include "fes/flow/SynthesisFlow.h"
#include "fes/interfaces/ISolver.h"
#include "fes/solvers/Z3Solver.h"
#include "fes/solvers/KissatSolver.h"
#include "fes/encoders/PatternEncoder.h"
#include "fes/utils/BlifWriter.h"
#include "fes/utils/EquivalenceChecker.h"
#include "fes/core/Specification.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#include <memory>
#include <cstdio>
#include <array>
#include <bitset>
#include <map>
#include <cmath>
#include <regex>
#include <filesystem>
#include <iomanip> // 新增：用于字符串格式化 setfill, setw
#include <set>
namespace fs = std::filesystem;

namespace fes {

    SynthesisFlow::SynthesisFlow(const std::vector<GateType>& lib, 
                             const std::string& abcPath, 
                             const std::string& libPath,
                             const std::string& pythonScriptPath)
    : library_(lib), 
      abcPath_(abcPath), 
      libPath_(libPath), 
      verifier_(pythonScriptPath) 
    {}

    static uint64_t hexToInt(const std::string& h) {
        uint64_t r; std::stringstream ss; ss << std::hex << h; ss >> r; return r;
    }

    static AbcStats convertPpaToAbcStats(const PPAResult& ppa) {
        AbcStats stats;
        if (ppa.valid) {
            stats.power = ppa.power_total; 
            stats.area = ppa.area;         
            stats.gates = -1;              
            stats.valid = true;
        } else {
            stats.power = -1.0;
            stats.valid = false;
        }
        return stats;
    }

    // =========================================================
    // [新增] 递归生成多输入概率组合 (Stratification Pattern Generator)
    // =========================================================
    void SynthesisFlow::generateProbPatterns(int numInputs, 
                                             const std::vector<double>& levels, 
                                             std::vector<double>& current, 
                                             std::vector<std::vector<double>>& results) {
        if (current.size() == (size_t)numInputs) {
            results.push_back(current);
            return;
        }
        for (double level : levels) {
            current.push_back(level);
            generateProbPatterns(numInputs, levels, current, results);
            current.pop_back(); // 回溯
        }
    }

    void SynthesisFlow::runAbcToGenerateBaseline(const std::string& hexFunc, const std::string& outBlifPath) {
        std::stringstream ss;
        ss << "read_genlib " << libPath_ << "; ";
        ss << "read_truth " << hexFunc << "; ";
        ss << "strash; balance; rewrite; rewrite -z; balance; rewrite -z; balance; ";
        ss << "map -a; "; 
        ss << "write_blif " << outBlifPath;

        std::string fullCmd = abcPath_ + " -c \"" + ss.str() + "\" > /dev/null 2>&1";
        int ret = system(fullCmd.c_str());
        if (ret != 0) {
            std::cerr << "[Error] ABC failed to generate baseline for: " << hexFunc << std::endl;
        }
    }

    // =========================================================
    // 核心综合与评估流程 (分离建库与评测)
    // =========================================================
    SynthesisResult SynthesisFlow::run(const std::string& hexFunc, 
                                       const std::vector<double>& inputProbs, 
                                       const std::string& probTag,
                                       const std::string& outputDir, 
                                       int maxGates,
                                       bool enablePhysicalEval) {
        SynthesisResult res;
        res.hexFunc = hexFunc; 
        res.success = false;
        
        // 唯一的电路变体标识符，例如：0888_20_50_80_20
        std::string variantName = hexFunc + probTag;

        // --- 准备工作目录 ---
        fs::path funcDir = fs::path(outputDir) / "detailed_infos" / hexFunc;
        if (!fs::exists(funcDir)) fs::create_directories(funcDir);

        // -----------------------------------------------------
        // Step 1: PONO 逻辑生成 (SAT Minimization)
        // -----------------------------------------------------
        auto start = std::chrono::high_resolution_clock::now();
        Specification spec = buildSpecification(hexFunc, inputProbs);
        int minGates = runMinimizationPhase(spec, maxGates);

        if (minGates == -1) {
            res.errorMsg = "UNSAT within max gates";
            return res;
        }

        // -----------------------------------------------------
        // Step 2: PONO 结构优化 (Z3/OMT)
        // -----------------------------------------------------
        auto [optSuccess, graph, cost] = runOptimizationPhase(spec, minGates);
        if (!optSuccess) {
            res.errorMsg = "Z3 Optimization Failed";
            return res;
        }

        if (verifyEnabled_) {
            EquivalenceChecker cec(library_);
            auto cecRes = cec.verifyAgainstHex(hexFunc, graph);
            if (!cecRes.equivalent) {
                std::cerr << "[CEC] FAIL " << hexFunc << probTag
                          << " -> " << cecRes.message << std::endl;
                res.errorMsg = "CEC: " + cecRes.message;
                return res;
            }
            std::cout << "[CEC] OK " << hexFunc << probTag << std::endl;
        }

        res.ponoGates = minGates;
        res.internalCost = cost;
        
        // --- 库文件导出 ---
        fs::path ponoBlifPath = funcDir / ("pono_" + variantName + ".blif");
        BlifWriter::write(ponoBlifPath.string(), "pono_design", graph, library_);

        // 临时使用 variantName 欺骗 processResults 写入正确的名字
        std::string originalHex = res.hexFunc;
        res.hexFunc = variantName; 
        processResults(res, graph, outputDir);
        res.hexFunc = originalHex; // 恢复

        // -----------------------------------------------------
        // Step 3: [可选] 物理综合评估 (Innovus) 
        // 只有当你想单独跑一组数据看真实效果时，才激活这部分
        // -----------------------------------------------------
        if (enablePhysicalEval) {
            // 科学计算真实翻转率 Activity: 2 * P * (1 - P)
            std::vector<double> acts(inputProbs.size());
            for (size_t i = 0; i < inputProbs.size(); ++i) {
                acts[i] = 2.0 * inputProbs[i] * (1.0 - inputProbs[i]);
            }

            std::string baselineBlif = (funcDir / ("baseline_abc_" + variantName + ".blif")).string();
            runAbcToGenerateBaseline(hexFunc, baselineBlif);

            std::cout << "  - Evaluating Physical Baseline (Innovus)..." << std::endl;
            res.baselineStats = convertPpaToAbcStats(verifier_.getPPAResult(baselineBlif, inputProbs, acts));
            
            std::cout << "  - Evaluating PONO Optimized Physical (Innovus)..." << std::endl;
            res.optStats = convertPpaToAbcStats(verifier_.getPPAResult(ponoBlifPath.string(), inputProbs, acts));
            
            res.success = res.baselineStats.valid && res.optStats.valid;
            if (!res.success) res.errorMsg = "Innovus Evaluation Failed";
        } else {
            // 建库模式下，Z3跑通即视为完全成功
            res.success = true;
        }

        auto end = std::chrono::high_resolution_clock::now();
        res.runtimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        return res;
    }

    // =========================================================
    // 辅助函数实现 (Helper Implementations)
    // =========================================================
    Specification SynthesisFlow::buildSpecification(const std::string& hexFunc, const std::vector<double>& inputProbs) {
        int numInputs = 0;
        size_t numBits = hexFunc.length() * 4;
        while ((1u << numInputs) < numBits) numInputs++;
        
        if ((1u << numInputs) != numBits && numBits != 0) {
            std::cerr << "[Warning] Hex string length implies fractional inputs?" << std::endl;
        }
        
        std::vector<double> currentProbs = inputProbs;
        if (currentProbs.size() < (size_t)numInputs) currentProbs.resize(numInputs, 0.5);
        
        Specification spec(numInputs, 1);
        spec.setTruthTable(hexToInt(hexFunc)); 
        spec.setInputProbabilities(currentProbs);
        return spec;
    }

    int SynthesisFlow::runMinimizationPhase(const Specification& spec, int maxGates) {
        for (int n = 1; n <= maxGates; ++n) {
            auto sat = std::make_unique<KissatSolver>();
            sat->setTimeLimit(10000); 
            PatternEncoder enc(n);
            if (!enc.encode(sat.get(), spec, library_)) continue;
            if (sat->solve() == SolveStatus::SAT) return n;
        }
        return -1;
    }

    std::tuple<bool, CircuitGraph, double> SynthesisFlow::runOptimizationPhase(const Specification& spec, int numGates) {
        auto z3 = std::make_unique<Z3Solver>();
        z3->setTimeLimit(60000); 
        PatternEncoder enc(numGates);
        enc.encode(z3.get(), spec, library_); 
        SolveStatus status = z3->solve();

        if (status == SolveStatus::OPTIMAL || status == SolveStatus::SAT) {
            return {true, enc.decode(z3.get()), z3->getOptimizationResult()};
        }
        return {false, CircuitGraph(), 0.0};
    }

    void SynthesisFlow::processResults(SynthesisResult& res, const CircuitGraph& graph, const std::string& outputDir) {
        std::string variantName = res.hexFunc;
        std::string baseHex = variantName.substr(0, variantName.find('_'));

        fs::path funcDir = fs::path(outputDir) / "detailed_infos" / baseHex;
        if (!fs::exists(funcDir)) fs::create_directories(funcDir);

        fs::path txtPath = funcDir / ("pono_" + variantName + "_struct.txt");
        std::ofstream txtFile(txtPath);
        
        if (txtFile.is_open()) {
            txtFile << "Variant: " << res.hexFunc << "\n";
            txtFile << "Gates: " << res.ponoGates << "\n";
            txtFile << "Cost(Internal Rp): " << res.internalCost << "\n";
            if (res.optStats.valid) {
                txtFile << "Physical Power (Innovus): " << res.optStats.power << " mW\n";
                txtFile << "Physical Area (Innovus): " << res.optStats.area << " um^2\n";
            }
            txtFile << "----------------------------------------\n";
            
            for (const auto& nodePair : graph.getAllNodes()) {
                const Node& node = nodePair.second;
                if (node.type == NodeType::GATE) {
                    // ==========================================
                    // 💡 修复点 1: 动态解析门类型，安全打印 struct.txt
                    // ==========================================
                    size_t last_underscore = node.gateType.find_last_of('_');
                    if (last_underscore == std::string::npos || last_underscore + 1 >= node.gateType.length()) {
                        txtFile << node.name << " = CONST0()\n";
                        continue;
                    }
                    
                    int typeIdx = -1;
                    try { typeIdx = std::stoi(node.gateType.substr(last_underscore + 1)); }
                    catch (...) { txtFile << node.name << " = CONST0()\n"; continue; }
                    
                    if (typeIdx < 0 || typeIdx >= (int)library_.size()) continue;

                    const auto& realGate = library_[typeIdx];
                    txtFile << node.name << " = " << realGate.name << "(";
                    
                    // 只打印有效输入数量
                    int limit = std::min((int)node.fanins.size(), realGate.numInputs);
                    for (int i = 0; i < limit; ++i) {
                        int fid = node.fanins[i];
                        if (graph.getAllNodes().count(fid)) txtFile << graph.getNode(fid).name;
                        else txtFile << "UNK";
                        if (i < limit - 1) txtFile << ", ";
                    }
                    txtFile << ")\n";
                }
            }
            txtFile.close();
        }

        std::cout << "\n  [DEBUG NETLIST] Solved with " << res.ponoGates << " gates. (Saved to " << txtPath << ")\n";

        if (!verify(res.hexFunc.substr(0, res.hexFunc.find('_')), graph)) {
            res.success = false;
            res.errorMsg = "Verification Failed";
            std::cerr << "[Error] Internal verification failed for " << res.hexFunc << std::endl;
        }
    }

    // ==========================================
    // 各种ABC调用保留 (供评测和其他流程使用)
    // ==========================================
    AbcStats SynthesisFlow::runAbcCommand(const std::string& cmdScript) {
        AbcStats stats;
        std::stringstream cmd;
        cmd << abcPath_ << " -c \"" << cmdScript << "\" 2>&1";

        std::string output = "";
        std::array<char, 128> buffer;
        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.str().c_str(), "r"), pclose);
        if (!pipe) return stats;
        while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) output += buffer.data();

        try {
            std::regex re_gates(R"((?:nd|nodes)\s*=\s*(\d+))", std::regex::icase);
            std::regex re_area(R"(area\s*=\s*([\d\.]+))", std::regex::icase);
            std::regex re_power(R"(power\s*=\s*([\d\.]+))", std::regex::icase);
            
            auto gates_begin = std::sregex_iterator(output.begin(), output.end(), re_gates);
            for (std::sregex_iterator i = gates_begin; i != std::sregex_iterator(); ++i) 
                if ((*i).size() > 1) stats.gates = std::stoi((*i).str(1));

            auto area_begin = std::sregex_iterator(output.begin(), output.end(), re_area);
            for (std::sregex_iterator i = area_begin; i != std::sregex_iterator(); ++i) 
                if ((*i).size() > 1) stats.area = std::stod((*i).str(1));

            auto power_begin = std::sregex_iterator(output.begin(), output.end(), re_power);
            for (std::sregex_iterator i = power_begin; i != std::sregex_iterator(); ++i) 
                if ((*i).size() > 1) stats.power = std::stod((*i).str(1));

        } catch (...) { /* 静默处理 */ }
        return stats;
    }

    AbcStats SynthesisFlow::evaluateBlif(const std::string& blifFile) {
        std::stringstream ss;
        ss << "read_genlib " << libPath_ << "; "; 
        ss << "read_blif " << blifFile << "; ";
        ss << "ps -p";
        return runAbcCommand(ss.str());
    }

    AbcStats SynthesisFlow::runAbcBaseline(const std::string& hexFunc) {
        std::stringstream ss;
        ss << "read_genlib " << libPath_ << "; ";
        ss << "read_truth " << hexFunc << "; ";
        ss << "strash; balance; rewrite; rewrite -z; balance; rewrite -z; balance; map -a; ps -p";
        return runAbcCommand(ss.str());
    }

    bool SynthesisFlow::verify(const std::string& hexFunc, const CircuitGraph& graph) {
        int numInputs = (int)graph.getInputs().size();
        if (numInputs == 0) numInputs = 4;

        uint64_t targetTruthTable = hexToInt(hexFunc);
        int numPatterns = 1 << numInputs;
        
        for (int i = 0; i < numPatterns; ++i) {
            std::map<int, bool> nodeValues;
            for (int bit = 0; bit < numInputs; ++bit) nodeValues[bit] = (i >> bit) & 1;

            for (const auto& nodePair : graph.getAllNodes()) {
                const Node& node = nodePair.second;
                if (node.type == NodeType::GATE) {
                    
                    // ==========================================
                    // 💡 修复点 2: 验证器兼容幽灵门 (将其模拟为 false/0)
                    // ==========================================
                    size_t last_underscore = node.gateType.find_last_of('_');
                    if (last_underscore == std::string::npos || last_underscore + 1 >= node.gateType.length()) {
                        nodeValues[node.id] = false; // 幽灵门等效为逻辑 0
                        continue;
                    }

                    int typeIdx = -1;
                    try { typeIdx = std::stoi(node.gateType.substr(last_underscore + 1)); }
                    catch (...) { nodeValues[node.id] = false; continue; }
                    
                    if (typeIdx < 0 || typeIdx >= (int)library_.size()) {
                        nodeValues[node.id] = false; continue;
                    }

                    const GateType& gateDef = library_[typeIdx];
                    int ttIndex = 0;
                    int limit = std::min(gateDef.numInputs, (int)node.fanins.size()); 
                    
                    for (int k = 0; k < limit; ++k) {
                        int faninId = node.fanins[k];
                        if (nodeValues[faninId]) ttIndex |= (1 << k); 
                    }
                    nodeValues[node.id] = (gateDef.truthTable >> ttIndex) & 1;
                }
            }
            if (graph.getOutputs().empty()) return false;
            if (nodeValues[graph.getOutputs()[0]] != ((targetTruthTable >> i) & 1)) return false;
        }
        return true;
    }

    // ==========================================
    // 智能批量建库 (动态推导 N, 遍历生成)
    // ==========================================
    void SynthesisFlow::runBatch(const std::string& inputFile, 
                                 const std::string& outputCsv, 
                                 const std::string& outputDir,
                                 bool enablePhysicalEval) {
        std::ifstream inFile(inputFile);
        if (!inFile.is_open()) return;
        
        // =========================================================
        // [新增] 1. 断点续传：读取已存在的 CSV，记录跑过的变体
        // =========================================================
        std::set<std::string> completedVariants;
        bool csvExists = fs::exists(outputCsv);
        
        if (csvExists) {
            std::ifstream existingCsv(outputCsv);
            std::string line;
            if (std::getline(existingCsv, line)) { /* 跳过已有的 Header */ }
            while (std::getline(existingCsv, line)) {
                if (line.empty()) continue;
                std::stringstream ss(line);
                std::string hex, tag;
                std::getline(ss, hex, ','); // 第一列: HexFunc
                std::getline(ss, tag, ','); // 第二列: ProbPattern
                
                // 将 hex 和 tag 拼接作为唯一标识符加入集合
                completedVariants.insert(hex + tag);
            }
            std::cout << "[Batch] Resume mode: Found " << completedVariants.size() 
                      << " already completed variants in " << outputCsv << std::endl;
        }

        // =========================================================
        // [修改] 2. 以追加模式 (std::ios::app) 打开输出文件
        // =========================================================
        std::ofstream outFile(outputCsv, std::ios::app);
        if (!outFile.is_open()) return;

        // 如果是新创建的文件，才需要写入表头
        if (!csvExists) {
            outFile << "HexFunc,ProbPattern,Success,Gates,InternalCost,RuntimeMs";
            if (enablePhysicalEval) {
                outFile << ",Base_Power(mW),Opt_Power(mW),Power_Impr(%)";
            }
            outFile << ",Error\n";
        }

        std::string line;
        if (std::getline(inFile, line)) { /* 跳过输入文件的 Header */ }

        // --- 定义概率离散挡位 ---
        std::vector<double> levels = {0.2, 0.5, 0.8}; 

        while (std::getline(inFile, line)) {
            if (line.empty() || line[0] == '#') continue;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            while (!line.empty() && std::isspace(line.back())) line.pop_back();
            if (line.empty()) continue;

            std::stringstream ss(line);
            std::string hexFunc;
            
            if (std::getline(ss, hexFunc, ',')) {
                hexFunc.erase(0, hexFunc.find_first_not_of(" \t\r\n"));
                hexFunc.erase(hexFunc.find_last_not_of(" \t\r\n") + 1);
            } else continue;

            // --- 自动推断当前真值表的输入端数量 N ---
            int numBits = hexFunc.length() * 4;
            int numInputs = 0;
            while ((1u << numInputs) < (unsigned)numBits) numInputs++;

            // --- 递归生成对应 N 输入的所有概率组合 ---
            std::vector<std::vector<double>> probPatterns;
            std::vector<double> currentPattern;
            for(int i = 1; i <= 9; i++){
                double t = 0.1 * i;
                probPatterns.push_back({t, t, t, t});
            }
            // generateProbPatterns(numInputs, levels, currentPattern, probPatterns);

            std::cout << "\n[Batch] Loaded " << hexFunc << " (" << numInputs 
                      << " inputs). Checking " << probPatterns.size() << " variants..." << std::endl;

            for (const auto& probs : probPatterns) {
                // 格式化标识符 tag (例: _20_50_80_20)
                std::stringstream probStr;
                for (double p : probs) {
                    probStr << "_" << std::setw(2) << std::setfill('0') << static_cast<int>(p * 100);
                }
                std::string probTag = probStr.str();

                // =========================================================
                // [新增] 3. 检查是否已经生成过，如果是则跳过
                // =========================================================
                std::string variantId = hexFunc + probTag;
                if (completedVariants.count(variantId)) {
                    std::cout << "  -> [SKIP] Variant " << probTag << " already processed." << std::endl;
                    continue; 
                }

                std::cout << "  -> Processing variant " << probTag << "... " << std::flush;
                
                SynthesisResult res = run(hexFunc, probs, probTag, outputDir, 10, enablePhysicalEval);
                
                std::cout << (res.success ? "OK" : "FAIL") << std::endl;

                // 写入数据
                outFile << res.hexFunc << "," << probTag << ","
                        << (res.success ? "true" : "false") << ","
                        << res.ponoGates << ","
                        << res.internalCost << ","
                        << res.runtimeMs;

                if (enablePhysicalEval) {
                    double impr = (res.baselineStats.power > 1e-9) ? 
                        (res.baselineStats.power - res.optStats.power) / res.baselineStats.power * 100.0 : 0.0;
                    outFile << "," << res.baselineStats.power << "," << res.optStats.power << "," << impr;
                }

                outFile << "," << res.errorMsg << "\n";
                outFile.flush(); // 实时落盘
            }
        }
        std::cout << "[Batch] Library generation complete. Data saved to " << outputCsv << std::endl;
    }

    std::string SynthesisFlow::buildRawBlifFromHexFunc(const std::string& hexFunc) const {
    unsigned value = 0;
    std::stringstream ss;
    ss << std::hex << hexFunc;
    ss >> value;

    std::ostringstream out;
    out << ".model raw_func_" << hexFunc << "\n";
    out << ".inputs a b c d\n";
    out << ".outputs y\n";

    // 常 0：不要带输入
    if (value == 0x0000) {
        out << ".names y\n";
        out << ".end\n";
        return out.str();
    }

    // 常 1：不要带输入
    if (value == 0xFFFF) {
        out << ".names y\n";
        out << "1\n";
        out << ".end\n";
        return out.str();
    }

    // 普通 4 输入函数
    out << ".names a b c d y\n";
    for (int m = 0; m < 16; ++m) {
        if ((value >> m) & 1U) {
            out << (((m >> 0) & 1) ? '1' : '0')
                << (((m >> 1) & 1) ? '1' : '0')
                << (((m >> 2) & 1) ? '1' : '0')
                << (((m >> 3) & 1) ? '1' : '0')
                << " 1\n";
        }
    }

    out << ".end\n";
    return out.str();
}

    int SynthesisFlow::countNamesInBlif(const std::string& blifPath) const {
        std::ifstream ifs(blifPath);
        if (!ifs.is_open()) return -1;

        std::string line;
        int cnt = 0;
        while (std::getline(ifs, line)) {
            if (line.rfind(".names", 0) == 0) cnt++;
        }
        return cnt;
    }

    bool SynthesisFlow::runSingleABCLocalCase(
    const std::string& hexFunc,
    const std::vector<double>& inputProbs,
    const std::string& outputDir,
    std::ofstream& csvOut)
{
    namespace fs = std::filesystem;

    std::string probTag = probVectorToTag(inputProbs);
    std::string variantName = hexFunc + probTag;

    fs::path funcDir = fs::path(outputDir) / "detailed_infos" / hexFunc;
    if (!fs::exists(funcDir)) fs::create_directories(funcDir);

    fs::path rawBlifPath = funcDir / ("raw_" + variantName + ".blif");
    fs::path abcBlifPath = funcDir / ("abc_" + variantName + ".blif");
    fs::path logPath     = funcDir / ("abc_" + variantName + ".log");

    {
        std::ofstream ofs(rawBlifPath.string());
        if (!ofs.is_open()) {
            csvOut << hexFunc << "," << probTag << ",false,-1,-1,0,Raw BLIF Write Failed\n";
            return false;
        }
        ofs << buildRawBlifFromHexFunc(hexFunc);
    }

    auto start = std::chrono::high_resolution_clock::now();

    std::string abcSeq =
        "strash; dc2; "
        "balance; rewrite; balance; rewrite; rewrite -z; balance; rewrite -z; balance";

    std::string abcCmd = abcPath_ + " -c \"read_blif " + rawBlifPath.string() +
                         "; " + abcSeq +
                         "; write_blif " + abcBlifPath.string() +
                         "\" > " + logPath.string() + " 2>&1";

    int ret = system(abcCmd.c_str());

    auto end = std::chrono::high_resolution_clock::now();
    long long runtimeMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    if (ret != 0 || !fs::exists(abcBlifPath)) {
    std::cerr << "[ABC-LIB] Failed case: " << variantName
              << " | log: " << logPath << std::endl;
    csvOut << hexFunc << "," << probTag << ",false,-1,-1," << runtimeMs << ",ABC Failed\n";
    return false;
}

// 读回优化后的 BLIF 内容
std::ifstream ifs(abcBlifPath.string());
std::string abcContent((std::istreambuf_iterator<char>(ifs)),
                       (std::istreambuf_iterator<char>()));

bool hexOk = false;
try {
    hexOk = validateBlifImplementsHex(abcContent, hexFunc);
} catch (const std::exception& e) {
    std::cerr << "[ABC-LIB] Hex validation exception for " << variantName
              << ": " << e.what() << std::endl;
    csvOut << hexFunc << "," << probTag << ",false,-1,-1," << runtimeMs << ",HexCheckException\n";
    return false;
}

if (!hexOk) {
    std::cerr << "[ABC-LIB] Hex mismatch for " << variantName
              << " | log: " << logPath << std::endl;
    csvOut << hexFunc << "," << probTag << ",false,-1,-1," << runtimeMs << ",HexMismatch\n";
    return false;
}

int gates = countNamesInBlif(abcBlifPath.string());
double internalCost = static_cast<double>(gates >= 0 ? gates : -1);

csvOut << hexFunc << ","
       << probTag << ","
       << "true,"
       << gates << ","
       << internalCost << ","
       << runtimeMs << ","
       << "\n";

return true;
}

    bool SynthesisFlow::buildABCLocalLibraryFromTopCsv(
        const std::string& topCsvPath,
        const std::string& outputDir)
    {
        namespace fs = std::filesystem;

        auto hexFuncs = loadTopHexFuncsFromCsv(topCsvPath);
        if (hexFuncs.empty()) {
            std::cerr << "[ABC-LIB] No hex funcs loaded from: " << topCsvPath << std::endl;
            return false;
        }

        fs::path outDir(outputDir);
        fs::path detailedDir = outDir / "detailed_infos";
        if (!fs::exists(outDir)) fs::create_directories(outDir);
        if (!fs::exists(detailedDir)) fs::create_directories(detailedDir);

        fs::path csvPath = outDir / "final_results.csv";
        std::ofstream csvOut(csvPath.string());
        if (!csvOut.is_open()) {
            std::cerr << "[ABC-LIB] Failed to create: " << csvPath << std::endl;
            return false;
        }

        csvOut << "HexFunc,ProbPattern,Success,Gates,InternalCost,RuntimeMs,Error\n";

        // 概率模板直接写死在这里
        std::vector<std::vector<double>> probPatterns;
        for(int i = 1; i <= 9; i++){
            double t = 0.1 * i;
            probPatterns.push_back({t, t, t, t});
        }
        int totalCases = 0;
        int successCases = 0;

        std::cout << "[ABC-LIB] Start building local library from: " << topCsvPath << std::endl;
        std::cout << "[ABC-LIB] Output directory: " << outputDir << std::endl;

        for (const auto& hexFunc : hexFuncs) {
            for (const auto& probs : probPatterns) {
                totalCases++;
                bool ok = runSingleABCLocalCase(hexFunc, probs, outputDir, csvOut);
                if (ok) successCases++;
            }
        }

        csvOut.close();

        std::cout << "[ABC-LIB] Build finished. Success "
                << successCases << "/" << totalCases << std::endl;

        return successCases > 0;
    }

    std::vector<std::string> SynthesisFlow::loadTopHexFuncsFromCsv(const std::string& topCsvPath) {
        std::vector<std::string> hexFuncs;

        std::ifstream ifs(topCsvPath);
        if (!ifs.is_open()) {
            std::cerr << "[ABC-LIB] Failed to open top csv: " << topCsvPath << std::endl;
            return hexFuncs;
        }

        std::string line;
        bool isHeader = true;

        while (std::getline(ifs, line)) {
            if (isHeader) {
                isHeader = false;
                continue;
            }
            if (line.empty()) continue;

            std::stringstream ss(line);
            std::string hexFunc;
            std::getline(ss, hexFunc, ',');

            hexFunc.erase(std::remove_if(hexFunc.begin(), hexFunc.end(), ::isspace), hexFunc.end());
            std::transform(hexFunc.begin(), hexFunc.end(), hexFunc.begin(), ::toupper);

            while (hexFunc.size() < 4) hexFunc = "0" + hexFunc;
            if (!hexFunc.empty()) hexFuncs.push_back(hexFunc);
        }

        std::cout << "[ABC-LIB] Loaded " << hexFuncs.size()
                << " hex funcs from " << topCsvPath << std::endl;

        return hexFuncs;
    }

    std::string SynthesisFlow::probVectorToTag(const std::vector<double>& probs) const {
        std::ostringstream oss;
        for (double p : probs) {
            int v = static_cast<int>(std::round(p * 100.0));
            oss << "_" << v;
        }
        return oss.str();
    }

    bool SynthesisFlow::evaluateCubeMatch(const std::string& cube, int mask, int nInputs) const {
        for (int i = 0; i < nInputs && i < (int)cube.size(); ++i) {
            char c = cube[i];
            if (c == '-') continue;

            int bit = (mask >> i) & 1;
            if (c == '1' && bit != 1) return false;
            if (c == '0' && bit != 0) return false;
        }
        return true;
    }

   double SynthesisFlow::evaluateNamesNodeTruth(
    const std::vector<std::string>& sop,
    int mask,
    int nInputs) const
{
    // 常 0：没有任何 cover
    if (sop.empty()) return 0.0;

    int cnt0 = 0, cnt1 = 0;
    for (const auto& row : sop) {
        std::stringstream ss(row);
        std::string cube, outVal;
        if (!(ss >> cube)) continue;

        if (!(ss >> outVal)) {
            // 常 1 的特殊写法：.names y 后面单独一行 "1"
            if (nInputs == 0 && cube == "1") return 1.0;
            continue;
        }

        if (outVal == "0") cnt0++;
        else if (outVal == "1") cnt1++;
    }

    // 如果是 0-cover 为主，则表示列出的 cube 输出为 0，其余为 1
    bool isZeroCover = (cnt0 > cnt1);

    bool covered = false;
    for (const auto& row : sop) {
        std::stringstream ss(row);
        std::string cube, outVal;
        if (!(ss >> cube >> outVal)) continue;

        if (evaluateCubeMatch(cube, mask, nInputs)) {
            covered = true;
            if (isZeroCover) {
                return (outVal == "0") ? 0.0 : 1.0;
            } else {
                return (outVal == "1") ? 1.0 : 0.0;
            }
        }
    }

    // 没有匹配到任何 cover
    // 1-cover 语义：默认 0
    // 0-cover 语义：默认 1
    return isZeroCover ? 1.0 : 0.0;
}

    uint16_t SynthesisFlow::computeHexFromBlifFragment(const std::string& blifContent) const {
        struct NamesNode {
            std::vector<std::string> inputs;
            std::string output;
            std::vector<std::string> sop;
        };

        std::vector<std::string> primaryInputs;
        std::string primaryOutput;
        std::vector<NamesNode> nodes;

        {
            std::stringstream ss(blifContent);
            std::string line;

            while (std::getline(ss, line)) {
                if (line.empty()) continue;

                if (line.rfind(".inputs", 0) == 0) {
                    std::stringstream ls(line.substr(7));
                    std::string tok;
                    while (ls >> tok) {
                        if (tok != "\\" && !tok.empty()) primaryInputs.push_back(tok);
                    }
                } else if (line.rfind(".outputs", 0) == 0) {
                    std::stringstream ls(line.substr(8));
                    std::string tok;
                    while (ls >> tok) {
                        if (tok != "\\" && !tok.empty()) {
                            primaryOutput = tok;
                            break;
                        }
                    }
                } else if (line.rfind(".names", 0) == 0) {
                    std::stringstream ls(line);
                    std::string dotNames, tok;
                    ls >> dotNames;

                    std::vector<std::string> ports;
                    while (ls >> tok) {
                        if (tok != "\\" && !tok.empty()) ports.push_back(tok);
                    }

                    if (ports.empty()) continue;

                    NamesNode node;
                    node.output = ports.back();
                    ports.pop_back();
                    node.inputs = ports;

                    while (ss.peek() != EOF) {
                        std::streampos pos = ss.tellg();
                        std::string sopLine;
                        if (!std::getline(ss, sopLine)) break;

                        if (sopLine.empty()) continue;
                        if (sopLine[0] == '.') {
                            ss.seekg(pos);
                            break;
                        }

                        node.sop.push_back(sopLine);
                    }

                    nodes.push_back(node);
                }
            }
        }

        if (primaryInputs.size() > 4) {
            throw std::runtime_error("[HexCheck] BLIF fragment has more than 4 primary inputs.");
        }

        if (primaryOutput.empty()) {
            throw std::runtime_error("[HexCheck] BLIF fragment has no primary output.");
        }

        uint16_t hexVal = 0;
        int total = 1 << (int)primaryInputs.size();

        for (int mask = 0; mask < total; ++mask) {
            std::map<std::string, double> values;

            // 赋 primary inputs
            for (int i = 0; i < (int)primaryInputs.size(); ++i) {
                values[primaryInputs[i]] = ((mask >> i) & 1) ? 1.0 : 0.0;
            }

            // 按出现顺序求每个 .names 节点
            for (const auto& node : nodes) {
                int localMask = 0;
                for (int i = 0; i < (int)node.inputs.size(); ++i) {
                    const auto& inName = node.inputs[i];
                    int bit = (values.count(inName) && values[inName] > 0.5) ? 1 : 0;
                    localMask |= (bit << i);
                }

                double outVal = evaluateNamesNodeTruth(node.sop, localMask, (int)node.inputs.size());
                values[node.output] = outVal;
            }

            int outBit = (values.count(primaryOutput) && values[primaryOutput] > 0.5) ? 1 : 0;
            if (outBit) hexVal |= (1u << mask);
        }

        // 如果 primaryInputs 少于 4，则高位组合默认扩展
        // 为了和 4-LUT hexFunc 对齐，把低维函数复制扩展到 4 输入空间
        if ((int)primaryInputs.size() < 4) {
            uint16_t expanded = 0;
            int oldVars = (int)primaryInputs.size();
            int oldTotal = 1 << oldVars;

            for (int mask4 = 0; mask4 < 16; ++mask4) {
                int reducedMask = mask4 & (oldTotal - 1);
                if ((hexVal >> reducedMask) & 1u) {
                    expanded |= (1u << mask4);
                }
            }
            hexVal = expanded;
        }

        return hexVal;
    }

    bool SynthesisFlow::validateBlifImplementsHex(
        const std::string& blifContent,
        const std::string& expectedHex) const
    {
        uint16_t actual = computeHexFromBlifFragment(blifContent);

        unsigned expected = 0;
        std::stringstream ss;
        ss << std::hex << expectedHex;
        ss >> expected;

        return actual == static_cast<uint16_t>(expected);
    }

    void SynthesisFlow::debugSingleHexCase(const std::string& hexFunc) {
    namespace fs = std::filesystem;

    fs::path dbgDir = fs::current_path() / "tmp_hex_debug";
    if (!fs::exists(dbgDir)) fs::create_directories(dbgDir);

    fs::path rawBlifPath = dbgDir / ("raw_" + hexFunc + ".blif");
    fs::path abcBlifPath = dbgDir / ("abc_" + hexFunc + ".blif");
    fs::path logPath     = dbgDir / ("abc_" + hexFunc + ".log");

    std::string rawContent = buildRawBlifFromHexFunc(hexFunc);

    {
        std::ofstream ofs(rawBlifPath.string());
        ofs << rawContent;
    }

    std::cout << "\n========== DEBUG HEX " << hexFunc << " ==========\n";
    std::cout << "[1] RAW BLIF:\n" << rawContent << "\n";

    try {
        uint16_t rawHex = computeHexFromBlifFragment(rawContent);
        std::stringstream ss;
        ss << std::uppercase << std::hex << std::setw(4) << std::setfill('0') << rawHex;
        std::cout << "[2] RAW->HEX = " << ss.str() << "\n";
    } catch (const std::exception& e) {
        std::cout << "[2] RAW->HEX exception: " << e.what() << "\n";
    }

    std::string abcSeq =
        "strash; dc2; "
        "balance; rewrite; balance; rewrite; rewrite -z; balance; rewrite -z; balance";

    std::string abcCmd = abcPath_ + " -c \"read_blif " + rawBlifPath.string() +
                         "; " + abcSeq +
                         "; write_blif " + abcBlifPath.string() +
                         "\" > " + logPath.string() + " 2>&1";

    int ret = system(abcCmd.c_str());
    std::cout << "[3] ABC ret = " << ret << "\n";

    if (!fs::exists(abcBlifPath)) {
        std::cout << "[4] ABC output not generated. See log: " << logPath << "\n";
        return;
    }

    std::ifstream ifs(abcBlifPath.string());
    std::string abcContent((std::istreambuf_iterator<char>(ifs)),
                           (std::istreambuf_iterator<char>()));

    std::cout << "[4] ABC BLIF:\n" << abcContent << "\n";

    try {
        uint16_t abcHex = computeHexFromBlifFragment(abcContent);
        std::stringstream ss;
        ss << std::uppercase << std::hex << std::setw(4) << std::setfill('0') << abcHex;
        std::cout << "[5] ABC->HEX = " << ss.str() << "\n";
    } catch (const std::exception& e) {
        std::cout << "[5] ABC->HEX exception: " << e.what() << "\n";
    }

    std::cout << "=========================================\n";
}
} // namespace fes