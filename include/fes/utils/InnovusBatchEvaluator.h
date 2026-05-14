#pragma once
#include <string>
#include <vector>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include "fes/core/GateType.h"
#include "fes/flow/RunManifest.h"
#include "fes/utils/EquivalenceChecker.h"
#include "fes/utils/InnovusVerifier.h"

namespace fes {

// 库项定义
struct LibEntry {
    std::string blifContent;              // 优化的 BLIF 代码段
    std::vector<double> idealActivities;  // 该结构预设的最优引脚翻转频率分布
    double score;                         // 生成阶段记录的 InternalCost
};

struct PPADiff {
    std::string fileName;
    // 三条路径的完整数据
    PPAResult origPPA;      // 原始
    PPAResult abcHighPPA;   // ABC 激进优化
    PPAResult abcLocalPPA;  // ABC 局部库 rewrite
    PPAResult ponoPPA;      // PONO 优化
    bool success = false;
};

inline std::string escapeJsonString(const std::string& input) {
    std::stringstream ss;
    for (char c : input) {
        if (c == '\\') ss << "\\\\";
        else if (c == '"') ss << "\\\"";
        else if (c == '\n') ss << "\\n";
        else if (c == '\r') ss << "\\r";
        else if (c == '\t') ss << "\\t";
        else ss << c;
    }
    return ss.str();
}

struct SingleOptResult {
    bool success = false;
    std::string errorMessage = "";
    double initialPower = 0.0;
    double optimizedPower = 0.0;
    std::string optimizedBlifContent = ""; // 这里直接存优化后的内容

    std::string toJson() const {
        std::stringstream ss;
        ss << "{";
        ss << "\"success\": " << (success ? "true" : "false") << ",";
        ss << "\"errorMessage\": \"" << escapeJsonString(errorMessage) << "\",";
        ss << "\"initialPower\": " << initialPower << ",";
        ss << "\"optimizedPower\": " << optimizedPower << ",";
        // 传输实际的文本内容
        ss << "\"optimizedBlifContent\": \"" << escapeJsonString(optimizedBlifContent) << "\"";
        ss << "}";
        return ss.str();
    }
};

inline std::string ppaResultToJson(const PPAResult& ppa) {
    std::stringstream ss;
    ss << "{";
    ss << "\"valid\": " << (ppa.valid ? "true" : "false") << ",";
    ss << "\"power_total\": " << ppa.power_total << ",";
    ss << "\"power_internal\": " << ppa.power_internal << ",";
    ss << "\"power_switching\": " << ppa.power_switching << ",";
    ss << "\"power_leakage\": " << ppa.power_leakage << ",";
    ss << "\"area\": " << ppa.area << ",";
    ss << "\"delay\": " << ppa.delay;
    ss << "}";
    return ss.str();
}

struct SingleBlifResult {
    bool success = false;
    std::string errorMessage;
    std::string command;
    std::string sourceBlifPath;
    std::string outputDir;
    std::string resultJsonPath;
    std::string stdoutLogPath;
    std::string stderrLogPath;
    std::string selectedStrategy = "NONE";
    std::string optimizedBlifPath;
    std::string optimizedBlifContent;
    std::vector<double> inputProbs;
    std::vector<double> inputActs;
    PPAResult origPPA;
    PPAResult abcHighPPA;
    PPAResult abcLocalPPA;
    PPAResult ponoPPA;
    double runtimeMs = 0.0;

    std::string toJson() const {
        auto vectorToJson = [](const std::vector<double>& values) {
            std::stringstream ss;
            ss << "[";
            for (std::size_t i = 0; i < values.size(); ++i) {
                if (i > 0) ss << ",";
                ss << values[i];
            }
            ss << "]";
            return ss.str();
        };
        auto gainToJson = [](const PPAResult& base, const PPAResult& target) {
            if (!base.valid || !target.valid || base.power_total <= 0.0) {
                return std::string("null");
            }
            const double gain =
                (base.power_total - target.power_total) / base.power_total * 100.0;
            std::stringstream ss;
            ss << gain;
            return ss.str();
        };

        std::stringstream ss;
        ss << "{";
        ss << "\"success\": " << (success ? "true" : "false") << ",";
        ss << "\"errorMessage\": \"" << escapeJsonString(errorMessage) << "\",";
        ss << "\"command\": \"" << escapeJsonString(command) << "\",";
        ss << "\"sourceBlifPath\": \"" << escapeJsonString(sourceBlifPath) << "\",";
        ss << "\"outputDir\": \"" << escapeJsonString(outputDir) << "\",";
        ss << "\"resultJsonPath\": \"" << escapeJsonString(resultJsonPath) << "\",";
        ss << "\"stdoutLogPath\": \"" << escapeJsonString(stdoutLogPath) << "\",";
        ss << "\"stderrLogPath\": \"" << escapeJsonString(stderrLogPath) << "\",";
        ss << "\"selectedStrategy\": \"" << escapeJsonString(selectedStrategy) << "\",";
        ss << "\"optimizedBlifPath\": \"" << escapeJsonString(optimizedBlifPath) << "\",";
        ss << "\"inputProbs\": " << vectorToJson(inputProbs) << ",";
        ss << "\"inputActs\": " << vectorToJson(inputActs) << ",";
        ss << "\"runtimeMs\": " << runtimeMs << ",";
        ss << "\"gainVsOrigPct\": " << gainToJson(origPPA, ponoPPA) << ",";
        ss << "\"gainVsAbcPct\": " << gainToJson(abcHighPPA, ponoPPA) << ",";
        ss << "\"gainVsAbcLocalPct\": " << gainToJson(abcLocalPPA, ponoPPA) << ",";
        ss << "\"orig\": " << ppaResultToJson(origPPA) << ",";
        ss << "\"abc\": " << ppaResultToJson(abcHighPPA) << ",";
        ss << "\"abcLocal\": " << ppaResultToJson(abcLocalPPA) << ",";
        ss << "\"pono\": " << ppaResultToJson(ponoPPA) << ",";
        ss << "\"optimizedBlifContent\": \""
           << escapeJsonString(optimizedBlifContent) << "\"";
        ss << "}";
        return ss.str();
    }
};

struct FragmentPowerInfo {
    double totalSwitching = 0.0; // 内部所有 .names 输出翻转率之和
    int    gateCount = 0;        // .names 个数
    double outputToggle = 0.0;   // 最终输出翻转率
};

class InnovusBatchEvaluator {
public:
    /**
     * 旧构造函数：只加载 PONO 库
     * @param libraryDirPath 存放被评测优化库的路径
     * @param pythonScriptPath 自动化评估脚本路径
     * @param abcPath ABC 可执行文件路径
     */
    InnovusBatchEvaluator(const std::string& libraryDirPath,
                          const std::string& pythonScriptPath,
                          const std::string& abcPath);

    /**
     * 新构造函数：同时加载 PONO 库 和 ABC 局部库
     * @param libraryDirPath 被评测优化库路径（PONO 库）
     * @param abcLocalLibraryDirPath ABC 局部库路径
     * @param pythonScriptPath 自动化评估脚本路径
     * @param abcPath ABC 可执行文件路径
     */
    InnovusBatchEvaluator(const std::string& libraryDirPath,
                          const std::string& abcLocalLibraryDirPath,
                          const std::string& pythonScriptPath,
                          const std::string& abcPath);

    // 统一评测流程：Original / ABC Global / ABC Local / PONO
    void runBatchVerification(const std::string& benchmarksDir);

    SingleOptResult optimizeSingleBlifFromContent(
            const std::string& blifContent, 
            const std::vector<double>& actualProbs);

    SingleBlifResult analyzeSingleBlif(
        const std::string& inputBlifPath,
        const std::vector<double>& inputProbs,
        const std::vector<double>& inputActs,
        const std::filesystem::path& optimizedBlifOutputPath = {},
        bool includeOptimizedBlifContent = false);
            
    // When enabled, every rewrite/optimization path runs a BLIF-level
    // combinational equivalence check against its input before handing
    // back the rewritten file. A failed check discards the rewrite by
    // returning the known-good input path instead.
    void enableVerification(bool on) { verifyEnabled_ = on; }
    void setResumePolicy(ResumePolicy policy) { resumePolicy_ = policy; }
    void setCaseTimeoutMs(int timeoutMs) { caseTimeoutMs_ = timeoutMs; }
    void setOutputRootDir(const std::filesystem::path& outputRoot);

private:
    // 1. 递归获取所有 blif 文件路径
    std::vector<std::string> findBlifFilesRecursive(const std::filesystem::path& folderPath);

    // 2. 读取库
    void loadOptimizationLibrary();      // 加载 PONO 库
    void loadABCOptimizationLibrary();   // 加载 ABC 局部库

    // Knobs shared by every library-rewrite flow. Tuning these recovers the
    // aggressive/conservative PONO tournament plus the ABC local-library
    // mapped rewrite without duplicating the loop.
    struct RewriteConfig {
        // ABC sequence used to pre-map the input BLIF to 4-LUTs. Empty
        // string means the input is already mapped.
        std::string preMapAbcSeq;

        // Score = kSwitchWeight * totalSwitching
        //       + kOutputWeight * outputToggle
        //       + kGateWeight * max(1, gateCount)
        //       + kActivityWeight * activityDistance
        //       + kLibraryScoreWeight * normalizedLibraryScore
        double kSwitchWeight   = 1.0;
        double kGateWeight     = 0.10;
        double kOutputWeight   = 0.0;
        double kActivityWeight = 0.0;
        double kLibraryScoreWeight = 0.0;

        // NPN matching controls. When enabled, the current cut is
        // canonicalized before library lookup; allowNegation gates whether
        // recipes that require explicit inverters are admissible.
        bool enableNpn = false;
        bool allowNegation = false;

        // When true, candidates must beat the original LUT's toggle-based
        // score by (1 - kImproveMargin). When false, any feasible
        // candidate wins -- matches the legacy PONO aggressive/conservative
        // engines.
        bool   useImprovementFilter = false;
        double kImproveMargin       = 0.01;

        // When true, a library fragment may have more inputs than the current
        // LUT. The missing LUT inputs are tied to logic 0 in emitted BLIF.
        // This is useful for LUT-mapped baselines backed by a fixed-K library.
        bool allowPadMissingInputs = false;

        // ABC sequence executed after the per-LUT rewriter emits optBlif.
        // Typical choices:
        //   "sweep; topo"                                -- light touch
        //   "strash; dc2; balance; if -K 4 -a; sweep; topo"  -- heavy pass
        std::string cleanupAbcSeq = "sweep; topo";

        // Tag embedded in log lines and intermediate file names.
        std::string tag = "PONO_LIB";
    };

    // Shared rewrite core. Returns the path to the resulting BLIF (after
    // CEC verification when --verify is active) or the input path if the
    // pre-mapping step fails.
    std::string rewriteBlifUnified(
        const std::string& inputBlifPath,
        const std::vector<double>& actualProbs,
        const std::map<std::string, std::vector<LibEntry>>& targetLib,
        const RewriteConfig& cfg);

    // 3. 旧重写流程
    std::string rewriteBlifWithLibrary(const std::string& originalBlifPath,
                                       const std::vector<double>& actualProbs,
                                       bool isAggressive = true);

    int selectBestEntry(const std::vector<LibEntry>& candidates,
                        const std::vector<double>& currentProbs,
                        double outToggleRate,
                        int origGateCost);

    // 4. 旧 ABC 强流
    std::string runABCExhaustiveOpt(const std::string& inputBlif);

    // 只做 LUT4 mapping，供 ABC local-library rewrite 复用
    std::string run4LutMappingOnly(const std::string& inputBlif);

    // 通用 LUT-rewrite 主循环：给定一个库（ABC 或 PONO）
    std::string rewriteMappedBlifWithGivenLibrarySimple(
        const std::string& mappedBlifPath,
        const std::vector<double>& actualProbs,
        const std::map<std::string, std::vector<LibEntry>>& targetLib,
        const std::string& tag);

    // 包装：用 ABC 局部库做 rewrite
    std::string rewriteMappedBlifWithABCLibrarySimple(
        const std::string& mappedBlifPath,
        const std::vector<double>& actualProbs);


    // 辅助：计算 SOP 输出概率
    double computeSopOutputProb(const std::vector<std::string>& sop,
                                const std::vector<double>& inProbs);

    // 辅助：估算一个候选 BLIF 片段的内部翻转
    FragmentPowerInfo estimateFragmentSwitching(
        const std::string& blifContent,
        const std::vector<double>& inputProbs);

    // Shared CEC helper: runs the BLIF miter and, on failure, returns the
    // input path so downstream PPA evaluation never sees a broken rewrite.
    // Logs counterexample under "[CEC][Rewrite]".
    std::string verifyRewriteOrRevert(const std::string& inputPath,
                                      const std::string& rewrittenPath,
                                      const std::string& tag);

private:
    std::string libPath_;          // PONO 库路径
    std::string abcLocalLibPath_;  // ABC 局部库路径（新增）
    std::string abcPath_;
    InnovusVerifier verifier_;

    // 核心存储：Hex_func -> 候选方案集合
    std::map<std::string, std::vector<LibEntry>> hexMappingLib_;     // PONO 库
    std::map<std::string, std::vector<LibEntry>> abcHexMappingLib_;  // ABC 局部库（新增）

    // --verify support: BLIF-level CEC (Z3-backed) for every rewrite.
    bool verifyEnabled_ = false;
    std::vector<GateType> cecLibrary_;            // Empty -- BLIF CEC ignores it.
    std::unique_ptr<EquivalenceChecker> cec_;

    ResumePolicy resumePolicy_ = ResumePolicy::kRunAll;
    int caseTimeoutMs_ = 0;
    std::filesystem::path outputRootPath_;
    std::filesystem::path workDir_;
};

} // namespace fes
