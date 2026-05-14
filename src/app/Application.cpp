#include "fes/app/Application.h"

#include "fes/app/AppConfig.h"
#include "fes/app/GenerateCli.h"
#include "fes/core/GateType.h"
#include "fes/core/Types.h"
#include "fes/flow/SynthesisFlow.h"
#include "fes/utils/CellLibraryLoader.h"
#include "fes/utils/InnovusBatchEvaluator.h"
#include "fes/utils/SynthesisLibraryLoader.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

#ifndef FES_PROJECT_ROOT
#define FES_PROJECT_ROOT "."
#endif

namespace fes::app {
namespace {

constexpr const char* kDefaultBenchmarkPath = "/home/yhs_joker/datasets/benchmarks";

struct AppContext {
    fs::path projectRoot;
    fs::path resourcesDir;
    fs::path resultsRepoDir;

    fs::path pythonScriptPath;
    fs::path genlibPath;
    fs::path libertyPath;
    fs::path standardCellCsvPath;

    std::string abcPath;
    fs::path defaultBenchmarkDir = kDefaultBenchmarkPath;
    unsigned workerCount = 1;
};

std::string joinStandardCellNames(const std::vector<StandardCell>& cells) {
    std::ostringstream out;
    for (std::size_t i = 0; i < cells.size(); ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << cells[i].name;
    }
    return out.str();
}

std::string joinGateTypeNames(const std::vector<GateType>& gates) {
    std::ostringstream out;
    for (std::size_t i = 0; i < gates.size(); ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << gates[i].name;
    }
    return out.str();
}

int maxGateInputs(const std::vector<GateType>& gates) {
    int maxInputs = 0;
    for (const auto& gate : gates) {
        maxInputs = std::max(maxInputs, gate.numInputs);
    }
    return maxInputs;
}

std::string getCurrentTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto inTimeT = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&inTimeT), "%Y%m%d_%H%M%S");
    return ss.str();
}

fs::path firstExistingPath(const std::vector<fs::path>& candidates) {
    for (const auto& path : candidates) {
        if (fs::exists(path)) {
            return path;
        }
    }
    return candidates.empty() ? fs::path() : candidates.front();
}

void removeDirectoryIfEmpty(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec) || !fs::is_directory(path, ec)) {
        return;
    }
    if (fs::is_empty(path, ec)) {
        fs::remove(path, ec);
    }
}

fs::path compileTimeProjectRoot() {
    return fs::weakly_canonical(fs::path(FES_PROJECT_ROOT));
}

fs::path resolveProjectPath(const AppContext& ctx, const fs::path& path) {
    if (path.empty() || path.is_absolute()) {
        return path.lexically_normal();
    }
    return (ctx.projectRoot / path).lexically_normal();
}

bool isPathWithinOrEqual(const fs::path& base, const fs::path& candidate) {
    const fs::path normalizedBase = base.lexically_normal();
    const fs::path normalizedCandidate = candidate.lexically_normal();

    auto baseIt = normalizedBase.begin();
    auto candidateIt = normalizedCandidate.begin();
    for (; baseIt != normalizedBase.end(); ++baseIt, ++candidateIt) {
        if (candidateIt == normalizedCandidate.end() ||
            *baseIt != *candidateIt) {
            return false;
        }
    }
    return true;
}

fs::path resolveResultsRepoPath(const AppContext& ctx, const fs::path& path) {
    if (path.empty() || path.is_absolute()) {
        return path.lexically_normal();
    }

    const fs::path requested = path.lexically_normal();
    fs::path candidate;
    auto first = requested.begin();
    if (first != requested.end() &&
        *first == ctx.resultsRepoDir.filename()) {
        candidate = ctx.projectRoot / requested;
    } else {
        candidate = ctx.resultsRepoDir / requested;
    }
    candidate = candidate.lexically_normal();

    if (!isPathWithinOrEqual(ctx.resultsRepoDir, candidate)) {
        throw std::runtime_error(
            "Relative evaluation/library paths must stay under results_repo.");
    }
    return candidate;
}

std::string resolveProjectPathString(const AppContext& ctx,
                                     const std::string& path) {
    if (path.empty()) {
        return path;
    }
    return resolveProjectPath(ctx, fs::path(path)).string();
}

std::string getEnvString(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string(value);
}

void requireFile(const fs::path& path, const std::string& label) {
    if (!fs::exists(path) || !fs::is_regular_file(path)) {
        throw std::runtime_error(
            label + " not found: " + fs::absolute(path).string());
    }
}

void requireConfiguredFile(const fs::path& path,
                           const std::string& label,
                           const std::string& hint) {
    if (path.empty()) {
        throw std::runtime_error(label + " is not configured. " + hint);
    }
    requireFile(path, label);
}

void requireDirectory(const fs::path& path, const std::string& label) {
    if (!fs::exists(path) || !fs::is_directory(path)) {
        throw std::runtime_error(
            label + " not found: " + fs::absolute(path).string());
    }
}

struct PreparedLibraries {
    fs::path libraryDir;
    fs::path abcLocalLibraryDir;
};

int countDeclaredBlifInputs(const fs::path& blifPath) {
    std::ifstream input(blifPath);
    if (!input.is_open()) {
        return 0;
    }

    std::string line;
    bool inInputSection = false;
    int inputCount = 0;
    while (std::getline(input, line)) {
        const size_t first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos || line[first] == '#') {
            continue;
        }
        std::string trimmed = line.substr(first);
        if (trimmed.rfind(".inputs", 0) == 0) {
            inInputSection = true;
            trimmed = trimmed.substr(7);
        } else if (!inInputSection) {
            continue;
        } else if (!trimmed.empty() && trimmed[0] == '.') {
            break;
        }

        std::istringstream names(trimmed);
        std::string token;
        while (names >> token) {
            ++inputCount;
        }
    }
    return inputCount;
}

std::vector<double> deriveActsFromProbs(const std::vector<double>& probs) {
    std::vector<double> acts;
    acts.reserve(probs.size());
    for (double prob : probs) {
        acts.push_back(std::clamp(2.0 * prob * (1.0 - prob), 0.0, 1.0));
    }
    return acts;
}

void writeTextFile(const fs::path& path, const std::string& text) {
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path());
    }
    std::ofstream output(path);
    if (!output.is_open()) {
        throw std::runtime_error(
            "Failed to write file: " + fs::absolute(path).string());
    }
    output << text;
}

class ScopedStreamCapture {
public:
    ScopedStreamCapture()
        : originalCout_(std::cout.rdbuf(stdoutBuffer_.rdbuf())),
          originalCerr_(std::cerr.rdbuf(stderrBuffer_.rdbuf())) {}

    ScopedStreamCapture(const ScopedStreamCapture&) = delete;
    ScopedStreamCapture& operator=(const ScopedStreamCapture&) = delete;

    ~ScopedStreamCapture() { restore(); }

    void restore() {
        if (!restored_) {
            std::cout.rdbuf(originalCout_);
            std::cerr.rdbuf(originalCerr_);
            restored_ = true;
        }
    }

    std::string stdoutText() const { return stdoutBuffer_.str(); }
    std::string stderrText() const { return stderrBuffer_.str(); }

private:
    std::streambuf* originalCout_ = nullptr;
    std::streambuf* originalCerr_ = nullptr;
    std::ostringstream stdoutBuffer_;
    std::ostringstream stderrBuffer_;
    bool restored_ = false;
};

bool hasFlag(const std::vector<std::string>& args, const std::string& flag) {
    return std::find(args.begin(), args.end(), flag) != args.end();
}

AppContext discoverContext() {
    AppContext ctx;

    ctx.projectRoot = compileTimeProjectRoot();
    ctx.resourcesDir = ctx.projectRoot / "resources";
    ctx.resultsRepoDir = ctx.projectRoot / "results_repo";

    fs::create_directories(ctx.resourcesDir);
    fs::create_directories(ctx.resultsRepoDir);
    removeDirectoryIfEmpty(ctx.resultsRepoDir / "tmp_eval");
    removeDirectoryIfEmpty(ctx.projectRoot / "tmp_eval");

    ctx.pythonScriptPath = ctx.projectRoot / "scripts" / "single_power_run.py";
    ctx.genlibPath = firstExistingPath({
        ctx.resourcesDir / "nangate_45nm.genlib",
        ctx.resourcesDir / "mcnc.genlib"});
    ctx.libertyPath = firstExistingPath({
        ctx.resourcesDir / "NangateOpenCellLibrary_typical.lib",
        ctx.resourcesDir / "NanGate_15nm_OCL_typical_conditional_nldm.lib"});
    ctx.standardCellCsvPath = ctx.resourcesDir / "standard_cells.csv";
    ctx.workerCount = std::max(1u, std::thread::hardware_concurrency());
    ctx.abcPath = resolveProjectPathString(ctx, getEnvString("ABC_PATH"));

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
        if (candidate.filename().string().find("_abc_local") !=
            std::string::npos) {
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
        << "  ABC is configured with dependencies.abc_path in JSON or ABC_PATH.\n"
        << "  The exact-synthesis gate library is derived directly from\n"
        << "  dependencies.standard_cell_csv.\n"
        << "  Standard-cell metadata and synthesis primitives are both loaded from\n"
        << ctx.standardCellCsvPath.string()
        << " / " << ctx.libertyPath.string() << ".\n"
        << "  Worker count defaults to hardware concurrency: "
        << ctx.workerCount << ".\n";
}

void printOptimizeHelp(const AppContext& ctx) {
    std::cout
        << "Usage: fes_app optimize|evaluate|benchmark [options]\n\n"
        << "Run the unified four-method physical-validation flow on a benchmark set.\n\n"
        << "Options:\n"
        << "  --config <file>     JSON config file. CLI flags override config values\n"
        << "  --path <dir>        Benchmark directory to optimize\n"
        << "                      (default: " << ctx.defaultBenchmarkDir.string() << ")\n"
        << "  --lib <dir>         Generated library directory to use\n"
        << "                      (default: newest results_repo/* with detailed_infos)\n"
        << "  --abc-local-lib <dir>\n"
        << "                      ABC local-library directory for the merged four-method flow\n"
        << "  --out <dir>         Output directory for evaluation CSV/JSON artifacts\n"
        << "                      (default: write back into the selected library dir)\n"
        << "  --resume, --skip-completed, --rerun <failed|timeout>\n"
        << "                      Apply evaluator-level per-benchmark resume/rerun\n"
        << "  --case-timeout-ms <int>\n"
        << "                      Mark an evaluation case timeout if runtime exceeds this threshold\n"
        << "  --verify            Enable rewrite-time CEC\n"
        << "  -h, --help          Show this message\n\n"
        << "Notes:\n"
        << "  ABC is configured with dependencies.abc_path in JSON or ABC_PATH.\n";
}

void printSingleBlifHelp(const AppContext& ctx,
                         const std::string& commandName) {
    const bool optimizeMode = commandName == "optimize-blif";
    std::cout
        << "Usage: fes_app " << commandName << " [options]\n\n"
        << (optimizeMode
                ? "Run the unified four-method flow on one BLIF and export the winning PONO rewrite.\n\n"
                : "Evaluate one BLIF with Original / ABC / ABC Local / PONO under explicit input parameters.\n\n")
        << "Options:\n"
        << "  --config <file>     JSON config file. CLI flags override config values\n"
        << "  --blif <file>       Input BLIF file to analyze\n"
        << "  --lib <dir>         Generated library directory to use\n"
        << "                      (default: newest results_repo/* with detailed_infos)\n"
        << "  --abc-local-lib <dir>\n"
        << "                      ABC local-library directory; auto-built if missing\n"
        << "  --out <dir>         Output directory for artifacts\n"
        << "                      (default: results_repo/single_blif/...)\n"
        << "  --result-json <file>\n"
        << "                      Result JSON path (default: <out>/result.json)\n"
        << "  --input-probs <csv> Required comma-separated input probabilities\n"
        << "  --input-acts <csv>  Optional comma-separated input activities\n"
        << "                      (default: derive each activity as 2*p*(1-p))\n"
        << "  --json              Print only the final result JSON to stdout\n"
        << "                      and write captured logs to <out>/stdout.log and stderr.log\n"
        << "  --emit-blif-content Embed the optimized BLIF text in result JSON\n"
        << "                      (mainly for backend/API use)\n"
        << "  --verify            Enable rewrite-time CEC\n"
        << "  -h, --help          Show this message\n\n"
        << "Notes:\n"
        << "  Relative paths resolve from the project root: "
        << ctx.projectRoot.string() << "\n";
}

void printValidateConfigHelp() {
    std::cout
        << "Usage: fes_app validate-config --config <file>\n\n"
        << "Parse a JSON config, resolve its paths, and check required files without\n"
        << "running generation or evaluation.\n";
}

void printDoctorHelp() {
    std::cout
        << "Usage: fes_app doctor [--config <file>]\n\n"
        << "Without --config, check the default environment discovery state.\n"
        << "With --config, resolve that config and check the command-specific\n"
        << "dependencies and required input paths without executing the job.\n";
}

void printGeneralHelp(const AppContext& ctx) {
    std::cout
        << "Usage:\n"
        << "  fes_app generate [options]\n"
        << "  fes_app optimize|evaluate|benchmark [options]\n"
        << "  fes_app optimize-blif|evaluate-blif [options]\n"
        << "  fes_app doctor [--config <file>]\n"
        << "  fes_app validate-config --config <file>\n"
        << "  fes_app help [generate|optimize|evaluate|benchmark|optimize-blif|evaluate-blif|doctor|validate-config]\n\n"
        << "Commands:\n"
        << "  generate   Build a sub-circuit library using exhaustive or benchmark mode\n"
        << "  optimize   Run the unified four-method physical-validation flow\n"
        << "  evaluate   Alias for optimize with config-friendly naming\n"
        << "  benchmark  Alias for optimize/physical validation workflows\n\n"
        << "  optimize-blif  Optimize one BLIF and emit structured JSON/artifacts\n"
        << "  evaluate-blif  Evaluate one BLIF under explicit input parameters\n"
        << "  doctor         Check tool/resource availability, optionally against a config\n"
        << "  validate-config Parse and validate a JSON config without execution\n\n"
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

    ctx.abcPath = resolveProjectPathString(ctx, ctx.abcPath);
    ctx.genlibPath = resolveProjectPath(ctx, ctx.genlibPath);
    ctx.libertyPath = resolveProjectPath(ctx, ctx.libertyPath);
    ctx.pythonScriptPath = resolveProjectPath(ctx, ctx.pythonScriptPath);
    ctx.standardCellCsvPath = resolveProjectPath(ctx, ctx.standardCellCsvPath);
}

void applyEvaluationToolOverrides(AppContext& ctx,
                                  const EvaluationOptions& opts) {
    if (!opts.abcPath.empty()) ctx.abcPath = opts.abcPath;
    if (!opts.pythonScriptPath.empty()) ctx.pythonScriptPath = opts.pythonScriptPath;
    if (opts.workerCount > 0) ctx.workerCount = opts.workerCount;

    ctx.abcPath = resolveProjectPathString(ctx, ctx.abcPath);
    ctx.pythonScriptPath = resolveProjectPath(ctx, ctx.pythonScriptPath);
}

void applySingleBlifToolOverrides(AppContext& ctx,
                                  const SingleBlifOptions& opts) {
    if (!opts.abcPath.empty()) ctx.abcPath = opts.abcPath;
    if (!opts.pythonScriptPath.empty()) ctx.pythonScriptPath = opts.pythonScriptPath;
    ctx.abcPath = resolveProjectPathString(ctx, ctx.abcPath);
    ctx.pythonScriptPath = resolveProjectPath(ctx, ctx.pythonScriptPath);
}

fs::path defaultSingleBlifOutputDir(const AppContext& ctx,
                                    const SingleBlifOptions& opts,
                                    const std::string& commandName) {
    const std::string stem = opts.blifPath.empty()
                                 ? std::string("input")
                                 : opts.blifPath.stem().string();
    return ctx.resultsRepoDir / "single_blif" /
           (commandName + "_" + stem + "_" + getCurrentTimestamp());
}

PreparedLibraries prepareEvaluationLibraries(const AppContext& ctx,
                                             fs::path libraryDir,
                                             fs::path abcLocalLibraryDir,
                                             bool autoBuildAbcLocal) {
    if (libraryDir.empty()) {
        libraryDir = findLatestGeneratedLibraryDir(ctx.resultsRepoDir);
    } else {
        libraryDir = resolveResultsRepoPath(ctx, libraryDir);
    }
    if (libraryDir.empty()) {
        throw std::runtime_error(
            "No generated library found. Run 'generate' first or pass --lib.");
    }

    requireDirectory(libraryDir, "Generated library directory");
    requireFile(libraryDir / "final_results.csv", "Library index");
    requireDirectory(libraryDir / "detailed_infos", "Library detailed_infos");

    if (abcLocalLibraryDir.empty()) {
        abcLocalLibraryDir =
            libraryDir.parent_path() /
            (libraryDir.filename().string() + "_abc_local");
    } else {
        abcLocalLibraryDir = resolveResultsRepoPath(ctx, abcLocalLibraryDir);
    }

    const bool haveAbcLocalLib =
        fs::exists(abcLocalLibraryDir / "final_results.csv") &&
        fs::exists(abcLocalLibraryDir / "detailed_infos");
    if (!haveAbcLocalLib && autoBuildAbcLocal) {
        std::cout << "[Prepare] Building ABC local library: "
                  << fs::absolute(abcLocalLibraryDir) << "\n";
        SynthesisFlow abcLocalBuilder(
            {},
            ctx.abcPath,
            ctx.genlibPath.string(),
            ctx.pythonScriptPath.string());
        if (!abcLocalBuilder.buildABCLocalLibraryFromTopCsv(
                (libraryDir / "final_results.csv").string(),
                abcLocalLibraryDir.string())) {
            throw std::runtime_error("ABC local library generation failed.");
        }
    }

    if (autoBuildAbcLocal || haveAbcLocalLib) {
        requireDirectory(abcLocalLibraryDir, "ABC local library directory");
        requireFile(abcLocalLibraryDir / "final_results.csv",
                    "ABC local library index");
        requireDirectory(abcLocalLibraryDir / "detailed_infos",
                         "ABC local library detailed_infos");
    }

    return {libraryDir, abcLocalLibraryDir};
}

int runGenerateCommand(const AppContext& ctx, GenerateOptions opts) {
    AppContext runCtx = ctx;
    applyGenerateToolOverrides(runCtx, opts);
    const unsigned requestedWorkers = std::max(1u, runCtx.workerCount);
    const unsigned effectiveWorkers = computeEffectiveWorkerCount(
        requestedWorkers, opts.maxWorkerMemoryMb, opts.maxTotalMemoryMb);
    runCtx.workerCount = effectiveWorkers;

    opts.outputDir = resolveGenerateOutputDir(
        runCtx.projectRoot,
        runCtx.resultsRepoDir,
        opts.outputDir,
        defaultGenerateOutputDir(runCtx, opts));

    std::cout << "[Generate] Project root: " << fs::absolute(runCtx.projectRoot) << "\n";
    std::cout << "[Generate] Resources: " << fs::absolute(runCtx.resourcesDir) << "\n";
    std::cout << "[Generate] ABC: " << fs::absolute(fs::path(runCtx.abcPath)) << "\n";
    std::cout << "[Generate] tmp_eval: "
              << fs::absolute(opts.outputDir / "tmp_eval") << "\n";
    std::cout << "[Generate] Output: " << fs::absolute(opts.outputDir) << "\n";
    std::cout << "[Generate] Activity mode: " << opts.activityModeName << "\n";
    std::cout << "[Generate] Resume policy: "
              << resumePolicyToString(opts.resumePolicy) << "\n";
    std::cout << "[Generate] Timeouts(ms): sat=" << opts.satTimeoutMs
              << ", optimization=" << opts.optTimeoutMs
              << ", case=" << opts.caseTimeoutMs << "\n";
    std::cout << "[Generate] Worker threads: requested=" << requestedWorkers
              << ", effective=" << effectiveWorkers << "\n";
    std::cout << "[Generate] Memory budget(MB): max_worker="
              << opts.maxWorkerMemoryMb
              << ", max_total=" << opts.maxTotalMemoryMb
              << " (best-effort worker-count guard)\n";
    std::cout << std::flush;

    requireConfiguredFile(
        fs::path(runCtx.abcPath),
        "ABC executable",
        "Set dependencies.abc_path in the JSON config or export ABC_PATH.");
    requireFile(runCtx.pythonScriptPath, "Python evaluation script");
    requireFile(runCtx.genlibPath, "ABC genlib");
    requireFile(runCtx.libertyPath, "Standard-cell Liberty file");

    fs::create_directories(opts.outputDir);

    const auto standardCells = CellLibraryLoader::loadOrGenerate(
        runCtx.standardCellCsvPath.string(),
        runCtx.libertyPath.string(),
        opts.k,
        (runCtx.projectRoot / "scripts" / "parse_liberty.py").string());
    if (standardCells.empty()) {
        throw std::runtime_error(
            "No standard cells available for K=" + std::to_string(opts.k));
    }

    const auto synthesisLibrary =
        SynthesisLibraryLoader::deriveFromStandardCells(standardCells);
    std::cout << "[Generate] standard_cell_csv: "
              << fs::absolute(runCtx.standardCellCsvPath) << "\n";
    std::cout << "[Generate] Loaded standard cells: "
              << standardCells.size() << " ["
              << joinStandardCellNames(standardCells) << "]\n";
    std::cout << "[Generate] Derived synthesis gate library: "
              << synthesisLibrary.size() << " ["
              << joinGateTypeNames(synthesisLibrary) << "]\n";
    std::cout << "[Generate] Derived max synthesis fanin: "
              << maxGateInputs(synthesisLibrary) << "\n";
    std::cout << "[Generate] Encoder type choices per synthesized gate: "
              << synthesisLibrary.size()
              << ", max candidate gate inputs: "
              << maxGateInputs(synthesisLibrary) << "\n";
    std::cout << "[Generate] Note: exact-synthesis gates are derived directly "
                 "from standard_cell_csv.\n";

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
    cfg.workerCount = effectiveWorkers;

    if (opts.mode == "exhaustive") {
        cfg.mode = LibraryGenerationMode::kExhaustiveNpn;
    } else {
        cfg.mode = LibraryGenerationMode::kBenchmarkDriven;
        if (!opts.benchmarkDir.empty()) {
            opts.benchmarkDir = resolveProjectPath(runCtx, opts.benchmarkDir);
        }
        cfg.benchmarkDir = opts.benchmarkDir.empty()
                               ? runCtx.defaultBenchmarkDir.string()
                               : opts.benchmarkDir.string();
        cfg.inputCsv = (opts.outputDir / "top_ranked_funcs.csv").string();
        requireDirectory(cfg.benchmarkDir, "Benchmark directory");
    }

    SynthesisFlow flow(
        synthesisLibrary,
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

int runOptimizeCommand(const AppContext& ctx, EvaluationOptions opts) {
    AppContext runCtx = ctx;
    applyEvaluationToolOverrides(runCtx, opts);
    const unsigned requestedWorkers = std::max(1u, runCtx.workerCount);
    const unsigned effectiveWorkers = computeEffectiveWorkerCount(
        requestedWorkers, opts.maxWorkerMemoryMb, opts.maxTotalMemoryMb);
    runCtx.workerCount = effectiveWorkers;

    requireConfiguredFile(
        fs::path(runCtx.abcPath),
        "ABC executable",
        "Set dependencies.abc_path in the JSON config or export ABC_PATH.");
    requireFile(runCtx.pythonScriptPath, "Python evaluation script");

    if (opts.benchmarkDir.empty()) {
        opts.benchmarkDir = runCtx.defaultBenchmarkDir;
    } else {
        opts.benchmarkDir = resolveProjectPath(runCtx, opts.benchmarkDir);
    }
    requireDirectory(opts.benchmarkDir, "Benchmark directory");
    const PreparedLibraries prepared = prepareEvaluationLibraries(
        runCtx, opts.libraryDir, opts.abcLocalLibraryDir, true);
    opts.libraryDir = prepared.libraryDir;
    opts.abcLocalLibraryDir = prepared.abcLocalLibraryDir;
    opts.outputDir = opts.outputDir.empty()
                         ? opts.libraryDir
                         : resolveResultsRepoPath(runCtx, opts.outputDir);
    fs::create_directories(opts.outputDir);

    std::cout << "[Optimize] Resources: " << fs::absolute(runCtx.resourcesDir) << "\n";
    std::cout << "[Optimize] Project root: " << fs::absolute(runCtx.projectRoot) << "\n";
    std::cout << "[Optimize] ABC: " << fs::absolute(fs::path(runCtx.abcPath)) << "\n";
    std::cout << "[Optimize] tmp_eval: "
              << fs::absolute(opts.outputDir / "tmp_eval") << "\n";
    std::cout << "[Optimize] Resume policy: "
              << resumePolicyToString(opts.resumePolicy) << "\n";
    std::cout << "[Optimize] Case timeout(ms): " << opts.caseTimeoutMs << "\n";
    std::cout << "[Optimize] Worker threads: requested=" << requestedWorkers
              << ", effective=" << effectiveWorkers
              << " (diagnostic; evaluator case loop is not thread-parallel)\n";
    std::cout << "[Optimize] Memory budget(MB): max_worker="
              << opts.maxWorkerMemoryMb
              << ", max_total=" << opts.maxTotalMemoryMb
              << " (best-effort diagnostic for evaluation)\n";
    std::cout << "[Optimize] Tournament mode: aggressive vs conservative PONO rewrite\n";
    std::cout << "[Optimize] Library: " << fs::absolute(opts.libraryDir) << "\n";
    std::cout << "[Optimize] ABC local library: "
              << fs::absolute(opts.abcLocalLibraryDir) << "\n";
    std::cout << "[Optimize] Benchmarks: " << fs::absolute(opts.benchmarkDir) << "\n";
    std::cout << "[Optimize] Output: " << fs::absolute(opts.outputDir) << "\n";

    InnovusBatchEvaluator evaluator(
        opts.libraryDir.string(),
        opts.abcLocalLibraryDir.string(),
        runCtx.pythonScriptPath.string(),
        runCtx.abcPath);
    evaluator.enableVerification(opts.verify);
    evaluator.setResumePolicy(opts.resumePolicy);
    evaluator.setCaseTimeoutMs(opts.caseTimeoutMs);
    evaluator.setOutputRootDir(opts.outputDir);
    evaluator.runBatchVerification(opts.benchmarkDir.string());

    std::cout << "[Optimize] Completed.\n";
    std::cout << "[Optimize] Validation CSV: "
              << fs::absolute(opts.outputDir / "ppa_complete_validation.csv")
              << "\n";
    return 0;
}

int runSingleBlifCommand(const AppContext& ctx,
                         SingleBlifOptions opts,
                         const std::string& commandName) {
    const bool optimizeMode = commandName == "optimize-blif";
    AppContext runCtx = ctx;
    applySingleBlifToolOverrides(runCtx, opts);
    opts.outputDir = opts.outputDir.empty()
                         ? defaultSingleBlifOutputDir(runCtx, opts, commandName)
                         : resolveResultsRepoPath(runCtx, opts.outputDir);
    if (opts.resultJsonPath.empty()) {
        opts.resultJsonPath = opts.outputDir / "result.json";
    } else {
        opts.resultJsonPath = resolveProjectPath(runCtx, opts.resultJsonPath);
    }
    fs::create_directories(opts.outputDir);

    std::unique_ptr<ScopedStreamCapture> capture;
    if (opts.jsonStdout) {
        capture = std::make_unique<ScopedStreamCapture>();
    }

    SingleBlifResult result;
    result.command = commandName;
    result.outputDir = fs::absolute(opts.outputDir).string();
    result.resultJsonPath = fs::absolute(opts.resultJsonPath).string();
    if (opts.jsonStdout) {
        result.stdoutLogPath = fs::absolute(opts.outputDir / "stdout.log").string();
        result.stderrLogPath = fs::absolute(opts.outputDir / "stderr.log").string();
    }

    try {
        requireConfiguredFile(
            fs::path(runCtx.abcPath),
            "ABC executable",
            "Set dependencies.abc_path in the JSON config or export ABC_PATH.");
        requireFile(runCtx.pythonScriptPath, "Python evaluation script");
        if (opts.blifPath.empty()) {
            throw std::runtime_error("--blif is required for single-BLIF commands.");
        }
        requireFile(resolveProjectPath(runCtx, opts.blifPath), "Input BLIF");

        opts.blifPath = resolveProjectPath(runCtx, opts.blifPath);
        result.sourceBlifPath = fs::absolute(opts.blifPath).string();

        const PreparedLibraries prepared = prepareEvaluationLibraries(
            runCtx, opts.libraryDir, opts.abcLocalLibraryDir, true);
        opts.libraryDir = prepared.libraryDir;
        opts.abcLocalLibraryDir = prepared.abcLocalLibraryDir;

        if (opts.inputProbs.empty()) {
            throw std::runtime_error(
                "--input-probs is required for single-BLIF commands.");
        }
        if (opts.inputActs.empty()) {
            opts.inputActs = deriveActsFromProbs(opts.inputProbs);
        }

        const int inputCount = countDeclaredBlifInputs(opts.blifPath);
        if (inputCount <= 0) {
            throw std::runtime_error("Failed to determine BLIF input count.");
        }
        if (static_cast<int>(opts.inputProbs.size()) != inputCount) {
            throw std::runtime_error(
                "input-probs count does not match BLIF .inputs count (" +
                std::to_string(opts.inputProbs.size()) + " vs " +
                std::to_string(inputCount) + ").");
        }
        if (opts.inputActs.size() != opts.inputProbs.size()) {
            throw std::runtime_error(
                "input-acts count does not match input-probs count.");
        }

        std::cout << "[" << commandName << "] BLIF: "
                  << fs::absolute(opts.blifPath) << "\n";
        std::cout << "[" << commandName << "] Library: "
                  << fs::absolute(opts.libraryDir) << "\n";
        std::cout << "[" << commandName << "] ABC local library: "
                  << fs::absolute(opts.abcLocalLibraryDir) << "\n";
        std::cout << "[" << commandName << "] Output: "
                  << fs::absolute(opts.outputDir) << "\n";
        std::cout << "[" << commandName << "] Result JSON: "
                  << fs::absolute(opts.resultJsonPath) << "\n";

        const fs::path optimizedBlifPath =
            optimizeMode ? (opts.outputDir / "optimized.blif") : fs::path{};

        InnovusBatchEvaluator evaluator(
            opts.libraryDir.string(),
            opts.abcLocalLibraryDir.string(),
            runCtx.pythonScriptPath.string(),
            runCtx.abcPath);
        evaluator.enableVerification(opts.verify);
        evaluator.setOutputRootDir(opts.outputDir);
        result = evaluator.analyzeSingleBlif(
            opts.blifPath.string(),
            opts.inputProbs,
            opts.inputActs,
            optimizedBlifPath,
            opts.emitBlifContent);
        result.command = commandName;
        result.outputDir = fs::absolute(opts.outputDir).string();
        result.resultJsonPath = fs::absolute(opts.resultJsonPath).string();
        if (opts.jsonStdout) {
            result.stdoutLogPath =
                fs::absolute(opts.outputDir / "stdout.log").string();
            result.stderrLogPath =
                fs::absolute(opts.outputDir / "stderr.log").string();
        } else {
            result.stdoutLogPath.clear();
            result.stderrLogPath.clear();
        }
        if (result.sourceBlifPath.empty()) {
            result.sourceBlifPath = fs::absolute(opts.blifPath).string();
        }
    } catch (const std::exception& e) {
        result.success = false;
        result.errorMessage = e.what();
        if (result.sourceBlifPath.empty() && !opts.blifPath.empty()) {
            result.sourceBlifPath =
                fs::absolute(resolveProjectPath(runCtx, opts.blifPath)).string();
        }
    }

    std::string capturedStdout;
    std::string capturedStderr;
    if (capture) {
        capture->restore();
        capturedStdout = capture->stdoutText();
        capturedStderr = capture->stderrText();
        writeTextFile(result.stdoutLogPath, capturedStdout);
        writeTextFile(result.stderrLogPath, capturedStderr);
    }

    writeTextFile(opts.resultJsonPath, result.toJson() + "\n");

    if (opts.jsonStdout) {
        std::cout << result.toJson() << std::endl;
    } else {
        std::cout << "[" << commandName << "] Completed.\n";
        std::cout << "[" << commandName << "] Success: "
                  << (result.success ? "true" : "false") << "\n";
        if (!result.optimizedBlifPath.empty()) {
            std::cout << "[" << commandName << "] Optimized BLIF: "
                      << result.optimizedBlifPath << "\n";
        }
        if (!result.errorMessage.empty()) {
            std::cout << "[" << commandName << "] Note: "
                      << result.errorMessage << "\n";
        }
    }

    return result.success ? 0 : 1;
}

int runDoctorCommand(const AppContext& ctx) {
    bool ok = true;
    auto printCheck = [&](const std::string& label,
                          const fs::path& path,
                          bool required) {
        const bool exists = !path.empty() && fs::exists(path);
        std::cout << "[Doctor] " << label << ": "
                  << (exists ? "OK " : "MISSING ")
                  << fs::absolute(path).string() << "\n";
        if (required && !exists) {
            ok = false;
        }
    };
    std::cout << "[Doctor] Project root: " << fs::absolute(ctx.projectRoot) << "\n";
    printCheck("Python evaluation script", ctx.pythonScriptPath, true);
    printCheck("ABC genlib", ctx.genlibPath, true);
    printCheck("Liberty", ctx.libertyPath, true);
    printCheck("Standard-cell CSV", ctx.standardCellCsvPath, true);
    if (ctx.abcPath.empty()) {
        std::cout << "[Doctor] ABC executable: NOT CONFIGURED (set ABC_PATH or dependencies.abc_path)\n";
        ok = false;
    } else {
        printCheck("ABC executable", fs::path(ctx.abcPath), true);
    }
    return ok ? 0 : 1;
}

int runDoctorCommand(const AppContext& ctx, const std::vector<std::string>& args) {
    fs::path configPath;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--config") {
            if (i + 1 >= args.size()) {
                throw std::runtime_error("Missing value for option --config");
            }
            configPath = args[i + 1];
            break;
        }
        const std::string prefix = "--config=";
        if (args[i].rfind(prefix, 0) == 0) {
            configPath = args[i].substr(prefix.size());
            break;
        }
        if (args[i] != "-h" && args[i] != "--help") {
            throw std::runtime_error("Unknown doctor option: " + args[i]);
        }
    }
    if (configPath.empty()) {
        return runDoctorCommand(ctx);
    }

    const AppConfig config = loadAppConfig(configPath, kLutMaxInputs);
    const std::string command = config.command;
    if (command.empty()) {
        throw std::runtime_error("Config must set the top-level command field.");
    }

    bool ok = true;
    auto printStatus = [&](const std::string& label,
                           const std::string& status,
                           const fs::path& path,
                           const std::string& note = std::string()) {
        std::cout << "[Doctor] " << label << ": " << status;
        if (!path.empty()) {
            std::cout << " " << fs::absolute(path).string();
        }
        if (!note.empty()) {
            std::cout << " (" << note << ")";
        }
        std::cout << "\n";
    };
    auto checkPath = [&](const std::string& label,
                         const fs::path& path,
                         bool expectDirectory,
                         bool required,
                         const std::string& note = std::string()) {
        const bool exists = !path.empty() && fs::exists(path) &&
                            (!expectDirectory || fs::is_directory(path));
        printStatus(label, exists ? "OK" : "MISSING", path, note);
        if (required && !exists) {
            ok = false;
        }
    };

    std::cout << "[Doctor] Project root: " << fs::absolute(ctx.projectRoot) << "\n";
    std::cout << "[Doctor] Config: " << fs::absolute(configPath) << "\n";
    std::cout << "[Doctor] Command: " << command << "\n";

    if (command == "generate") {
        GenerateOptions opts = parseGenerateOptions({"--config", configPath.string()},
                                                    kLutMaxInputs);
        AppContext runCtx = ctx;
        applyGenerateToolOverrides(runCtx, opts);
        opts.outputDir = resolveGenerateOutputDir(
            runCtx.projectRoot,
            runCtx.resultsRepoDir,
            opts.outputDir,
            defaultGenerateOutputDir(runCtx, opts));
        checkPath("ABC executable", fs::path(runCtx.abcPath), false, true);
        checkPath("Python evaluation script", runCtx.pythonScriptPath, false, true);
        checkPath("ABC genlib", runCtx.genlibPath, false, true);
        checkPath("Liberty", runCtx.libertyPath, false, true);
        checkPath("Standard-cell CSV", runCtx.standardCellCsvPath, false, true);
        if (opts.mode == "benchmark") {
            const fs::path benchmarkDir = opts.benchmarkDir.empty()
                                              ? runCtx.defaultBenchmarkDir
                                              : resolveProjectPath(runCtx, opts.benchmarkDir);
            checkPath("Benchmark directory", benchmarkDir, true, true);
        }
        printStatus("Planned output directory", "INFO", opts.outputDir,
                    "created on execution if missing");
        return ok ? 0 : 1;
    }

    if (command == "evaluate" || command == "benchmark" || command == "optimize") {
        EvaluationOptions opts =
            parseEvaluationOptions({"--config", configPath.string()}, kLutMaxInputs);
        AppContext runCtx = ctx;
        applyEvaluationToolOverrides(runCtx, opts);
        const fs::path benchmarkDir = opts.benchmarkDir.empty()
                                          ? runCtx.defaultBenchmarkDir
                                          : resolveProjectPath(runCtx, opts.benchmarkDir);
        fs::path libraryDir = opts.libraryDir.empty()
                                  ? findLatestGeneratedLibraryDir(runCtx.resultsRepoDir)
                                  : resolveResultsRepoPath(runCtx, opts.libraryDir);
        fs::path abcLocalLibraryDir = opts.abcLocalLibraryDir.empty()
                                          ? (libraryDir.empty()
                                                 ? fs::path{}
                                                 : libraryDir.parent_path() /
                                                       (libraryDir.filename().string() + "_abc_local"))
                                          : resolveResultsRepoPath(runCtx, opts.abcLocalLibraryDir);
        fs::path outputDir = opts.outputDir.empty()
                                 ? libraryDir
                                 : resolveResultsRepoPath(runCtx, opts.outputDir);

        checkPath("ABC executable", fs::path(runCtx.abcPath), false, true);
        checkPath("Python evaluation script", runCtx.pythonScriptPath, false, true);
        checkPath("Benchmark directory", benchmarkDir, true, true);
        checkPath("Library directory", libraryDir, true, true);
        checkPath("Library index", libraryDir / "final_results.csv", false, true);
        checkPath("Library detailed_infos", libraryDir / "detailed_infos", true, true);
        if (opts.abcLocalLibraryDir.empty()) {
            const bool exists = !abcLocalLibraryDir.empty() &&
                                fs::exists(abcLocalLibraryDir / "final_results.csv") &&
                                fs::exists(abcLocalLibraryDir / "detailed_infos");
            printStatus("ABC local library", exists ? "OK" : "AUTO",
                        abcLocalLibraryDir,
                        exists ? "prebuilt" : "will be auto-built during evaluation");
        } else {
            checkPath("ABC local library directory", abcLocalLibraryDir, true, true);
            checkPath("ABC local library index",
                      abcLocalLibraryDir / "final_results.csv",
                      false,
                      true);
            checkPath("ABC local library detailed_infos",
                      abcLocalLibraryDir / "detailed_infos",
                      true,
                      true);
        }
        printStatus("Planned output directory", "INFO", outputDir,
                    "created on execution if missing");
        return ok ? 0 : 1;
    }

    if (command == "optimize-blif" || command == "evaluate-blif") {
        SingleBlifOptions opts = parseSingleBlifOptions(
            {"--config", configPath.string()}, kLutMaxInputs, command);
        AppContext runCtx = ctx;
        applySingleBlifToolOverrides(runCtx, opts);
        const fs::path blifPath = resolveProjectPath(runCtx, opts.blifPath);
        fs::path libraryDir = opts.libraryDir.empty()
                                  ? findLatestGeneratedLibraryDir(runCtx.resultsRepoDir)
                                  : resolveResultsRepoPath(runCtx, opts.libraryDir);
        fs::path abcLocalLibraryDir = opts.abcLocalLibraryDir.empty()
                                          ? (libraryDir.empty()
                                                 ? fs::path{}
                                                 : libraryDir.parent_path() /
                                                       (libraryDir.filename().string() + "_abc_local"))
                                          : resolveResultsRepoPath(runCtx, opts.abcLocalLibraryDir);
        const fs::path outputDir = opts.outputDir.empty()
                                       ? defaultSingleBlifOutputDir(runCtx, opts, command)
                                       : resolveResultsRepoPath(runCtx, opts.outputDir);
        const fs::path resultJsonPath = opts.resultJsonPath.empty()
                                            ? outputDir / "result.json"
                                            : resolveProjectPath(runCtx, opts.resultJsonPath);

        checkPath("ABC executable", fs::path(runCtx.abcPath), false, true);
        checkPath("Python evaluation script", runCtx.pythonScriptPath, false, true);
        checkPath("Input BLIF", blifPath, false, true);
        checkPath("Library directory", libraryDir, true, true);
        checkPath("Library index", libraryDir / "final_results.csv", false, true);
        checkPath("Library detailed_infos", libraryDir / "detailed_infos", true, true);
        if (opts.abcLocalLibraryDir.empty()) {
            const bool exists = !abcLocalLibraryDir.empty() &&
                                fs::exists(abcLocalLibraryDir / "final_results.csv") &&
                                fs::exists(abcLocalLibraryDir / "detailed_infos");
            printStatus("ABC local library", exists ? "OK" : "AUTO",
                        abcLocalLibraryDir,
                        exists ? "prebuilt" : "will be auto-built during single-BLIF execution");
        } else {
            checkPath("ABC local library directory", abcLocalLibraryDir, true, true);
            checkPath("ABC local library index",
                      abcLocalLibraryDir / "final_results.csv",
                      false,
                      true);
            checkPath("ABC local library detailed_infos",
                      abcLocalLibraryDir / "detailed_infos",
                      true,
                      true);
        }
        if (opts.inputProbs.empty()) {
            std::cout << "[Doctor] Input probabilities: MISSING\n";
            ok = false;
        } else {
            std::cout << "[Doctor] Input probabilities: OK "
                      << opts.inputProbs.size() << " value(s)\n";
        }
        if (!opts.inputActs.empty()) {
            std::cout << "[Doctor] Input activities: OK "
                      << opts.inputActs.size() << " value(s)\n";
        } else {
            std::cout << "[Doctor] Input activities: INFO derived automatically from probabilities\n";
        }
        printStatus("Planned output directory", "INFO", outputDir,
                    "created on execution if missing");
        printStatus("Planned result JSON", "INFO", resultJsonPath);
        return ok ? 0 : 1;
    }

    throw std::runtime_error("Unsupported config command: " + command);
}

int runValidateConfigCommand(const AppContext& ctx,
                             const std::vector<std::string>& args) {
    fs::path configPath;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--config") {
            if (i + 1 >= args.size()) {
                throw std::runtime_error("Missing value for option --config");
            }
            configPath = args[i + 1];
            break;
        }
        const std::string prefix = "--config=";
        if (args[i].rfind(prefix, 0) == 0) {
            configPath = args[i].substr(prefix.size());
            break;
        }
    }
    if (configPath.empty()) {
        throw std::runtime_error("validate-config requires --config <file>.");
    }

    const AppConfig config = loadAppConfig(configPath, kLutMaxInputs);
    const std::string command = config.command;
    if (command.empty()) {
        throw std::runtime_error("Config must set the top-level command field.");
    }

    if (command == "generate") {
        GenerateOptions opts = parseGenerateOptions({"--config", configPath.string()},
                                                    kLutMaxInputs);
        AppContext runCtx = ctx;
        applyGenerateToolOverrides(runCtx, opts);
        opts.outputDir = resolveGenerateOutputDir(
            runCtx.projectRoot,
            runCtx.resultsRepoDir,
            opts.outputDir,
            defaultGenerateOutputDir(runCtx, opts));
        requireConfiguredFile(fs::path(runCtx.abcPath),
                              "ABC executable",
                              "Set dependencies.abc_path in the JSON config or export ABC_PATH.");
        requireFile(runCtx.pythonScriptPath, "Python evaluation script");
        requireFile(runCtx.genlibPath, "ABC genlib");
        requireFile(runCtx.libertyPath, "Standard-cell Liberty file");
        std::cout << "[validate-config] OK command=generate\n";
        std::cout << "[validate-config] Output: "
                  << fs::absolute(opts.outputDir) << "\n";
        return 0;
    }

    if (command == "evaluate" || command == "benchmark" || command == "optimize") {
        EvaluationOptions opts =
            parseEvaluationOptions({"--config", configPath.string()}, kLutMaxInputs);
        AppContext runCtx = ctx;
        applyEvaluationToolOverrides(runCtx, opts);
        const fs::path benchmarkDir = opts.benchmarkDir.empty()
                                          ? runCtx.defaultBenchmarkDir
                                          : resolveProjectPath(runCtx, opts.benchmarkDir);
        requireDirectory(benchmarkDir, "Benchmark directory");
        const PreparedLibraries prepared = prepareEvaluationLibraries(
            runCtx, opts.libraryDir, opts.abcLocalLibraryDir, false);
        const fs::path outputDir = opts.outputDir.empty()
                                       ? prepared.libraryDir
                                       : resolveResultsRepoPath(runCtx, opts.outputDir);
        std::cout << "[validate-config] OK command=" << command << "\n";
        std::cout << "[validate-config] Library: "
                  << fs::absolute(prepared.libraryDir) << "\n";
        std::cout << "[validate-config] Benchmarks: "
                  << fs::absolute(benchmarkDir) << "\n";
        std::cout << "[validate-config] ABC local library: "
                  << fs::absolute(prepared.abcLocalLibraryDir) << "\n";
        std::cout << "[validate-config] Output: "
                  << fs::absolute(outputDir) << "\n";
        return 0;
    }

    if (command == "optimize-blif" || command == "evaluate-blif") {
        SingleBlifOptions opts = parseSingleBlifOptions(
            {"--config", configPath.string()}, kLutMaxInputs, command);
        AppContext runCtx = ctx;
        applySingleBlifToolOverrides(runCtx, opts);
        requireConfiguredFile(fs::path(runCtx.abcPath),
                              "ABC executable",
                              "Set dependencies.abc_path in the JSON config or export ABC_PATH.");
        requireFile(runCtx.pythonScriptPath, "Python evaluation script");
        requireFile(resolveProjectPath(runCtx, opts.blifPath), "Input BLIF");
        const PreparedLibraries prepared = prepareEvaluationLibraries(
            runCtx, opts.libraryDir, opts.abcLocalLibraryDir, false);
        const fs::path outputDir = opts.outputDir.empty()
                                       ? defaultSingleBlifOutputDir(runCtx, opts, command)
                                       : resolveResultsRepoPath(runCtx, opts.outputDir);
        std::cout << "[validate-config] OK command=" << command << "\n";
        std::cout << "[validate-config] BLIF: "
                  << fs::absolute(resolveProjectPath(runCtx, opts.blifPath)) << "\n";
        std::cout << "[validate-config] Library: "
                  << fs::absolute(prepared.libraryDir) << "\n";
        std::cout << "[validate-config] Output: "
                  << fs::absolute(outputDir) << "\n";
        return 0;
    }

    throw std::runtime_error("Unsupported config command: " + command);
}

void printApiFailureJson(const std::string& message) {
    SingleOptResult result;
    result.success = false;
    result.errorMessage = message;
    std::cout << result.toJson() << std::endl;
}

int runLegacyApiMode(const AppContext& ctx, int argc, char** argv) {
    requireConfiguredFile(
        fs::path(ctx.abcPath),
        "ABC executable",
        "Set ABC_PATH before using the legacy stdin API.");

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

int runApplication(int argc, char** argv) {
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
            } else if (args[0] == "optimize-blif" ||
                       args[0] == "evaluate-blif") {
                printSingleBlifHelp(ctx, args[0]);
            } else if (args[0] == "doctor") {
                printDoctorHelp();
            } else if (args[0] == "validate-config") {
                printValidateConfigHelp();
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

        if (command == "optimize-blif" || command == "evaluate-blif") {
            const bool wantsJson = hasFlag(args, "--json");
            try {
                const SingleBlifOptions opts =
                    parseSingleBlifOptions(args, kLutMaxInputs, command);
                if (opts.help) {
                    printSingleBlifHelp(ctx, command);
                    return 0;
                }
                return runSingleBlifCommand(ctx, opts, command);
            } catch (const std::exception& e) {
                if (wantsJson) {
                    SingleBlifResult result;
                    result.success = false;
                    result.command = command;
                    result.errorMessage = e.what();
                    std::cout << result.toJson() << std::endl;
                    return 1;
                }
                throw;
            }
        }

        if (command == "doctor") {
            if (hasFlag(args, "-h") || hasFlag(args, "--help")) {
                printDoctorHelp();
                return 0;
            }
            return runDoctorCommand(ctx, args);
        }

        if (command == "validate-config") {
            if (std::find(args.begin(), args.end(), "-h") != args.end() ||
                std::find(args.begin(), args.end(), "--help") != args.end()) {
                printValidateConfigHelp();
                return 0;
            }
            return runValidateConfigCommand(ctx, args);
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

}  // namespace fes::app
