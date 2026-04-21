#include "fes/core/GateType.h"
#include "fes/core/Types.h"
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

struct GenerateOptions {
    int k = 4;
    int numFunctions = 222;
    std::string mode = "exhaustive";
    fs::path benchmarkDir;
    fs::path outputDir;
    bool verify = false;
    bool help = false;
};

struct OptimizeOptions {
    fs::path benchmarkDir;
    fs::path libraryDir;
    bool verify = false;
    bool help = false;
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
    ctx.tmpEvalDir = cwd / "tmp_eval";

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
        << "  --mode <mode>       exhaustive | benchmark (default: exhaustive)\n"
        << "  --path <dir>        Benchmark directory for benchmark mode\n"
        << "                      (default: " << ctx.defaultBenchmarkDir.string() << ")\n"
        << "  --out <dir>         Output directory under results_repo\n"
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
        << "Usage: fes_app optimize [options]\n\n"
        << "Run the existing PONO tournament optimization flow on a benchmark set.\n\n"
        << "Options:\n"
        << "  --path <dir>        Benchmark directory to optimize\n"
        << "                      (default: " << ctx.defaultBenchmarkDir.string() << ")\n"
        << "  --lib <dir>         Generated library directory to use\n"
        << "                      (default: newest results_repo/* with detailed_infos)\n"
        << "  --verify            Enable rewrite-time CEC\n"
        << "  -h, --help          Show this message\n";
}

void printGeneralHelp(const AppContext& ctx) {
    std::cout
        << "Usage:\n"
        << "  fes_app generate [options]\n"
        << "  fes_app optimize [options]\n"
        << "  fes_app help [generate|optimize]\n\n"
        << "Commands:\n"
        << "  generate   Build a sub-circuit library using exhaustive or benchmark mode\n"
        << "  optimize   Run the tournament-based PONO optimization flow on benchmarks\n\n"
        << "Compatibility:\n"
        << "  cat design.blif | fes_app -c p1 p2 ...\n"
        << "  keeps the legacy stdin API optimization path.\n\n";
    printGenerateHelp(ctx);
    std::cout << "\n";
    printOptimizeHelp(ctx);
}

GenerateOptions parseGenerateOptions(const std::vector<std::string>& args) {
    GenerateOptions opts;

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        std::string value;

        if (arg == "-h" || arg == "--help") {
            opts.help = true;
            continue;
        }
        if (arg == "--verify") {
            opts.verify = true;
            continue;
        }
        if (consumeOption(args, i, "--k", &value)) {
            opts.k = parseIntOption(value, "--k");
            continue;
        }
        if (consumeOption(args, i, "--num", &value)) {
            opts.numFunctions = parseIntOption(value, "--num");
            continue;
        }
        if (consumeOption(args, i, "--mode", &value)) {
            opts.mode = toLowerCopy(value);
            continue;
        }
        if (consumeOption(args, i, "--path", &value)) {
            opts.benchmarkDir = value;
            continue;
        }
        if (consumeOption(args, i, "--out", &value)) {
            opts.outputDir = value;
            continue;
        }

        throw std::runtime_error("Unknown generate option: " + arg);
    }

    if (opts.k <= 0 || opts.k > kLutMaxInputs) {
        throw std::runtime_error(
            "--k must be in the range [1, " + std::to_string(kLutMaxInputs) + "]");
    }
    if (opts.numFunctions < 0) {
        throw std::runtime_error("--num must be non-negative.");
    }
    if (opts.mode != "exhaustive" && opts.mode != "benchmark") {
        throw std::runtime_error("--mode must be 'exhaustive' or 'benchmark'.");
    }

    return opts;
}

OptimizeOptions parseOptimizeOptions(const std::vector<std::string>& args) {
    OptimizeOptions opts;

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        std::string value;

        if (arg == "-h" || arg == "--help") {
            opts.help = true;
            continue;
        }
        if (arg == "--verify") {
            opts.verify = true;
            continue;
        }
        if (consumeOption(args, i, "--path", &value)) {
            opts.benchmarkDir = value;
            continue;
        }
        if (consumeOption(args, i, "--lib", &value)) {
            opts.libraryDir = value;
            continue;
        }

        throw std::runtime_error("Unknown optimize option: " + arg);
    }

    return opts;
}

int runGenerateCommand(const AppContext& ctx, GenerateOptions opts) {
    requireFile(ctx.pythonScriptPath, "Python evaluation script");
    requireFile(ctx.genlibPath, "ABC genlib");
    requireFile(ctx.libertyPath, "Standard-cell Liberty file");

    if (opts.outputDir.empty()) {
        opts.outputDir = defaultGenerateOutputDir(ctx, opts);
    }
    fs::create_directories(opts.outputDir);

    const auto standardCells = CellLibraryLoader::loadOrGenerate(
        ctx.standardCellCsvPath.string(),
        ctx.libertyPath.string(),
        opts.k);
    if (standardCells.empty()) {
        throw std::runtime_error(
            "No standard cells available for K=" + std::to_string(opts.k));
    }

    std::cout << "[Generate] Resources: " << fs::absolute(ctx.resourcesDir) << "\n";
    std::cout << "[Generate] tmp_eval: " << fs::absolute(ctx.tmpEvalDir) << "\n";
    std::cout << "[Generate] Worker threads: " << ctx.workerCount
              << " (used internally by SynthesisFlow)\n";
    std::cout << "[Generate] Loaded standard cells: " << standardCells.size()
              << " for K<=" << opts.k << "\n";

    LibraryGenerationConfig cfg;
    cfg.lutInputs = opts.k;
    cfg.numFunctionsToInclude = opts.numFunctions;
    cfg.outputDir = opts.outputDir.string();
    cfg.outputCsv = (opts.outputDir / "final_results.csv").string();
    cfg.enablePhysicalEval = false;

    if (opts.mode == "exhaustive") {
        cfg.mode = LibraryGenerationMode::kExhaustiveNpn;
    } else {
        cfg.mode = LibraryGenerationMode::kBenchmarkDriven;
        cfg.benchmarkDir = opts.benchmarkDir.empty()
                               ? ctx.defaultBenchmarkDir.string()
                               : opts.benchmarkDir.string();
        cfg.inputCsv = (opts.outputDir / "top_ranked_funcs.csv").string();
        requireDirectory(cfg.benchmarkDir, "Benchmark directory");
    }

    SynthesisFlow flow(
        createLibrary(),
        ctx.abcPath,
        ctx.genlibPath.string(),
        ctx.pythonScriptPath.string());
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

int runOptimizeCommand(const AppContext& ctx, OptimizeOptions opts) {
    requireFile(ctx.pythonScriptPath, "Python evaluation script");

    if (opts.benchmarkDir.empty()) {
        opts.benchmarkDir = ctx.defaultBenchmarkDir;
    }
    requireDirectory(opts.benchmarkDir, "Benchmark directory");

    if (opts.libraryDir.empty()) {
        opts.libraryDir = findLatestGeneratedLibraryDir(ctx.resultsRepoDir);
    }
    if (opts.libraryDir.empty()) {
        throw std::runtime_error(
            "No generated library found. Run 'generate' first or pass --lib.");
    }

    requireDirectory(opts.libraryDir, "Generated library directory");
    requireFile(opts.libraryDir / "final_results.csv", "Library index");
    requireDirectory(opts.libraryDir / "detailed_infos", "Library detailed_infos");

    std::cout << "[Optimize] Resources: " << fs::absolute(ctx.resourcesDir) << "\n";
    std::cout << "[Optimize] tmp_eval: " << fs::absolute(ctx.tmpEvalDir) << "\n";
    std::cout << "[Optimize] Tournament mode: aggressive vs conservative PONO rewrite\n";
    std::cout << "[Optimize] Library: " << fs::absolute(opts.libraryDir) << "\n";
    std::cout << "[Optimize] Benchmarks: " << fs::absolute(opts.benchmarkDir) << "\n";

    InnovusBatchEvaluator evaluator(
        opts.libraryDir.string(),
        ctx.pythonScriptPath.string(),
        ctx.abcPath);
    evaluator.enableVerification(opts.verify);
    evaluator.runBatchVerification(opts.benchmarkDir.string());

    std::cout << "[Optimize] Completed.\n";
    std::cout << "[Optimize] Validation CSV: "
              << fs::absolute(opts.libraryDir / "ppa_complete_validation.csv")
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
            } else if (args[0] == "optimize") {
                printOptimizeHelp(ctx);
            } else {
                throw std::runtime_error("Unknown help topic: " + args[0]);
            }
            return 0;
        }

        if (command == "generate") {
            const GenerateOptions opts = parseGenerateOptions(args);
            if (opts.help) {
                printGenerateHelp(ctx);
                return 0;
            }
            return runGenerateCommand(ctx, opts);
        }

        if (command == "optimize") {
            const OptimizeOptions opts = parseOptimizeOptions(args);
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
