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

// 新增：mapped-origin 四路比较结果
struct MappedFourWayResult {
    std::string fileName;

    PPAResult mappedOrigPPA;   // LUT4 mapped 后作为新的 Original
    PPAResult abcLocalPPA;     // ABC 局部库，同 LUT-rewrite 流程
    PPAResult abcGlobalPPA;    // ABC 强命令流
    PPAResult ponoLocalPPA;    // PONO 局部库，同 LUT-rewrite 流程

    bool mappedOrigValid = false;
    bool abcLocalValid   = false;
    bool abcGlobalValid  = false;
    bool ponoLocalValid  = false;

    bool success = false;      // 四路都成功时为 true
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

    /**
     * 旧三路流程：Original / ABC Global / PONO
     */
    void runBatchVerification(const std::string& benchmarksDir);

    SingleOptResult optimizeSingleBlifFromContent(
            const std::string& blifContent, 
            const std::vector<double>& actualProbs);
            
    /**
     * 新四路流程：
     * Mapped-Origin / ABC-Local / ABC-Global / PONO-Local
     */
    void runBatchVerificationMappedFourWay(const std::string& benchmarksDir);

    // When enabled, every rewrite/optimization path runs a BLIF-level
    // combinational equivalence check against its input before handing
    // back the rewritten file. A failed check discards the rewrite by
    // returning the known-good input path instead.
    void enableVerification(bool on) { verifyEnabled_ = on; }
    void setResumePolicy(ResumePolicy policy) { resumePolicy_ = policy; }
    void setCaseTimeoutMs(int timeoutMs) { caseTimeoutMs_ = timeoutMs; }

private:
    // 1. 递归获取所有 blif 文件路径
    std::vector<std::string> findBlifFilesRecursive(const std::filesystem::path& folderPath);

    // 2. 读取库
    void loadOptimizationLibrary();      // 加载 PONO 库
    void loadABCOptimizationLibrary();   // 加载 ABC 局部库

    // Knobs shared by every library-rewrite flow. Tuning these recovers the
    // two legacy engines (aggressive/conservative PONO on raw BLIF, and the
    // mapped-origin four-way flows) without duplicating the loop.
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

    // =========================
    // 新增：mapped-origin 四路功能
    // =========================

    // 只做 LUT4 mapping，输出 mapped netlist
    std::string run4LutMappingOnly(const std::string& inputBlif);

    // 在 mapped netlist 上跑强 ABC 命令流
    std::string runABCGlobalStrongOnMapped(const std::string& mappedBlif);

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

    // 包装：用 PONO 库做 rewrite
    std::string rewriteMappedBlifWithPONOLibrarySimple(
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
    std::filesystem::path workDir_;
};

} // namespace fes
