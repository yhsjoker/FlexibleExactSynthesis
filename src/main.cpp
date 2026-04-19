#include "fes/core/GateType.h"
#include "fes/flow/SynthesisFlow.h"
#include "fes/utils/BenchmarkExtractor.h"
#include "fes/utils/InnovusVerifier.h"
#include "fes/utils/InnovusBatchEvaluator.h"
#include <iostream>
#include <vector>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <string>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <regex>

using namespace fes;
namespace fs = std::filesystem;

std::vector<GateType> createLibrary() {
    std::vector<GateType> lib;
    
    // =========================================================
    // 0. 常量单元 (Constants)
    // 用于实现恒0 (0000) 或恒1 (FFFF) 逻辑，防止 SMT 用复杂门去凑
    // =========================================================
    // CONST0: 无论输入什么，输出永远是 0
    lib.emplace_back("CONST0", 0, 0x0000, 5.32, 0.266, 0.0);
    // CONST1: 无论输入什么，输出永远是 1
    lib.emplace_back("CONST1", 0, 0xFFFF, 5.32, 0.266, 0.0);

    // =========================================================
    // 1. 基础门 (Basic Gates)
    // =========================================================
    // INV: !A  -> 01 (bit0:f(0)=1, bit1:f(1)=0) -> 0x1
    lib.emplace_back("INV", 1, 0x1, 14.35, 0.532, 10.0);
    // BUF: A   -> 10 (bit0:f(0)=0, bit1:f(1)=1) -> 0x2
    lib.emplace_back("BUF", 1, 0x2, 21.44, 0.798, 20.0); 

    // =========================================================
    // 2. 与非/或非门 (NAND/NOR Family) —— CMOS 原生高效单元
    // =========================================================
    // NAND2: !(A&B) -> 0111 -> 0x7
    lib.emplace_back("NAND2", 2, 0x7, 17.39, 0.798, 15.0);    
    // NAND3: 只有 111 为 0，其余为 1 -> 0111 1111 -> 0x7F
    lib.emplace_back("NAND3", 3, 0x7F, 18.10, 1.064, 22.0);   
    // NAND4: 只有 1111 为 0 -> 0111 1111 1111 1111 -> 0x7FFF
    lib.emplace_back("NAND4", 4, 0x7FFF, 18.13, 1.330, 30.0); 

    // NOR2: !(A|B) -> 0001 -> 0x1
    lib.emplace_back("NOR2", 2, 0x1, 21.20, 0.798, 18.0);     
    // NOR3: 只有 000 为 1 -> 0000 0001 -> 0x01
    lib.emplace_back("NOR3", 3, 0x01, 26.83, 1.064, 26.0);    
    // NOR4: 只有 0000 为 1 -> 0x0001
    lib.emplace_back("NOR4", 4, 0x0001, 32.60, 1.330, 35.0);  

    // =========================================================
    // 3. 标准逻辑门 (Standard Logic) 
    // =========================================================
    // AND2: A&B -> 1000 -> 0x8
    lib.emplace_back("AND2", 2, 0x8, 25.07, 1.064, 25.0);
    // OR2:  A|B -> 1110 -> 0xE
    lib.emplace_back("OR2", 2, 0xE, 22.69, 1.064, 25.0);
    
    // XOR2: A^B -> 0110 -> 0x6
    lib.emplace_back("XOR2", 2, 0x6, 36.16, 1.596, 40.0);
    // XNOR2: !(A^B) -> 1001 -> 0x9
    lib.emplace_back("XNOR2", 2, 0x9, 36.44, 1.596, 40.0);

    // =========================================================
    // 4. 复合逻辑门 (AOI/OAI) —— 论文中功耗优化的关键
    // =========================================================
    // AOI21: !((A&B)|C) 
    // 逻辑：当 (A=1 且 B=1) 或 C=1 时，输出 0
    // 位序：C(MSB), B, A(LSB) -> 0000 0111 -> 0x07
    lib.emplace_back("AOI21", 3, 0x07, 27.86, 1.064, 20.0);

    // AOI22: !((A&B)|(C&D)) 
    // 逻辑：AB同时为1 或 CD同时为1 时，输出 0
    // 真值表：1110 1110 1110 0000 (二进制倒序) -> 0x0777
    lib.emplace_back("AOI22", 4, 0x0777, 32.61, 1.330, 25.0);

    // OAI21: !((A|B)&C) 
    // 逻辑：当 (A=1 或 B=1) 且 C=1 时，输出 0
    // 真值表：0001 1111 -> 0x1F
    lib.emplace_back("OAI21", 3, 0x1F, 22.62, 1.064, 20.0);

    // OAI22: !((A|B)&(C|D)) 
    // 逻辑：当 (A或B为1) 且 (C或D为1) 时，输出 0
    // 真值表：0001 0001 0001 1111 -> 0x111F
    lib.emplace_back("OAI22", 4, 0x111F, 34.03, 1.330, 25.0);

    return lib;
}

std::string getCurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%Y%m%d_%H%M%S");
    return ss.str();
}

// 辅助函数：检测文件是否存在
bool fileExists(const std::string& name) {
    std::ifstream f(name.c_str());
    return f.good();
}

struct RunConfig {
    // 基础路径
    fs::path projectRoot;
    fs::path resourceDir;
    fs::path resultsRepoDir;
    fs::path outputDir;
    
    // 外部工具与数据
    std::string abcPath;
    std::string benchmarkDir;
    
    // 关键文件
    fs::path mcncGenlib;
    fs::path topFuncsCsv;       // 提取出的特征文件
    fs::path finalResultsCsv;   // 综合后的结果文件
    fs::path outLibPath;        // 生成的 .lib 文件
    
    fs::path abcLocalLibDir;          
    fs::path abcLocalFinalResultsCsv; 

    // 运行标识
    std::string runName;
};

// [功能 1] 环境初始化与路径配置
RunConfig setupEnvironment(int argc, char** argv, bool isApiMode = false) {
    RunConfig cfg;
    
    // 如果是 API 模式，将日志输出重定向到 std::cerr，否则正常输出到 std::cout
    auto& logOut = isApiMode ? std::cerr : std::cout;

    if (!isApiMode) {
        logOut << "==========================================" << std::endl;
        logOut << "      Automated Synthesis Framework       " << std::endl;
        logOut << "==========================================" << std::endl;
    }

    // 1. 路径定位
    fs::path executionPath = fs::current_path();
    cfg.projectRoot = executionPath.parent_path();

    // 健壮性检查
    if (!fs::exists(cfg.projectRoot / "resources") && fs::exists(executionPath / "resources")) {
        cfg.projectRoot = executionPath;
    }

    cfg.resourceDir = cfg.projectRoot / "resources";
    cfg.resultsRepoDir = cfg.projectRoot / "results_repo";

    // 2. 外部环境配置 (绝对路径)
    cfg.abcPath = "/home/yhs_joker/softwares/abc/abc";
    cfg.benchmarkDir = "/home/yhs_joker/datasets/benchmarks";

    // 3. 资源文件定位
    cfg.mcncGenlib = cfg.resourceDir / "nangate_45nm.genlib";
    cfg.topFuncsCsv = cfg.resourceDir / "top_50_funcs.csv";

    // 4. 运行名称与输出目录
    // 兼容原来的批处理逻辑，避开 "-c" 和 "-r" 等 API 参数
    cfg.runName = (argc > 1 && std::string(argv[1]) != "-c" && std::string(argv[1]) != "-r") 
                  ? argv[1] : ("run_" + getCurrentTimestamp()); 
    
    cfg.outputDir = cfg.resultsRepoDir / cfg.runName;
    
    // 派生路径
    cfg.finalResultsCsv = cfg.outputDir / "final_results.csv";
    cfg.outLibPath = cfg.outputDir / "pono_optimized.lib";

    // cfg.abcLocalLibDir = cfg.resultsRepoDir / (cfg.runName + "_abc_local");
    cfg.abcLocalLibDir = cfg.resultsRepoDir / "local_abc_lib";
cfg.abcLocalFinalResultsCsv = cfg.abcLocalLibDir / "final_results.csv";

    // 5. 检查与创建目录
    if (!fs::exists(cfg.mcncGenlib)) {
        throw std::runtime_error("[Fatal] 'mcnc.genlib' not found in resources!");
    }

    if (!fs::exists(cfg.outputDir)) {
        fs::create_directories(cfg.outputDir);
        if (!isApiMode) logOut << "[Config] Created run directory: " << cfg.outputDir << std::endl;
    }

    if (!isApiMode) {
        logOut << "[Config] Project Root: " << fs::absolute(cfg.projectRoot) << std::endl;
        logOut << "[Config] Output Dir:   " << fs::absolute(cfg.outputDir) << std::endl;
    }

    if (!fs::exists(cfg.outputDir)) {
        fs::create_directories(cfg.outputDir);
        if (!isApiMode) logOut << "[Config] Created run directory: " << cfg.outputDir << std::endl;
    }

    if (!fs::exists(cfg.abcLocalLibDir)) {
        fs::create_directories(cfg.abcLocalLibDir);
        if (!isApiMode) logOut << "[Config] Created ABC local lib directory: " << cfg.abcLocalLibDir << std::endl;
    }

    return cfg;
}
// [功能 2] Phase 1: 特征提取
void runPhase1_Extraction(const RunConfig& cfg) {
    std::cout << "\n[Phase 1] Checking Feature Extraction..." << std::endl;

    if (fs::exists(cfg.topFuncsCsv)) {
        std::cout << " -> Benchmark CSV found. Skipping extraction." << std::endl;
    } else {
        std::cout << " -> Benchmark CSV not found. Extracting from: " << cfg.benchmarkDir << std::endl;
        
        fes::BenchmarkExtractor extractor(cfg.abcPath);
        extractor.processDirectory(cfg.benchmarkDir);
        extractor.exportTopHexFuncs(cfg.topFuncsCsv.string(), 100);

        std::cout << " -> Extraction Done. Saved to: " << cfg.topFuncsCsv << std::endl;
    }
}

void runPhase2_Synthesis(const RunConfig& cfg) {
    std::cout << "\n[Phase 2] Starting Synthesis Batch Run (Library Generation Mode)..." << std::endl;

    // 这里假设 createLibrary 是一个你可以调用的函数，或者你需要在这里实例化
    auto lib = createLibrary(); 
    std::string pythonScript = (cfg.projectRoot / "scripts" / "single_power_run.py").string();
    fes::SynthesisFlow flow(lib, cfg.abcPath, cfg.mcncGenlib.string(), pythonScript);
    
    bool enablePhysicalEval = false; 
    
    // 运行批量综合，传入开关参数
    flow.runBatch(cfg.topFuncsCsv.string(), cfg.finalResultsCsv.string(), cfg.outputDir.string(), enablePhysicalEval);
    
    std::cout << " -> Synthesis Done. Library generated in: " << cfg.outputDir.string() << "/detailed_infos/" << std::endl;
    std::cout << " -> Summary CSV in: " << cfg.finalResultsCsv << std::endl;
}

void runPhase2_ABC_Local_Library_Build(const RunConfig& cfg) {
    std::cout << "\n[Phase 2B] Building ABC Local Library..." << std::endl;

    auto lib = createLibrary();
    std::string pythonScript = (cfg.projectRoot / "scripts" / "single_power_run.py").string();

    // 注意：这里第三个参数依然传 genlib 路径，和你原来的 SynthesisFlow 构造保持一致
    fes::SynthesisFlow flow(lib, cfg.abcPath, cfg.mcncGenlib.string(), pythonScript);

    // 如果已经存在，可以选择跳过
    if (fs::exists(cfg.abcLocalFinalResultsCsv)) {
        std::cout << " -> ABC local library index already exists. Skipping build." << std::endl;
        std::cout << " -> Existing index: " << cfg.abcLocalFinalResultsCsv << std::endl;
        return;
    }

    bool ok = flow.buildABCLocalLibraryFromTopCsv(
        cfg.topFuncsCsv.string(),
        cfg.abcLocalLibDir.string());

    if (!ok) {
        std::cerr << "[Phase 2B Error] Failed to build ABC local library." << std::endl;
        return;
    }

    std::cout << " -> ABC local library build done." << std::endl;
    std::cout << " -> Library dir: " << cfg.abcLocalLibDir << std::endl;
    std::cout << " -> Summary CSV: " << cfg.abcLocalFinalResultsCsv << std::endl;
}

// [新功能] Phase 3: 物理验证与重写对比 (Innovus Validation)
void runPhase3_Validation(const RunConfig& cfg) {
    std::cout << "\n[Phase 3] Starting Innovus PPA Validation..." << std::endl;

    // 1. 设置相关的自动化脚本路径
    // 假设脚本在项目根目录下的 scripts 文件夹中
    std::string pythonScript = (cfg.projectRoot / "scripts" / "single_power_run.py").string();

    // 2. 初始化批量评估器
    // 参数：优化库路径 (当前运行目录), Python 脚本路径, ABC 路径
    fes::InnovusBatchEvaluator evaluator(cfg.outputDir.string(), pythonScript, cfg.abcPath);

    // 3. 执行批量验证
    // 它会递归扫描 cfg.benchmarkDir，重写电路，并对比 PPA
    evaluator.runBatchVerification(cfg.benchmarkDir);

    std::cout << " -> Validation Done. Comparison CSV saved to: " << cfg.outputDir << "/final_results.csv" << std::endl;
}

// [新功能] Phase 3B: 四路物理验证与重写对比 (Mapped-Origin Four-Way Validation)
void runPhase3_MappedFourWayValidation(const RunConfig& cfg) {
    std::cout << "\n[Phase 3B] Starting Four-Way Innovus PPA Validation..." << std::endl;

    // 1. 自动化脚本路径
    std::string pythonScript = (cfg.projectRoot / "scripts" / "single_power_run.py").string();

    // 2. 初始化批量评估器
    // 参数：
    //   - PONO 库路径
    //   - ABC 局部库路径
    //   - Python 脚本路径
    //   - ABC 路径
    fes::InnovusBatchEvaluator evaluator(
        cfg.outputDir.string(),
        cfg.abcLocalLibDir.string(),
        pythonScript,
        cfg.abcPath
    );

    // 3. 执行新的四路批量验证
    evaluator.runBatchVerificationMappedFourWay(cfg.benchmarkDir);

    std::cout << " -> Four-Way Validation Done." << std::endl;
    std::cout << " -> Comparison CSV saved to: "
              << (cfg.outputDir / "ppa_mapped_four_way_validation.csv") << std::endl;
}

int main(int argc, char** argv) {
    try {
        // ==========================================================
        // 🚀 新增分支：API 内存流调用模式 (供 Java 后端跨 WSL 调用)
        // 调用方式：cat my_circuit.blif | ./fes_app -c prob1 prob2 ...
        // ==========================================================
        if (argc >= 2 && std::string(argv[1]) == "-c") {
            // 1. 解析传入的概率参数 (从 argv[2] 开始)
            std::vector<double> probs;
            for (int i = 2; i < argc; ++i) {
                probs.push_back(std::stod(argv[i]));
            }

            // 2. 从标准输入 (stdin) 读取 Java 传过来的完整 blif 文本内容
            std::string blifContent;
            std::string line;
            while (std::getline(std::cin, line)) {
                blifContent += line + "\n";
            }

            // 3. 初始化环境
            // 注意：你需要确保你的 setupEnvironment 支持第三个参数 isApiMode
            // 如果 isApiMode = true，setupEnvironment 内部不应该用 std::cout 打印任何进度日志
            RunConfig cfg = setupEnvironment(argc, argv, true); 
            std::string pythonScript = (cfg.projectRoot / "scripts" / "single_power_run.py").string();
            
            // 4. 初始化评估器并执行
            // 确保 cfg.outputDir 下有你之前 Phase 2 跑好的 detailed_infos 库
            fes::InnovusBatchEvaluator evaluator(cfg.outputDir.string(), pythonScript, cfg.abcPath);
            SingleOptResult optResult = evaluator.optimizeSingleBlifFromContent(blifContent, probs);

            // 5. 【绝对关键】唯一输出 JSON，供 Java 后端抓取
            std::cout << optResult.toJson() << std::endl;
            
            return optResult.success ? 0 : 1;
        }


        // ==========================================================
        // ⚙️ 原有分支：本地批处理与库生成模式 (你平时跑实验用的)
        // 调用方式：./fes_app [run_name]
        // ==========================================================
        RunConfig cfg = setupEnvironment(argc, argv, false); // false 表示允许打印普通日志
        // runPhase2_ABC_Local_Library_Build(cfg);
        runPhase3_MappedFourWayValidation(cfg);
        bool enable_extraction  = false; // Phase 1
        bool enable_synthesis   = false; // Phase 2
        bool enable_validation  = false;  // Phase 3: 逻辑重写与 Innovus 验证
        
        // Phase 1: 提取 Benchmark 特征
        if (enable_extraction) {
            runPhase1_Extraction(cfg);
        }

        // Phase 2: 运行 PONO 综合 (生成 temp blif 和 csv)
        if (enable_synthesis) {
            runPhase2_Synthesis(cfg);
        }

        if (enable_validation) {
            // 确保库目录存在 (即至少运行过 Phase 2)
            if (!fs::exists(cfg.outputDir / "detailed_infos")) {
                std::cerr << "[Error] Optimized library (detailed_infos) not found in: " 
                          << cfg.outputDir << "\nDid you run Phase 2?" << std::endl;
                return -1;
            }
            runPhase3_Validation(cfg);
        }

        std::cout << "\n==========================================" << std::endl;
        std::cout << "All requested tasks completed." << std::endl;
        std::cout << "==========================================" << std::endl;

    } catch (const std::exception& e) {
        // 如果在 API 模式下崩溃，返回一个兜底的 JSON 字符串给 Java，防止后端 JSON 解析报错
        if (argc >= 2 && std::string(argv[1]) == "-c") {
            std::cerr << "[Fatal Error in API Mode] " << e.what() << std::endl;
            std::cout << "{\"success\": false, \"errorMessage\": \"Fatal C++ Exception\", \"initialPower\": 0, \"optimizedPower\": 0, \"optimizedBlifContent\": \"\"}" << std::endl;
        } else {
            std::cerr << "\n[Fatal Error] " << e.what() << std::endl;
        }
        return -1;
    }

    return 0;
}