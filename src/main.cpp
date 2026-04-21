#include "fes/core/GateType.h"
#include "fes/core/Types.h"
#include "fes/app/AppConfig.h"
#include "fes/app/GenerateCli.h"
#include "fes/flow/SynthesisFlow.h"
#include "fes/utils/CellLibraryLoader.h"
#include "fes/utils/InnovusBatchEvaluator.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace fes;
namespace fs = std::filesystem;

using fes::app::GenerateOptions;
using fes::app::EvaluationOptions;
using fes::app::parseGenerateOptions;
using fes::app::parseEvaluationOptions;
using fes::app::resolveGenerateOutputDir;

namespace {

constexpr const char* kDefaultAbcPath = "/home/yhs_joker/softwares/abc/abc";
constexpr const char* kDefaultBenchmarkPath = "/home/yhs_joker/datasets/benchmarks";

struct AppContext {
    fs::path projectRoot;
    fs::path resourcesDir;
    fs::path resultsRepoDir;
    fs::path tmpEvalDir;

    fs::path pythonScriptPath;
    fs::path genlibPath;
    fs::path libertyPath;
    fs::path standardCellCsvPath;

    std::string abcPath = kDefaultAbcPath;
    fs::path defaultBenchmarkDir = kDefaultBenchmarkPath;
    unsigned workerCount = 1;
};

std::vector<GateType> createLibrary() {
    std::vector<GateType> lib;

    lib.emplace_back("CONST0", 0, 0x0000, 5.32, 0.266, 0.0);
    lib.emplace_back("CONST1", 0, 0xFFFF, 5.32, 0.266, 0.0);

    lib.emplace_back("INV", 1, 0x1, 14.35, 0.532, 10.0);
    lib.emplace_back("BUF", 1, 0x2, 21.44, 0.798, 20.0);

    lib.emplace_back("NAND2", 2, 0x7, 17.39, 0.798, 15.0);
    lib.emplace_back("NAND3", 3, 0x7F, 18.10, 1.064, 22.0);
    lib.emplace_back("NAND4", 4, 0x7FFF, 18.13, 1.330, 30.0);

    lib.emplace_back("NOR2", 2, 0x1, 21.20, 0.798, 18.0);
    lib.emplace_back("NOR3", 3, 0x01, 26.83, 1.064, 26.0);
    lib.emplace_back("NOR4", 4, 0x0001, 32.60, 1.330, 35.0);

    lib.emplace_back("AND2", 2, 0x8, 25.07, 1.064, 25.0);
    lib.emplace_back("OR2", 2, 0xE, 22.69, 1.064, 25.0);
    lib.emplace_back("XOR2", 2, 0x6, 36.16, 1.596, 40.0);
    lib.emplace_back("XNOR2", 2, 0x9, 36.44, 1.596, 40.0);

    lib.emplace_back("AOI21", 3, 0x07, 27.86, 1.064, 20.0);
    lib.emplace_back("AOI22", 4, 0x0777, 32.61, 1.330, 25.0);
    lib.emplace_back("OAI21", 3, 0x1F, 22.62, 1.064, 20.0);
    lib.emplace_back("OAI22", 4, 0x111F, 34.03, 1.330, 25.0);

    return lib;
}

std::string getCurrentTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto inTimeT = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&inTimeT), "%Y%m%d_%H%M%S");
    return ss.str();
}

std::string toLowerCopy(std::string text) {
    std::transform(
        text.begin(),
        text.end(),
        text.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
    return text;
}

bool consumeOption(const std::vector<std::string>& args,
                   size_t& index,
                   const std::string& name,
                   std::string* valueOut) {
    const std::string prefix = name + "=";
    if (args[index] == name) {
        if (index + 1 >= args.size()) {
            throw std::runtime_error("Missing value for option " + name);
        }
        *valueOut = args[++index];
        return true;
    }
    if (args[index].rfind(prefix, 0) == 0) {
        *valueOut = args[index].substr(prefix.size());
        return true;
    }
    return false;
}

int parseIntOption(const std::string& text, const std::string& name) {
    try {
        size_t consumed = 0;
        const int value = std::stoi(text, &consumed);
        if (consumed != text.size()) {
            throw std::runtime_error("");
        }
        return value;
    } catch (...) {
        throw std::runtime_error(
            "Invalid integer for " + name + ": " + text);
    }
}

fs::path firstExistingPath(const std::vector<fs::path>& candidates) {
    for (const auto& path : candidates) {
        if (fs::exists(path)) {
            return path;
        }
    }
    return candidates.empty() ? fs::path() : candidates.front();
}

void requireFile(const fs::path& path, const std::string& label) {
    if (!fs::exists(path) || !fs::is_regular_file(path)) {
        throw std::runtime_error(
            label + " not found: " + fs::absolute(path).string());
    }
}

void requireDirectory(const fs::path& path, const std::string& label) {
    if (!fs::exists(path) || !fs::is_directory(path)) {
        throw std::runtime_error(
            label + " not found: " + fs::absolute(path).string());
    }
}

AppContext discoverContext() {
    AppContext ctx;

    fs::path cwd = fs::current_path();
    ctx.projectRoot = cwd;
    if (!fs::exists(ctx.projectRoot / "resources") &&
        fs::exists(cwd.parent_path() / "resources")) {
        ctx.projectRoot = cwd.parent_path();
    }

    ctx.resourcesDir = ctx.projectRoot / "resources";
    ctx.resultsRepoDir = ctx.projectRoot / "results_repo";
    ctx.tmpEvalDir = ctx.resultsRepoDir / "tmp_eval";

    fs::create_directories(ctx.resourcesDir);
    fs::create_directories(ctx.resultsRepoDir);
    fs::create_directories(ctx.tmpEvalDir);

    ctx.pythonScriptPath = ctx.projectRoot / "scripts" / "single_power_run.py";
    ctx.genlibPath = firstExistingPath({
        ctx.resourcesDir / "nangate_45nm.genlib",
        ctx.resourcesDir / "mcnc.genlib"});
    ctx.libertyPath = firstExistingPath({
        ctx.resourcesDir / "NangateOpenCellLibrary_typical.lib",
        ctx.resourcesDir / "NanGate_15nm_OCL_typical_conditional_nldm.lib"});
    ctx.standardCellCsvPath = ctx.resourcesDir / "standard_cells.csv";
    ctx.workerCount = std::max(1u, std::thread::hardware_concurrency());

    return ctx;
}

fs::path defaultGenerateOutputDir(const AppContext& ctx,
                                  const GenerateOptions& opts) {
    return ctx.resultsRepoDir /
           ("generate_" + opts.mode + "_k" + std::to_string(opts.k) + "_" +
            getCurrentTimestamp());
}

fs::path findLatestGeneratedLibraryDir(const fs::path& resultsRepoDir) {
    fs::path bestPath;
    bool found = false;
    fs::file_time_type bestTime{};

    if (!fs::exists(resultsRepoDir)) {
        return {};
    }

    for (const auto& entry : fs::directory_iterator(resultsRepoDir)) {
        if (!entry.is_directory()) {
            continue;
        }

        const fs::path candidate = entry.path();
        if (candidate.filename() == "local_abc_lib") {
            continue;
        }
        if (!fs::exists(candidate / "final_results.csv")) {
            continue;
        }
        if (!fs::exists(candidate / "detailed_infos")) {
            continue;
        }

        const auto stamp = fs::last_write_time(candidate / "final_results.csv");
        if (!found || stamp > bestTime) {
            bestTime = stamp;
            bestPath = candidate;
            found = true;
        }
    }

    return bestPath;
}

void printGenerateHelp(const AppContext& ctx) {
    std::cout
        << "Usage: fes_app generate [options]\n\n"
        << "Build a sub-circuit library with SynthesisFlow.\n\n"
        << "Options:\n"
        << "  --k <int>           LUT input size (default: 4, max compiled: "
        << kLutMaxInputs << ")\n"
        << "  --num <int>         Number of functions to include (default: 222)\n"
        << "  --config <file>     JSON config file. CLI flags override config values\n"
        << "  --mode <mode>       exhaustive | benchmark (default: exhaustive)\n"
        << "  --path <dir>        Benchmark directory for benchmark mode\n"
        << "                      (default: " << ctx.defaultBenchmarkDir.string() << ")\n"
        << "  --out <dir>         Output directory. Relative paths are rooted under\n"
        << "                      " << ctx.resultsRepoDir.string() << "\n"
        << "  --activity-mode <mode>\n"
        << "                      uniform | cartesian | explicit (default: uniform)\n"
        << "  --activity-levels <csv>\n"
        << "                      Comma-separated levels for uniform/cartesian modes\n"
        << "  --activity-explicit <patterns>\n"
        << "                      Semicolon-separated explicit patterns, e.g.\n"
        << "                      \"0.1,0.2,0.3,0.4;0.4,0.3,0.2,0.1\"\n"
        << "  --resume            Resume partial run; completed cases are skipped\n"
        << "  --skip-completed    Skip cases already marked success in run_manifest.csv\n"
        << "  --rerun <status>    failed | timeout\n"
        << "  --sat-timeout-ms <int>, --opt-timeout-ms <int>\n"
        << "                      Solver timeout settings for generated cases\n"
        << "  --case-timeout-ms <int>\n"
        << "                      Mark a case timeout if runtime exceeds this threshold\n"
        << "  --verify            Enable CEC on synthesized sub-circuits\n"
        << "  -h, --help          Show this message\n\n"
        << "Notes:\n"
        << "  Standard cells are loaded from "
        << ctx.standardCellCsvPath.string()
        << " / " << ctx.libertyPath.string() << ".\n"
        << "  Worker count defaults to hardware concurrency: "
        << ctx.workerCount << ".\n";
}

void printOptimizeHelp(const AppContext& ctx) {
    std::cout
        << "Usage: fes_app optimize|evaluate|benchmark [options]\n\n"
        << "Run the existing PONO tournament optimization flow on a benchmark set.\n\n"
        << "Options:\n"
        << "  --config <file>     JSON config file. CLI flags override config values\n"
        << "  --path <dir>        Benchmark directory to optimize\n"
        << "                      (default: " << ctx.defaultBenchmarkDir.string() << ")\n"
        << "  --lib <dir>         Generated library directory to use\n"
        << "                      (default: newest results_repo/* with detailed_infos)\n"
        << "  --mapped-four-way   Run mapped-origin four-way validation\n"
        << "  --abc-local-lib <dir>\n"
        << "                      ABC local-library directory for mapped-four-way mode\n"
        << "  --resume, --skip-completed, --rerun <failed|timeout>\n"
        << "                      Apply evaluator-level per-benchmark resume/rerun\n"
        << "  --case-timeout-ms <int>\n"
        << "                      Mark an evaluation case timeout if runtime exceeds this threshold\n"
        << "  --verify            Enable rewrite-time CEC\n"
        << "  -h, --help          Show this message\n";
}

void printGeneralHelp(const AppContext& ctx) {
    std::cout
        << "Usage:\n"
        << "  fes_app generate [options]\n"
        << "  fes_app optimize|evaluate|benchmark [options]\n"
        << "  fes_app help [generate|optimize|evaluate|benchmark]\n\n"
        << "Commands:\n"
        << "  generate   Build a sub-circuit library using exhaustive or benchmark mode\n"
        << "  optimize   Run the tournament-based PONO optimization flow on benchmarks\n"
        << "  evaluate   Alias for optimize with config-friendly naming\n"
        << "  benchmark  Alias for optimize/physical validation workflows\n\n"
        << "Compatibility:\n"
        << "  cat design.blif | fes_app -c p1 p2 ...\n"
        << "  keeps the legacy stdin API optimization path.\n\n";
    printGenerateHelp(ctx);
    std::cout << "\n";
    printOptimizeHelp(ctx);
}

void applyGenerateToolOverrides(AppContext& ctx, const GenerateOptions& opts) {
    if (!opts.abcPath.empty()) ctx.abcPath = opts.abcPath;
    if (!opts.genlibPath.empty()) ctx.genlibPath = opts.genlibPath;
    if (!opts.libertyPath.empty()) ctx.libertyPath = opts.libertyPath;
    if (!opts.pythonScriptPath.empty()) ctx.pythonScriptPath = opts.pythonScriptPath;
    if (!opts.standardCellCsvPath.empty()) {
        ctx.standardCellCsvPath = opts.standardCellCsvPath;
    }
    if (opts.workerCount > 0) ctx.workerCount = opts.workerCount;
}

void applyEvaluationToolOverrides(AppContext& ctx,
                                  const EvaluationOptions& opts) {
    if (!opts.abcPath.empty()) ctx.abcPath = opts.abcPath;
    if (!opts.pythonScriptPath.empty()) ctx.pythonScriptPath = opts.pythonScriptPath;
    if (opts.workerCount > 0) ctx.workerCount = opts.workerCount;
}

int runGenerateCommand(const AppContext& ctx, GenerateOptions opts) {
    AppContext runCtx = ctx;
    applyGenerateToolOverrides(runCtx, opts);

    requireFile(runCtx.pythonScriptPath, "Python evaluation script");
    requireFile(runCtx.genlibPath, "ABC genlib");
    requireFile(runCtx.libertyPath, "Standard-cell Liberty file");

    opts.outputDir = resolveGenerateOutputDir(
        runCtx.projectRoot,
        runCtx.resultsRepoDir,
        opts.outputDir,
        defaultGenerateOutputDir(runCtx, opts));
    fs::create_directories(opts.outputDir);

    const auto standardCells = CellLibraryLoader::loadOrGenerate(
        runCtx.standardCellCsvPath.string(),
        runCtx.libertyPath.string(),
        opts.k);
    if (standardCells.empty()) {
        throw std::runtime_error(
            "No standard cells available for K=" + std::to_string(opts.k));
    }

    std::cout << "[Generate] Resources: " << fs::absolute(runCtx.resourcesDir) << "\n";
    std::cout << "[Generate] tmp_eval: " << fs::absolute(runCtx.tmpEvalDir) << "\n";
    std::cout << "[Generate] Worker threads: " << runCtx.workerCount
              << " (used internally by SynthesisFlow)\n";
    std::cout << "[Generate] Loaded standard cells: " << standardCells.size()
              << " for K<=" << opts.k << "\n";

    LibraryGenerationConfig cfg;
    cfg.lutInputs = opts.k;
    cfg.numFunctionsToInclude = opts.numFunctions;
    cfg.outputDir = opts.outputDir.string();
    cfg.outputCsv = (opts.outputDir / "final_results.csv").string();
    cfg.enablePhysicalEval = false;
    cfg.activityPatternSpec = opts.activityPatternSpec;
    cfg.activityPatternMode = opts.activityModeName;
    cfg.resumePolicy = opts.resumePolicy;
    cfg.satTimeoutMs = opts.satTimeoutMs;
    cfg.optTimeoutMs = opts.optTimeoutMs;
    cfg.caseTimeoutMs = opts.caseTimeoutMs;
    cfg.workerCount = opts.workerCount;

    if (opts.mode == "exhaustive") {
        cfg.mode = LibraryGenerationMode::kExhaustiveNpn;
    } else {
        cfg.mode = LibraryGenerationMode::kBenchmarkDriven;
            cfg.benchmarkDir = opts.benchmarkDir.empty()
                               ? runCtx.defaultBenchmarkDir.string()
                               : opts.benchmarkDir.string();
        cfg.inputCsv = (opts.outputDir / "top_ranked_funcs.csv").string();
        requireDirectory(cfg.benchmarkDir, "Benchmark directory");
    }

    SynthesisFlow flow(
        createLibrary(),
        runCtx.abcPath,
        runCtx.genlibPath.string(),
        runCtx.pythonScriptPath.string());
    flow.enableVerification(opts.verify);

    if (!flow.generateLibrary(cfg)) {
        throw std::runtime_error("Library generation failed.");
    }

    std::cout << "[Generate] Completed.\n";
    std::cout << "[Generate] Output directory: "
              << fs::absolute(opts.outputDir) << "\n";
    std::cout << "[Generate] Summary CSV: "
              << fs::absolute(opts.outputDir / "final_results.csv") << "\n";
    return 0;
}

fs::path resolveResultsRepoPath(const AppContext& ctx, const fs::path& path) {
    if (path.empty() || path.is_absolute()) {
        return path;
    }
    return resolveGenerateOutputDir(
        ctx.projectRoot, ctx.resultsRepoDir, path, ctx.resultsRepoDir);
}

void writeEvaluationSummary(const fs::path& libraryDir,
                            const fs::path& benchmarkDir,
                            bool mappedFourWay) {
    std::ofstream out(libraryDir / "evaluation_summary.json");
    if (!out.is_open()) {
        return;
    }
    out << "{\n"
        << "  \"command\": \"evaluate\",\n"
        << "  \"status\": \"completed\",\n"
        << "  \"mode\": \"" << (mappedFourWay ? "mapped_four_way" : "standard") << "\",\n"
        << "  \"benchmark_dir\": \"" << benchmarkDir.string() << "\",\n"
        << "  \"library_dir\": \"" << libraryDir.string() << "\",\n"
        << "  \"validation_csv\": \""
        << (libraryDir / (mappedFourWay ? "ppa_mapped_four_way_validation.csv"
                                        : "ppa_complete_validation.csv")).string()
        << "\"\n"
        << "}\n";
}

int runOptimizeCommand(const AppContext& ctx, EvaluationOptions opts) {
    AppContext runCtx = ctx;
    applyEvaluationToolOverrides(runCtx, opts);

    requireFile(runCtx.pythonScriptPath, "Python evaluation script");

    if (opts.benchmarkDir.empty()) {
        opts.benchmarkDir = runCtx.defaultBenchmarkDir;
    }
    requireDirectory(opts.benchmarkDir, "Benchmark directory");

    if (opts.libraryDir.empty()) {
        opts.libraryDir = findLatestGeneratedLibraryDir(runCtx.resultsRepoDir);
    } else {
        opts.libraryDir = resolveResultsRepoPath(runCtx, opts.libraryDir);
    }
    if (opts.libraryDir.empty()) {
        throw std::runtime_error(
            "No generated library found. Run 'generate' first or pass --lib.");
    }

    requireDirectory(opts.libraryDir, "Generated library directory");
    requireFile(opts.libraryDir / "final_results.csv", "Library index");
    requireDirectory(opts.libraryDir / "detailed_infos", "Library detailed_infos");

    std::cout << "[Optimize] Resources: " << fs::absolute(runCtx.resourcesDir) << "\n";
    std::cout << "[Optimize] tmp_eval: "
              << fs::absolute(opts.libraryDir / "tmp_eval") << "\n";
    std::cout << "[Optimize] Tournament mode: aggressive vs conservative PONO rewrite\n";
    std::cout << "[Optimize] Library: " << fs::absolute(opts.libraryDir) << "\n";
    std::cout << "[Optimize] Benchmarks: " << fs::absolute(opts.benchmarkDir) << "\n";

    if (opts.mappedFourWay) {
        if (opts.abcLocalLibraryDir.empty()) {
            throw std::runtime_error(
                "--mapped-four-way requires --abc-local-lib or config evaluate.abc_local_library_dir.");
        }
        opts.abcLocalLibraryDir =
            resolveResultsRepoPath(runCtx, opts.abcLocalLibraryDir);
        InnovusBatchEvaluator mappedEvaluator(
            opts.libraryDir.string(),
            opts.abcLocalLibraryDir.string(),
            runCtx.pythonScriptPath.string(),
            runCtx.abcPath);
        mappedEvaluator.enableVerification(opts.verify);
        mappedEvaluator.setResumePolicy(opts.resumePolicy);
        mappedEvaluator.setCaseTimeoutMs(opts.caseTimeoutMs);
        mappedEvaluator.runBatchVerificationMappedFourWay(opts.benchmarkDir.string());
    } else {
        InnovusBatchEvaluator evaluator(
            opts.libraryDir.string(),
            runCtx.pythonScriptPath.string(),
            runCtx.abcPath);
        evaluator.enableVerification(opts.verify);
        evaluator.setResumePolicy(opts.resumePolicy);
        evaluator.setCaseTimeoutMs(opts.caseTimeoutMs);
        evaluator.runBatchVerification(opts.benchmarkDir.string());
    }

    std::cout << "[Optimize] Completed.\n";
    std::cout << "[Optimize] Validation CSV: "
              << fs::absolute(
                     opts.libraryDir /
                     (opts.mappedFourWay ? "ppa_mapped_four_way_validation.csv"
                                         : "ppa_complete_validation.csv"))
              << "\n";
    return 0;
}

void printApiFailureJson(const std::string& message) {
    SingleOptResult result;
    result.success = false;
    result.errorMessage = message;
    std::cout << result.toJson() << std::endl;
}

int runLegacyApiMode(const AppContext& ctx, int argc, char** argv) {
    fs::path libraryDir = findLatestGeneratedLibraryDir(ctx.resultsRepoDir);
    if (libraryDir.empty()) {
        printApiFailureJson(
            "No generated library found for API optimization.");
        return 1;
    }

    std::vector<double> probs;
    for (int i = 2; i < argc; ++i) {
        probs.push_back(std::stod(argv[i]));
    }

    std::string blifContent;
    std::string line;
    while (std::getline(std::cin, line)) {
        blifContent += line + "\n";
    }

    InnovusBatchEvaluator evaluator(
        libraryDir.string(),
        ctx.pythonScriptPath.string(),
        ctx.abcPath);
    SingleOptResult result =
        evaluator.optimizeSingleBlifFromContent(blifContent, probs);
    std::cout << result.toJson() << std::endl;
    return result.success ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    const bool legacyApiMode =
        (argc >= 2 && std::string(argv[1]) == "-c");

    try {
        const AppContext ctx = discoverContext();

        if (legacyApiMode) {
            return runLegacyApiMode(ctx, argc, argv);
        }

        if (argc < 2) {
            printGeneralHelp(ctx);
            return 0;
        }

        const std::string command = argv[1];
        const std::vector<std::string> args(argv + 2, argv + argc);

        if (command == "help" || command == "-h" || command == "--help") {
            if (args.empty()) {
                printGeneralHelp(ctx);
            } else if (args[0] == "generate") {
                printGenerateHelp(ctx);
            } else if (args[0] == "optimize" ||
                       args[0] == "evaluate" ||
                       args[0] == "benchmark") {
                printOptimizeHelp(ctx);
            } else {
                throw std::runtime_error("Unknown help topic: " + args[0]);
            }
            return 0;
        }

        if (command == "generate") {
            const GenerateOptions opts =
                parseGenerateOptions(args, kLutMaxInputs);
            if (opts.help) {
                printGenerateHelp(ctx);
                return 0;
            }
            return runGenerateCommand(ctx, opts);
        }

        if (command == "optimize" ||
            command == "evaluate" ||
            command == "benchmark") {
            const EvaluationOptions opts =
                parseEvaluationOptions(args, kLutMaxInputs);
            if (opts.help) {
                printOptimizeHelp(ctx);
                return 0;
            }
            return runOptimizeCommand(ctx, opts);
        }

        throw std::runtime_error("Unknown command: " + command);
    } catch (const std::exception& e) {
        if (legacyApiMode) {
            printApiFailureJson(std::string("Fatal C++ Exception: ") + e.what());
        } else {
            std::cerr << "[Fatal] " << e.what() << std::endl;
        }
        return 1;
    }
}
