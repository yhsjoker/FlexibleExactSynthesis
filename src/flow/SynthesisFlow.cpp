#include "fes/flow/SynthesisFlow.h"

#include "fes/core/Types.h"
#include "fes/core/NpnTransform.h"
#include "fes/core/Specification.h"
#include "fes/encoders/PatternEncoder.h"
#include "fes/interfaces/ISolver.h"
#include "fes/solvers/KissatSolver.h"
#include "fes/solvers/Z3Solver.h"
#include "fes/utils/BenchmarkExtractor.h"
#include "fes/utils/BlifWriter.h"
#include "fes/utils/CellLibraryLoader.h"
#include "fes/utils/EquivalenceChecker.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace fes {

namespace {

struct GeneratedLibraryCsvEntry {
    std::string sortKey;
    std::string csvLine;
};

class GeneratedLibrary {
public:
    void addEntry(GeneratedLibraryCsvEntry entry) {
        entries_.push_back(std::move(entry));
    }

    const std::vector<GeneratedLibraryCsvEntry>& entries() const {
        return entries_;
    }

private:
    std::vector<GeneratedLibraryCsvEntry> entries_;
};

std::map<std::string, GeneratedLibraryCsvEntry> loadGeneratedLibraryRows(
    const std::string& csvPath) {
    std::map<std::string, GeneratedLibraryCsvEntry> rows;
    std::ifstream input(csvPath);
    if (!input.is_open()) {
        return rows;
    }

    std::string line;
    bool isHeader = true;
    while (std::getline(input, line)) {
        if (isHeader) {
            isHeader = false;
            continue;
        }
        if (line.empty()) {
            continue;
        }

        std::vector<std::string> fields;
        std::string current;
        bool inQuotes = false;
        for (size_t i = 0; i < line.size(); ++i) {
            const char c = line[i];
            if (inQuotes) {
                if (c == '"') {
                    if (i + 1 < line.size() && line[i + 1] == '"') {
                        current += '"';
                        ++i;
                    } else {
                        inQuotes = false;
                    }
                } else {
                    current += c;
                }
            } else if (c == ',') {
                fields.push_back(current);
                current.clear();
            } else if (c == '"') {
                inQuotes = true;
            } else {
                current += c;
            }
        }
        fields.push_back(current);

        if (fields.size() < 2) {
            continue;
        }
        const std::string sortKey = fields[0] + fields[1];
        rows[sortKey] = GeneratedLibraryCsvEntry{sortKey, line + "\n"};
    }
    return rows;
}

bool textContainsTimeout(std::string text) {
    std::transform(
        text.begin(),
        text.end(),
        text.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
    return text.find("timeout") != std::string::npos;
}

RunStatus statusForResult(const SynthesisResult& result) {
    if (result.success) {
        return RunStatus::kSuccess;
    }
    return textContainsTimeout(result.errorMsg) ? RunStatus::kTimeout
                                                : RunStatus::kFailed;
}

std::string trimCopy(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }

    const size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string toUpperCopy(std::string text) {
    std::transform(
        text.begin(),
        text.end(),
        text.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
    return text;
}

std::string shellQuote(const std::string& text) {
    std::string quoted = "'";
    for (char c : text) {
        if (c == '\'') {
            quoted += "'\\''";
        } else {
            quoted += c;
        }
    }
    quoted += "'";
    return quoted;
}

std::vector<std::string> parseCsvRow(const std::string& line) {
    std::vector<std::string> fields;
    std::string current;
    bool inQuotes = false;

    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (inQuotes) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    current += '"';
                    ++i;
                } else {
                    inQuotes = false;
                }
            } else {
                current += c;
            }
        } else if (c == ',') {
            fields.push_back(trimCopy(current));
            current.clear();
        } else if (c == '"') {
            inQuotes = true;
        } else {
            current += c;
        }
    }

    fields.push_back(trimCopy(current));
    return fields;
}

bool readLogicalLine(std::istream& is, std::string& outLine) {
    outLine.clear();

    std::string line;
    if (!std::getline(is, line)) {
        return false;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    outLine = line;

    while (!outLine.empty()) {
        const size_t last = outLine.find_last_not_of(" \t\r\n");
        if (last == std::string::npos) {
            outLine.clear();
            break;
        }
        if (outLine[last] != '\\') {
            break;
        }

        outLine.erase(last);
        std::string continuation;
        if (!std::getline(is, continuation)) {
            break;
        }
        if (!continuation.empty() && continuation.back() == '\r') {
            continuation.pop_back();
        }
        outLine += " " + continuation;
    }

    return true;
}

LutTruthTable hexToTruthTable(const std::string& hexText) {
    std::string normalized = trimCopy(hexText);
    if (normalized.rfind("0x", 0) == 0 || normalized.rfind("0X", 0) == 0) {
        normalized = normalized.substr(2);
    }
    if (normalized.empty()) {
        return 0;
    }

    std::stringstream ss;
    ss << std::hex << normalized;
    LutTruthTable value = 0;
    ss >> value;
    return value;
}

int inferNumInputsFromHexWidth(const std::string& hexFunc) {
    const size_t bits = trimCopy(hexFunc).size() * 4;
    int numInputs = 0;
    while ((1u << numInputs) < bits) {
        ++numInputs;
    }
    return std::max(1, numInputs);
}

LutTruthTable truthTableMaskForInputs(int numInputs) {
    if (numInputs < 0) {
        return 0;
    }
    const int rows = 1 << numInputs;
    if (rows >= 64) {
        return ~LutTruthTable{0};
    }
    return (LutTruthTable{1} << rows) - 1;
}

std::string formatHexTruthTable(LutTruthTable tt) {
    std::ostringstream oss;
    oss << std::uppercase
        << std::hex
        << std::setfill('0')
        << std::setw(kLutTruthTableHexDigits)
        << tt;
    return oss.str();
}

std::string normalizeHexForInputs(const std::string& hexFunc, int numInputs) {
    return formatHexTruthTable(
        hexToTruthTable(hexFunc) & truthTableMaskForInputs(numInputs));
}

int resolveGateTypeIndex(const std::string& gateType,
                         const std::vector<GateType>& library) {
    if (gateType.empty()) {
        return -1;
    }

    const size_t pos = gateType.find_last_of('_');
    if (pos != std::string::npos && pos + 1 < gateType.size()) {
        try {
            const int idx = std::stoi(gateType.substr(pos + 1));
            if (idx >= 0 && idx < static_cast<int>(library.size())) {
                return idx;
            }
        } catch (...) {
        }
    }

    for (size_t i = 0; i < library.size(); ++i) {
        if (library[i].name == gateType) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

AbcStats convertPpaToAbcStats(const PPAResult& ppa) {
    AbcStats stats;
    stats.area = ppa.area;
    stats.power = ppa.power_total;
    stats.gates = -1;
    stats.valid = ppa.valid;
    return stats;
}

int parseInputIndexFromName(const std::string& name) {
    size_t pos = name.size();
    while (pos > 0 && std::isdigit(static_cast<unsigned char>(name[pos - 1]))) {
        --pos;
    }
    if (pos == name.size()) {
        return -1;
    }

    try {
        return std::stoi(name.substr(pos));
    } catch (...) {
        return -1;
    }
}

std::vector<StandardCell> maybeLoadStandardCells(const std::string& libPath,
                                                 const std::string& outputDir,
                                                 int lutInputs) {
    const fs::path libFile(libPath);
    std::string ext = toUpperCopy(libFile.extension().string());
    if (ext != ".LIB") {
        return {};
    }

    fs::path csvPath = fs::path(outputDir);
    if (csvPath.empty()) {
        csvPath = fs::current_path();
    }
    csvPath /= "parsed_cells_k" + std::to_string(lutInputs) + ".csv";

    return CellLibraryLoader::loadOrGenerate(
        csvPath.string(), libPath, lutInputs);
}

}  // namespace

SynthesisFlow::SynthesisFlow(const std::vector<GateType>& lib,
                             const std::string& abcPath,
                             const std::string& libPath,
                             const std::string& pythonScriptPath)
    : library_(lib),
      abcPath_(abcPath),
      libPath_(libPath),
      verifier_(pythonScriptPath) {}

void SynthesisFlow::runAbcToGenerateBaseline(const std::string& hexFunc,
                                             int numInputs,
                                             const std::string& outBlifPath) {
    const fs::path outPath(outBlifPath);
    if (outPath.has_parent_path()) {
        fs::create_directories(outPath.parent_path());
    }

    const fs::path rawPath =
        outPath.parent_path() / (outPath.stem().string() + "_raw.blif");
    {
        std::ofstream rawStream(rawPath);
        if (!rawStream.is_open()) {
            std::cerr << "[ABC] Failed to create raw BLIF: " << rawPath << "\n";
            return;
        }
        rawStream << buildRawBlifFromHexFunc(hexFunc, numInputs);
    }

    const std::string script =
        "read_genlib " + libPath_ +
        "; read_blif " + rawPath.string() +
        "; strash; balance; rewrite; rewrite -z; balance; rewrite -z; "
          "balance; map -a; write_blif " + outBlifPath;
    const std::string cmd =
        abcPath_ + " -c \"" + script + "\" > /dev/null 2>&1";

    if (std::system(cmd.c_str()) != 0) {
        std::cerr << "[ABC] Baseline generation failed for "
                  << hexFunc << "\n";
    }
}

SynthesisResult SynthesisFlow::run(const std::string& hexFunc,
                                   int numInputs,
                                   const std::vector<double>& inputProbs,
                                   const std::string& probTag,
                                   const std::string& outputDir,
                                   int maxGates,
                                   bool enablePhysicalEval,
                                   const std::string& workerScratchDir,
                                   int satTimeoutMs,
                                   int optTimeoutMs) {
    SynthesisResult res{};
    res.hexFunc = normalizeHexForInputs(hexFunc, numInputs);
    res.numInputs = numInputs;
    res.success = false;
    res.ponoGates = -1;
    res.internalCost = -1.0;
    res.runtimeMs = 0.0;

    const std::string variantName = res.hexFunc + probTag;
    const auto started = std::chrono::steady_clock::now();
    auto finalize = [&]() -> SynthesisResult {
        res.runtimeMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started)
                .count();
        return res;
    };

    try {
        Specification spec = buildSpecification(res.hexFunc, numInputs, inputProbs);
        const int minGates =
            runMinimizationPhase(spec, maxGates, satTimeoutMs);
        if (minGates < 0) {
            res.errorMsg = "UNSAT within max gates";
            return finalize();
        }

        auto [optSuccess, graph, internalCost] =
            runOptimizationPhase(spec, minGates, optTimeoutMs);
        if (!optSuccess) {
            res.errorMsg = "Z3 optimization failed";
            return finalize();
        }

        if (verifyEnabled_) {
            EquivalenceChecker cec(library_);
            const auto cecResult = cec.verifyAgainstTruthTable(
                hexToTruthTable(res.hexFunc) & truthTableMaskForInputs(numInputs),
                numInputs,
                graph);
            if (!cecResult.equivalent) {
                res.errorMsg = "CEC: " + cecResult.message;
                return finalize();
            }
        }

        res.ponoGates = minGates;
        res.internalCost = internalCost;

        processResults(res, graph, outputDir, variantName);
        if (!res.errorMsg.empty()) {
            return finalize();
        }

        if (enablePhysicalEval) {
            fs::path scratchDir = workerScratchDir.empty()
                                      ? (fs::path(outputDir) / "tmp_eval")
                                      : fs::path(workerScratchDir);
            fs::create_directories(scratchDir);

            const fs::path ponoBlif =
                fs::path(outputDir) / "detailed_infos" / res.hexFunc /
                ("pono_" + variantName + ".blif");
            const fs::path baselineBlif =
                scratchDir / ("baseline_" + variantName + ".blif");

            runAbcToGenerateBaseline(res.hexFunc, numInputs, baselineBlif.string());

            std::vector<double> acts(inputProbs.size(), 0.0);
            for (size_t i = 0; i < inputProbs.size(); ++i) {
                acts[i] = 2.0 * inputProbs[i] * (1.0 - inputProbs[i]);
            }

            res.baselineStats = convertPpaToAbcStats(
                verifier_.getPPAResult(baselineBlif.string(), inputProbs, acts));
            res.optStats = convertPpaToAbcStats(
                verifier_.getPPAResult(ponoBlif.string(), inputProbs, acts));
            res.success = res.baselineStats.valid && res.optStats.valid;
            if (!res.success) {
                res.errorMsg = "Innovus evaluation failed";
            }
        } else {
            res.success = res.errorMsg.empty();
        }
    } catch (const std::exception& e) {
        res.errorMsg = e.what();
    }

    return finalize();
}

SynthesisResult SynthesisFlow::run(const std::string& hexFunc,
                                   const std::vector<double>& inputProbs,
                                   const std::string& probTag,
                                   const std::string& outputDir,
                                   int maxGates,
                                   bool enablePhysicalEval) {
    return run(
        hexFunc,
        inferNumInputsFromHexWidth(hexFunc),
        inputProbs,
        probTag,
        outputDir,
        maxGates,
        enablePhysicalEval);
}

void SynthesisFlow::runBatch(const std::string& inputFile,
                             const std::string& outputCsv,
                             const std::string& outputDir,
                             bool enablePhysicalEval) {
    LibraryGenerationConfig cfg;
    cfg.mode = LibraryGenerationMode::kFromCsv;
    cfg.lutInputs = kLutMaxInputs;
    cfg.inputCsv = inputFile;
    cfg.outputCsv = outputCsv;
    cfg.outputDir = outputDir;
    cfg.enablePhysicalEval = enablePhysicalEval;
    runBatch(cfg);
}

bool SynthesisFlow::runBatch(const LibraryGenerationConfig& cfg) {
    try {
        const auto loadedCells =
            maybeLoadStandardCells(libPath_, cfg.outputDir, cfg.lutInputs);
        if (!loadedCells.empty()) {
            std::cout << "[Batch] Loaded " << loadedCells.size()
                      << " standard cells for K<=" << cfg.lutInputs << "\n";
        }

        std::vector<LibraryFunction> functions;
        switch (cfg.mode) {
            case LibraryGenerationMode::kFromCsv:
                functions = loadTopHexFuncsFromCsv(cfg.inputCsv, cfg.lutInputs);
                break;
            case LibraryGenerationMode::kExhaustiveNpn:
                functions = buildExhaustiveFunctionSet(
                    cfg.lutInputs, cfg.numFunctionsToInclude);
                break;
            case LibraryGenerationMode::kBenchmarkDriven:
                functions = buildBenchmarkDrivenFunctionSet(cfg);
                break;
        }

        if (functions.empty()) {
            std::cerr << "[Batch] No functions selected.\n";
            return false;
        }

        if (cfg.numFunctionsToInclude > 0 &&
            static_cast<int>(functions.size()) > cfg.numFunctionsToInclude) {
            functions.resize(cfg.numFunctionsToInclude);
        }

        fs::create_directories(cfg.outputDir);
        fs::create_directories(fs::path(cfg.outputDir) / "detailed_infos");
        fs::create_directories(fs::path(cfg.outputDir) / "tmp_eval");

        RunManifest manifest(fs::path(cfg.outputDir) / "run_manifest.csv");
        manifest.load();
        const auto previousRows = loadGeneratedLibraryRows(cfg.outputCsv);

        GeneratedLibrary m_library;
        std::mutex libraryMutex;
        std::mutex manifestMutex;
        std::mutex logMutex;
        std::atomic<size_t> nextIndex{0};
        std::atomic<int> totalCaseCount{0};
        std::atomic<int> runCaseCount{0};
        std::atomic<int> skippedCount{0};
        std::atomic<int> resumedCount{0};
        std::atomic<int> successCount{0};
        std::atomic<int> failureCount{0};
        std::atomic<int> timeoutCount{0};

        const unsigned hw = cfg.workerCount > 0
                                ? cfg.workerCount
                                : std::max(1u, std::thread::hardware_concurrency());
        const unsigned threadCount = std::min<unsigned>(
            hw, std::max<unsigned>(1u, static_cast<unsigned>(functions.size())));

        auto worker = [&]() {
            while (true) {
                const size_t index = nextIndex.fetch_add(1);
                if (index >= functions.size()) {
                    break;
                }

                const LibraryFunction& func = functions[index];
                const auto probPatterns =
                    generateActivityPatterns(
                        func.numInputs, cfg.activityPatternSpec);

                {
                    std::lock_guard<std::mutex> lock(logMutex);
                    std::cout << "[Batch] Synthesizing " << func.hexFunc
                              << " (K=" << func.numInputs
                              << ", freq=" << func.frequency << ")\n";
                }

                for (const auto& inputProbs : probPatterns) {
                    const std::string probTag = probVectorToTag(inputProbs);
                    const std::string variantName = func.hexFunc + probTag;
                    ++totalCaseCount;

                    bool shouldRun = true;
                    bool isResumeAttempt = false;
                    {
                        std::lock_guard<std::mutex> lock(manifestMutex);
                        shouldRun =
                            manifest.shouldRun(variantName, cfg.resumePolicy);
                        isResumeAttempt =
                            manifest.isResumeAttempt(variantName, cfg.resumePolicy);
                    }

                    if (!shouldRun) {
                        ++skippedCount;
                        const auto previous = previousRows.find(variantName);
                        if (previous != previousRows.end()) {
                            std::lock_guard<std::mutex> lock(libraryMutex);
                            m_library.addEntry(previous->second);
                        }
                        continue;
                    }

                    if (isResumeAttempt) {
                        ++resumedCount;
                    }
                    ++runCaseCount;

                    SynthesisResult res = run(
                        func.hexFunc,
                        func.numInputs,
                        inputProbs,
                        probTag,
                        cfg.outputDir,
                        10,
                        cfg.enablePhysicalEval,
                        (fs::path(cfg.outputDir) / "tmp_eval").string(),
                        cfg.satTimeoutMs,
                        cfg.optTimeoutMs);

                    if (cfg.caseTimeoutMs > 0 &&
                        res.runtimeMs > static_cast<double>(cfg.caseTimeoutMs)) {
                        res.success = false;
                        res.errorMsg = "Case timeout threshold exceeded";
                    }

                    std::ostringstream row;
                    row << res.hexFunc << "," << probTag << ","
                        << (res.success ? "true" : "false") << ","
                        << res.ponoGates << ","
                        << res.internalCost << ","
                        << res.runtimeMs;
                    if (cfg.enablePhysicalEval) {
                        const double improvement =
                            (res.baselineStats.power > 0.0 && res.optStats.valid)
                                ? (res.baselineStats.power - res.optStats.power) /
                                      res.baselineStats.power * 100.0
                                : 0.0;
                        row << "," << res.baselineStats.power
                            << "," << res.optStats.power
                            << "," << improvement;
                    }
                    row << "," << res.errorMsg << "\n";

                    {
                        std::lock_guard<std::mutex> lock(libraryMutex);
                        m_library.addEntry({variantName, row.str()});
                    }

                    if (res.success) {
                        ++successCount;
                    } else {
                        if (statusForResult(res) == RunStatus::kTimeout) {
                            ++timeoutCount;
                        } else {
                            ++failureCount;
                        }
                    }

                    RunManifestEntry manifestEntry;
                    manifestEntry.caseId = variantName;
                    manifestEntry.kind = "generate";
                    manifestEntry.functionId = func.hexFunc;
                    manifestEntry.activityTag = probTag;
                    manifestEntry.activityMode = cfg.activityPatternMode;
                    manifestEntry.status = statusForResult(res);
                    manifestEntry.runtimeMs = res.runtimeMs;
                    manifestEntry.outputPath =
                        (fs::path(cfg.outputDir) / "detailed_infos" /
                         res.hexFunc / ("pono_" + variantName + ".blif"))
                            .string();
                    manifestEntry.reason = res.errorMsg;
                    {
                        std::lock_guard<std::mutex> lock(manifestMutex);
                        manifest.update(manifestEntry);
                        manifest.flush();
                    }
                }
            }
        };

        std::vector<std::thread> workers;
        workers.reserve(threadCount);
        for (unsigned i = 0; i < threadCount; ++i) {
            workers.emplace_back(worker);
        }
        for (auto& thread : workers) {
            thread.join();
        }

        std::vector<GeneratedLibraryCsvEntry> rows = m_library.entries();
        std::sort(
            rows.begin(),
            rows.end(),
            [](const GeneratedLibraryCsvEntry& lhs,
               const GeneratedLibraryCsvEntry& rhs) {
                return lhs.sortKey < rhs.sortKey;
            });

        std::ofstream out(cfg.outputCsv);
        if (!out.is_open()) {
            std::cerr << "[Batch] Failed to open output CSV: "
                      << cfg.outputCsv << "\n";
            return false;
        }

        out << "HexFunc,ProbPattern,Success,Gates,InternalCost,RuntimeMs";
        if (cfg.enablePhysicalEval) {
            out << ",Base_Power(mW),Opt_Power(mW),Power_Impr(%)";
        }
        out << ",Error\n";
        for (const auto& row : rows) {
            out << row.csvLine;
        }

        RunSummary summary;
        summary.command = "generate";
        summary.activityMode = cfg.activityPatternMode;
        summary.outputCsv = cfg.outputCsv;
        summary.manifestCsv = manifest.path();
        summary.totalCases = static_cast<std::size_t>(totalCaseCount.load());
        summary.runCases = static_cast<std::size_t>(runCaseCount.load());
        summary.skippedCases = static_cast<std::size_t>(skippedCount.load());
        summary.resumedCases = static_cast<std::size_t>(resumedCount.load());
        summary.successCases = static_cast<std::size_t>(successCount.load());
        summary.failedCases = static_cast<std::size_t>(failureCount.load());
        summary.timeoutCases = static_cast<std::size_t>(timeoutCount.load());
        writeRunSummaryJson(
            fs::path(cfg.outputDir) / "generation_summary.json", summary);

        std::cout << "[Batch] Done. Functions=" << functions.size()
                  << ", Variants=" << rows.size()
                  << ", Success=" << successCount.load()
                  << ", Fail=" << failureCount.load()
                  << ", Timeout=" << timeoutCount.load()
                  << ", Skipped=" << skippedCount.load()
                  << ", CSV=" << cfg.outputCsv << "\n";
        return successCount.load() > 0 || skippedCount.load() > 0;
    } catch (const std::exception& e) {
        std::cerr << "[Batch] Exception: " << e.what() << "\n";
        return false;
    }
}

bool SynthesisFlow::generateLibrary(const LibraryGenerationConfig& cfg) {
    return runBatch(cfg);
}

bool SynthesisFlow::buildABCLocalLibraryFromTopCsv(
    const std::string& topCsvPath,
    const std::string& outputDir) {
    auto functions = loadTopHexFuncsFromCsv(topCsvPath, kLutMaxInputs);
    if (functions.empty()) {
        std::cerr << "[ABC-LIB] No functions loaded from " << topCsvPath << "\n";
        return false;
    }

    const auto loadedCells =
        maybeLoadStandardCells(libPath_, outputDir, kLutMaxInputs);
    if (!loadedCells.empty()) {
        std::cout << "[ABC-LIB] Loaded " << loadedCells.size()
                  << " standard cells for K<=" << kLutMaxInputs << "\n";
    }

    fs::create_directories(outputDir);
    fs::create_directories(fs::path(outputDir) / "detailed_infos");
    fs::create_directories(fs::path(outputDir) / "tmp_eval");

    GeneratedLibrary m_library;
    std::mutex libraryMutex;
    std::mutex logMutex;
    std::atomic<size_t> nextIndex{0};
    std::atomic<int> totalCases{0};
    std::atomic<int> successCases{0};

    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const unsigned threadCount = std::min<unsigned>(
        hw, std::max<unsigned>(1u, static_cast<unsigned>(functions.size())));

    auto worker = [&]() {
        while (true) {
            const size_t index = nextIndex.fetch_add(1);
            if (index >= functions.size()) {
                break;
            }

            const LibraryFunction& func = functions[index];
            const auto probPatterns = generateActivityPatterns(func.numInputs);

            {
                std::lock_guard<std::mutex> lock(logMutex);
                std::cout << "[ABC-LIB] Processing " << func.hexFunc
                          << " (K=" << func.numInputs << ")\n";
            }

            for (const auto& inputProbs : probPatterns) {
                ++totalCases;

                std::string tmpBlif =
                    (fs::path(outputDir) / "tmp_eval" /
                     ("thread_" +
                      std::to_string(
                          std::hash<std::thread::id>{}(
                              std::this_thread::get_id())) +
                      ".blif"))
                        .string();

                std::string csvLine;
                const bool ok = runSingleABCLocalCase(
                    func, inputProbs, outputDir, tmpBlif, &csvLine);
                if (ok) {
                    ++successCases;
                }

                {
                    std::lock_guard<std::mutex> lock(libraryMutex);
                    m_library.addEntry(
                        {func.hexFunc + probVectorToTag(inputProbs), csvLine});
                }
            }
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(threadCount);
    for (unsigned i = 0; i < threadCount; ++i) {
        workers.emplace_back(worker);
    }
    for (auto& thread : workers) {
        thread.join();
    }

    std::vector<GeneratedLibraryCsvEntry> rows = m_library.entries();
    std::sort(
        rows.begin(),
        rows.end(),
        [](const GeneratedLibraryCsvEntry& lhs,
           const GeneratedLibraryCsvEntry& rhs) {
            return lhs.sortKey < rhs.sortKey;
        });

    const fs::path csvPath = fs::path(outputDir) / "final_results.csv";
    std::ofstream csvOut(csvPath);
    if (!csvOut.is_open()) {
        std::cerr << "[ABC-LIB] Failed to create " << csvPath << "\n";
        return false;
    }

    csvOut << "HexFunc,ProbPattern,Success,Gates,InternalCost,RuntimeMs,Error\n";
    for (const auto& row : rows) {
        csvOut << row.csvLine;
    }

    std::cout << "[ABC-LIB] Done. Success=" << successCases.load()
              << "/" << totalCases.load()
              << ", CSV=" << csvPath << "\n";
    return successCases.load() > 0;
}

Specification SynthesisFlow::buildSpecification(
    const std::string& hexFunc,
    int numInputs,
    const std::vector<double>& inputProbs) {
    std::vector<double> probs = inputProbs;
    if (probs.size() < static_cast<size_t>(numInputs)) {
        probs.resize(numInputs, 0.5);
    } else if (probs.size() > static_cast<size_t>(numInputs)) {
        probs.resize(numInputs);
    }

    Specification spec(numInputs, 1);
    spec.setTruthTable(hexToTruthTable(hexFunc) & truthTableMaskForInputs(numInputs));
    spec.setInputProbabilities(probs);
    return spec;
}

Specification SynthesisFlow::buildSpecification(
    const std::string& hexFunc,
    const std::vector<double>& inputProbs) {
    return buildSpecification(
        hexFunc, inferNumInputsFromHexWidth(hexFunc), inputProbs);
}

int SynthesisFlow::runMinimizationPhase(const Specification& spec, int maxGates) {
    return runMinimizationPhase(spec, maxGates, 10000);
}

int SynthesisFlow::runMinimizationPhase(
    const Specification& spec,
    int maxGates,
    int satTimeoutMs) {
    for (int numGates = 1; numGates <= maxGates; ++numGates) {
        auto solver = std::make_unique<KissatSolver>();
        solver->setTimeLimit(static_cast<unsigned int>(std::max(0, satTimeoutMs)));
        PatternEncoder encoder(numGates);
        if (!encoder.encode(solver.get(), spec, library_)) {
            continue;
        }
        if (solver->solve() == SolveStatus::SAT) {
            return numGates;
        }
    }
    return -1;
}

std::tuple<bool, CircuitGraph, double> SynthesisFlow::runOptimizationPhase(
    const Specification& spec,
    int numGates) {
    return runOptimizationPhase(spec, numGates, 60000);
}

std::tuple<bool, CircuitGraph, double> SynthesisFlow::runOptimizationPhase(
    const Specification& spec,
    int numGates,
    int optTimeoutMs) {
    auto solver = std::make_unique<Z3Solver>();
    solver->setTimeLimit(static_cast<unsigned int>(std::max(0, optTimeoutMs)));

    PatternEncoder encoder(numGates);
    if (!encoder.encode(solver.get(), spec, library_)) {
        return {false, CircuitGraph(), 0.0};
    }

    const SolveStatus status = solver->solve();
    if (status == SolveStatus::OPTIMAL || status == SolveStatus::SAT) {
        return {
            true,
            encoder.decode(solver.get()),
            solver->getOptimizationResult()};
    }

    return {false, CircuitGraph(), 0.0};
}

void SynthesisFlow::processResults(SynthesisResult& res,
                                   const CircuitGraph& graph,
                                   const std::string& outputDir,
                                   const std::string& variantName) {
    const fs::path funcDir = fs::path(outputDir) / "detailed_infos" / res.hexFunc;
    fs::create_directories(funcDir);

    const fs::path blifPath = funcDir / ("pono_" + variantName + ".blif");
    BlifWriter::write(blifPath.string(), "pono_design", graph, library_);

    const fs::path txtPath = funcDir / ("pono_" + variantName + "_struct.txt");
    std::ofstream txt(txtPath);
    if (txt.is_open()) {
        txt << "Variant: " << variantName << "\n";
        txt << "HexFunc: " << res.hexFunc << "\n";
        txt << "NumInputs: " << res.numInputs << "\n";
        txt << "Gates: " << res.ponoGates << "\n";
        txt << "InternalCost: " << res.internalCost << "\n";
        for (const auto& nodePair : graph.getAllNodes()) {
            const Node& node = nodePair.second;
            if (node.type != NodeType::GATE) {
                continue;
            }

            const int typeIdx = resolveGateTypeIndex(node.gateType, library_);
            if (typeIdx < 0 || typeIdx >= static_cast<int>(library_.size())) {
                txt << node.name << " = CONST0()\n";
                continue;
            }

            const GateType& gate = library_[typeIdx];
            txt << node.name << " = " << gate.name << "(";
            const int limit = std::min(
                gate.numInputs, static_cast<int>(node.fanins.size()));
            for (int i = 0; i < limit; ++i) {
                const auto it = graph.getAllNodes().find(node.fanins[i]);
                txt << (it == graph.getAllNodes().end() ? "UNK" : it->second.name);
                if (i + 1 < limit) {
                    txt << ", ";
                }
            }
            txt << ")\n";
        }
    }

    if (!verify(res.hexFunc, res.numInputs, graph)) {
        res.success = false;
        res.errorMsg = "Verification Failed";
    }
}

AbcStats SynthesisFlow::runAbcCommand(const std::string& cmdScript) {
    AbcStats stats;
    const std::string cmd =
        shellQuote(abcPath_) + " -c " + shellQuote(cmdScript) + " 2>&1";

    std::array<char, 256> buffer{};
    std::string output;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(
        popen(cmd.c_str(), "r"), pclose);
    if (!pipe) {
        return stats;
    }

    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) != nullptr) {
        output += buffer.data();
    }

    try {
        std::regex reGates(R"((?:nd|nodes)\s*=\s*(\d+))", std::regex::icase);
        std::regex reArea(R"(area\s*=\s*([0-9.+-eE]+))", std::regex::icase);
        std::regex rePower(R"(power\s*=\s*([0-9.+-eE]+))", std::regex::icase);

        for (std::sregex_iterator it(output.begin(), output.end(), reGates);
             it != std::sregex_iterator();
             ++it) {
            stats.gates = std::stoi((*it)[1].str());
        }
        for (std::sregex_iterator it(output.begin(), output.end(), reArea);
             it != std::sregex_iterator();
             ++it) {
            stats.area = std::stod((*it)[1].str());
        }
        for (std::sregex_iterator it(output.begin(), output.end(), rePower);
             it != std::sregex_iterator();
             ++it) {
            stats.power = std::stod((*it)[1].str());
        }
    } catch (...) {
    }

    stats.valid = (stats.gates > 0 || stats.area > 0.0 || stats.power > 0.0);
    return stats;
}

AbcStats SynthesisFlow::evaluateBlif(const std::string& blifFile) {
    const std::string script =
        "read_genlib " + libPath_ +
        "; read_blif " + blifFile +
        "; ps -p";
    return runAbcCommand(script);
}

AbcStats SynthesisFlow::runAbcBaseline(const std::string& hexFunc, int numInputs) {
    fs::create_directories("tmp_eval");

    const std::string threadTag =
        std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    const fs::path mappedPath = fs::path("tmp_eval") / ("baseline_" + threadTag + ".blif");
    runAbcToGenerateBaseline(hexFunc, numInputs, mappedPath.string());
    return evaluateBlif(mappedPath.string());
}

bool SynthesisFlow::verify(const std::string& hexFunc,
                           int numInputs,
                           const CircuitGraph& graph) {
    const LutTruthTable expected =
        hexToTruthTable(hexFunc) & truthTableMaskForInputs(numInputs);
    const int totalPatterns = 1 << numInputs;

    if (graph.getOutputs().empty()) {
        return false;
    }

    for (int mask = 0; mask < totalPatterns; ++mask) {
        std::map<int, bool> nodeValues;
        const auto& pis = graph.getInputs();
        for (size_t i = 0; i < pis.size(); ++i) {
            nodeValues[pis[i]] = ((mask >> static_cast<int>(i)) & 1) != 0;
        }

        for (const auto& entry : graph.getAllNodes()) {
            const Node& node = entry.second;
            if (node.type != NodeType::GATE) {
                if (node.type == NodeType::CONST0) {
                    nodeValues[node.id] = false;
                } else if (node.type == NodeType::CONST1) {
                    nodeValues[node.id] = true;
                }
                continue;
            }

            const int typeIdx = resolveGateTypeIndex(node.gateType, library_);
            if (typeIdx < 0 || typeIdx >= static_cast<int>(library_.size())) {
                nodeValues[node.id] = false;
                continue;
            }

            const GateType& gate = library_[typeIdx];
            int ttIndex = 0;
            const int limit = std::min(
                gate.numInputs, static_cast<int>(node.fanins.size()));
            for (int i = 0; i < limit; ++i) {
                const auto it = nodeValues.find(node.fanins[i]);
                if (it != nodeValues.end() && it->second) {
                    ttIndex |= (1 << i);
                }
            }
            nodeValues[node.id] =
                ((gate.truthTable >> ttIndex) & 1ULL) != 0;
        }

        const bool got = nodeValues[graph.getOutputs().front()];
        const bool want = ((expected >> mask) & 1ULL) != 0;
        if (got != want) {
            return false;
        }
    }

    return true;
}

std::vector<LibraryFunction> SynthesisFlow::loadTopHexFuncsFromCsv(
    const std::string& topCsvPath,
    int defaultNumInputs) const {
    std::vector<LibraryFunction> functions;
    std::ifstream input(topCsvPath);
    if (!input.is_open()) {
        std::cerr << "[Batch] Failed to open CSV: " << topCsvPath << "\n";
        return functions;
    }

    std::map<std::pair<std::string, int>, std::size_t> dedup;
    std::string line;
    while (std::getline(input, line)) {
        line = trimCopy(line);
        if (line.empty()) {
            continue;
        }

        const auto fields = parseCsvRow(line);
        if (fields.empty()) {
            continue;
        }
        if (toUpperCopy(fields[0]) == "HEXFUNC") {
            continue;
        }

        int numInputs = defaultNumInputs;
        std::size_t frequency = 0;
        if (fields.size() >= 2) {
            try {
                const int parsed = std::stoi(fields[1]);
                if (parsed > 0 && parsed <= kLutMaxInputs) {
                    numInputs = parsed;
                } else if (parsed > 0) {
                    frequency = static_cast<std::size_t>(parsed);
                }
            } catch (...) {
            }
        }
        if (fields.size() >= 3) {
            try {
                const long long parsed = std::stoll(fields[2]);
                if (parsed > 0) {
                    frequency = static_cast<std::size_t>(parsed);
                }
            } catch (...) {
            }
        }

        const std::string hex = normalizeHexForInputs(fields[0], numInputs);
        dedup[{hex, numInputs}] = std::max(dedup[{hex, numInputs}], frequency);
    }

    functions.reserve(dedup.size());
    for (const auto& item : dedup) {
        functions.push_back(
            LibraryFunction{item.first.first, item.first.second, item.second});
    }

    std::sort(
        functions.begin(),
        functions.end(),
        [](const LibraryFunction& lhs, const LibraryFunction& rhs) {
            if (lhs.frequency != rhs.frequency) {
                return lhs.frequency > rhs.frequency;
            }
            if (lhs.numInputs != rhs.numInputs) {
                return lhs.numInputs < rhs.numInputs;
            }
            return lhs.hexFunc < rhs.hexFunc;
        });

    return functions;
}

std::vector<LibraryFunction> SynthesisFlow::buildExhaustiveFunctionSet(
    int numInputs,
    int numFunctionsToInclude) const {
    if (numInputs <= 0 || numInputs > kLutMaxInputs) {
        throw std::runtime_error("Exhaustive mode K out of range.");
    }
    if (numInputs > 4) {
        throw std::runtime_error(
            "Exhaustive mode is intentionally capped at K<=4.");
    }

    const uint64_t totalFunctions = uint64_t{1} << (1 << numInputs);
    std::map<LutTruthTable, std::size_t> canonicalFrequency;
    const LutTruthTable mask = truthTableMaskForInputs(numInputs);

    for (uint64_t truth = 0; truth < totalFunctions; ++truth) {
        const NpnRecipe recipe =
            NpnCanonizer::computeCanonical(truth, numInputs);
        ++canonicalFrequency[recipe.canonicalHex & mask];
    }

    std::vector<LibraryFunction> functions;
    functions.reserve(canonicalFrequency.size());
    for (const auto& item : canonicalFrequency) {
        functions.push_back(
            LibraryFunction{
                formatHexTruthTable(item.first),
                numInputs,
                item.second});
    }

    std::sort(
        functions.begin(),
        functions.end(),
        [](const LibraryFunction& lhs, const LibraryFunction& rhs) {
            if (lhs.frequency != rhs.frequency) {
                return lhs.frequency > rhs.frequency;
            }
            return lhs.hexFunc < rhs.hexFunc;
        });

    if (numFunctionsToInclude > 0 &&
        static_cast<int>(functions.size()) > numFunctionsToInclude) {
        functions.resize(numFunctionsToInclude);
    }
    return functions;
}

std::vector<LibraryFunction> SynthesisFlow::buildBenchmarkDrivenFunctionSet(
    const LibraryGenerationConfig& cfg) const {
    if (!cfg.inputCsv.empty() && fs::exists(cfg.inputCsv)) {
        auto functions = loadTopHexFuncsFromCsv(cfg.inputCsv, cfg.lutInputs);
        if (cfg.numFunctionsToInclude > 0 &&
            static_cast<int>(functions.size()) > cfg.numFunctionsToInclude) {
            functions.resize(cfg.numFunctionsToInclude);
        }
        return functions;
    }

    if (cfg.benchmarkDir.empty()) {
        throw std::runtime_error(
            "Benchmark mode requires benchmarkDir or a precomputed CSV.");
    }

    BenchmarkExtractor extractor(abcPath_, cfg.lutInputs);
    extractor.processDirectory(cfg.benchmarkDir);

    if (!cfg.inputCsv.empty()) {
        extractor.exportTopHexFuncs(cfg.inputCsv, cfg.numFunctionsToInclude);
    }

    const auto ranked = extractor.getTopTruthTables(cfg.numFunctionsToInclude);
    std::vector<LibraryFunction> functions;
    functions.reserve(ranked.size());
    for (const auto& item : ranked) {
        functions.push_back(
            LibraryFunction{
                formatHexTruthTable(item.truthTable),
                item.numInputs,
                static_cast<std::size_t>(item.frequency)});
    }
    return functions;
}

std::string SynthesisFlow::probVectorToTag(
    const std::vector<double>& probs) const {
    std::ostringstream oss;
    for (double p : probs) {
        const double stable = ActivityPatternGenerator::stableActivityValue(p);
        const double percent = stable * 100.0;
        const double roundedPercent = std::round(percent);
        if (std::abs(percent - roundedPercent) <=
            ActivityPatternGenerator::kComparisonTolerance * 100.0) {
            oss << "_" << static_cast<int>(roundedPercent);
            continue;
        }

        std::string value = ActivityPatternGenerator::stableValueString(stable);
        while (value.size() > 1 && value.back() == '0') {
            value.pop_back();
        }
        if (!value.empty() && value.back() == '.') {
            value.push_back('0');
        }
        std::replace(value.begin(), value.end(), '.', 'p');
        oss << "_p" << value;
    }
    return oss.str();
}

std::string SynthesisFlow::buildRawBlifFromHexFunc(
    const std::string& hexFunc,
    int numInputs) const {
    const LutTruthTable value =
        hexToTruthTable(hexFunc) & truthTableMaskForInputs(numInputs);

    std::ostringstream out;
    out << ".model raw_" << normalizeHexForInputs(hexFunc, numInputs) << "\n";
    out << ".inputs";
    for (int i = 0; i < numInputs; ++i) {
        out << " i" << i;
    }
    out << "\n";
    out << ".outputs y\n";

    if (value == 0) {
        out << ".names y\n";
        out << ".end\n";
        return out.str();
    }

    if (value == truthTableMaskForInputs(numInputs)) {
        out << ".names y\n";
        out << "1\n";
        out << ".end\n";
        return out.str();
    }

    out << ".names";
    for (int i = 0; i < numInputs; ++i) {
        out << " i" << i;
    }
    out << " y\n";

    const int rows = 1 << numInputs;
    for (int mask = 0; mask < rows; ++mask) {
        if (((value >> mask) & 1ULL) == 0) {
            continue;
        }
        for (int bit = 0; bit < numInputs; ++bit) {
            out << (((mask >> bit) & 1) ? '1' : '0');
        }
        out << " 1\n";
    }

    out << ".end\n";
    return out.str();
}

int SynthesisFlow::countNamesInBlif(const std::string& blifPath) const {
    std::ifstream input(blifPath);
    if (!input.is_open()) {
        return -1;
    }

    std::string line;
    int count = 0;
    while (std::getline(input, line)) {
        if (trimCopy(line).rfind(".names", 0) == 0) {
            ++count;
        }
    }
    return count;
}

bool SynthesisFlow::runSingleABCLocalCase(
    const LibraryFunction& func,
    const std::vector<double>& inputProbs,
    const std::string& outputDir,
    const std::string& tmpBlifPath,
    std::string* csvLineOut) {
    const std::string probTag = probVectorToTag(inputProbs);
    const std::string variantName = func.hexFunc + probTag;

    const fs::path funcDir = fs::path(outputDir) / "detailed_infos" / func.hexFunc;
    fs::create_directories(funcDir);

    fs::path tmpBlif(tmpBlifPath);
    if (tmpBlif.has_parent_path()) {
        fs::create_directories(tmpBlif.parent_path());
    }

    const fs::path rawBlif =
        tmpBlif.parent_path() / (tmpBlif.stem().string() + "_raw.blif");
    const fs::path logPath =
        tmpBlif.parent_path() / (tmpBlif.stem().string() + ".log");
    const fs::path finalBlif = funcDir / ("abc_" + variantName + ".blif");

    {
        std::ofstream rawStream(rawBlif);
        if (!rawStream.is_open()) {
            if (csvLineOut != nullptr) {
                *csvLineOut = func.hexFunc + "," + probTag +
                              ",false,-1,-1,0,Raw BLIF Write Failed\n";
            }
            return false;
        }
        rawStream << buildRawBlifFromHexFunc(func.hexFunc, func.numInputs);
    }

    const auto started = std::chrono::steady_clock::now();
    const std::string abcScript =
        "read_blif " + rawBlif.string() +
        "; strash; dc2; balance; rewrite; balance; rewrite; rewrite -z; "
          "balance; rewrite -z; balance; write_blif " + tmpBlif.string();
    const std::string cmd =
        abcPath_ + " -c \"" + abcScript + "\" > " +
        logPath.string() + " 2>&1";
    const int ret = std::system(cmd.c_str());
    const auto runtimeMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started)
            .count();

    auto emitRow = [&](bool success,
                       int gates,
                       double internalCost,
                       const std::string& error) {
        if (csvLineOut == nullptr) {
            return;
        }
        std::ostringstream row;
        row << func.hexFunc << "," << probTag << ","
            << (success ? "true" : "false") << ","
            << gates << "," << internalCost << ","
            << runtimeMs << "," << error << "\n";
        *csvLineOut = row.str();
    };

    if (ret != 0 || !fs::exists(tmpBlif)) {
        emitRow(false, -1, -1.0, "ABC Failed");
        return false;
    }

    std::ifstream tmpInput(tmpBlif);
    std::string blifContent{
        std::istreambuf_iterator<char>(tmpInput),
        std::istreambuf_iterator<char>()};

    if (!validateBlifImplementsHex(blifContent, func.hexFunc, func.numInputs)) {
        emitRow(false, -1, -1.0, "HexMismatch");
        return false;
    }

    fs::copy_file(tmpBlif, finalBlif, fs::copy_options::overwrite_existing);
    const int gates = countNamesInBlif(finalBlif.string());
    emitRow(true, gates, static_cast<double>(gates), "");
    return true;
}

bool SynthesisFlow::evaluateCubeMatch(const std::string& cube,
                                      int mask,
                                      int nInputs) const {
    for (int i = 0; i < nInputs && i < static_cast<int>(cube.size()); ++i) {
        const char c = cube[i];
        if (c == '-') {
            continue;
        }

        const int bit = (mask >> i) & 1;
        if ((c == '1' && bit == 0) || (c == '0' && bit == 1)) {
            return false;
        }
    }
    return true;
}

double SynthesisFlow::evaluateNamesNodeTruth(const std::vector<std::string>& sop,
                                             int mask,
                                             int nInputs) const {
    if (sop.empty()) {
        return 0.0;
    }

    bool havePolarity = false;
    bool isZeroCover = false;
    for (const std::string& row : sop) {
        std::stringstream ss(row);
        std::string cube;
        std::string outVal;
        if (!(ss >> cube)) {
            continue;
        }
        if (!(ss >> outVal)) {
            if (nInputs == 0 && cube == "1") {
                return 1.0;
            }
            continue;
        }
        isZeroCover = (outVal == "0");
        havePolarity = true;
        break;
    }

    if (!havePolarity) {
        return 0.0;
    }

    for (const std::string& row : sop) {
        std::stringstream ss(row);
        std::string cube;
        std::string outVal;
        if (!(ss >> cube >> outVal)) {
            continue;
        }
        if (!evaluateCubeMatch(cube, mask, nInputs)) {
            continue;
        }
        if (isZeroCover) {
            return outVal == "0" ? 0.0 : 1.0;
        }
        return outVal == "1" ? 1.0 : 0.0;
    }

    return isZeroCover ? 1.0 : 0.0;
}

LutTruthTable SynthesisFlow::computeHexFromBlifFragment(
    const std::string& blifContent,
    int numInputs) const {
    struct NamesNode {
        std::vector<std::string> inputs;
        std::string output;
        std::vector<std::string> sop;
    };

    std::vector<std::string> primaryInputs;
    std::string primaryOutput;
    std::vector<NamesNode> nodes;

    std::stringstream input(blifContent);
    std::string rawLine;
    while (readLogicalLine(input, rawLine)) {
        const std::string line = trimCopy(rawLine);
        if (line.empty() || line[0] == '#') {
            continue;
        }

        if (line.rfind(".inputs", 0) == 0) {
            std::stringstream ss(line.substr(7));
            std::string token;
            while (ss >> token) {
                if (token != "\\") {
                    primaryInputs.push_back(token);
                }
            }
            continue;
        }

        if (line.rfind(".outputs", 0) == 0) {
            std::stringstream ss(line.substr(8));
            ss >> primaryOutput;
            continue;
        }

        if (line.rfind(".names", 0) == 0) {
            std::stringstream ss(line);
            std::string token;
            ss >> token;

            std::vector<std::string> ports;
            while (ss >> token) {
                if (token != "\\") {
                    ports.push_back(token);
                }
            }
            if (ports.empty()) {
                continue;
            }

            NamesNode node;
            node.output = ports.back();
            ports.pop_back();
            node.inputs = std::move(ports);

            while (input.good()) {
                const std::streampos pos = input.tellg();
                std::string sopLine;
                if (!std::getline(input, sopLine)) {
                    break;
                }
                if (!sopLine.empty() && sopLine.back() == '\r') {
                    sopLine.pop_back();
                }
                sopLine = trimCopy(sopLine);
                if (sopLine.empty()) {
                    continue;
                }
                if (sopLine[0] == '.') {
                    input.seekg(pos);
                    break;
                }
                node.sop.push_back(sopLine);
            }

            nodes.push_back(std::move(node));
        }
    }

    if (primaryOutput.empty()) {
        throw std::runtime_error("BLIF fragment has no primary output.");
    }
    if (numInputs < 0 || numInputs > kLutMaxInputs) {
        throw std::runtime_error("Requested K is out of range.");
    }
    if (static_cast<int>(primaryInputs.size()) > numInputs) {
        throw std::runtime_error("BLIF fragment uses more inputs than requested K.");
    }

    LutTruthTable truth = 0;
    const int totalRows = 1 << numInputs;
    for (int fullMask = 0; fullMask < totalRows; ++fullMask) {
        std::map<std::string, double> values;
        for (size_t i = 0; i < primaryInputs.size(); ++i) {
            int sourceIndex = parseInputIndexFromName(primaryInputs[i]);
            if (sourceIndex < 0 || sourceIndex >= numInputs) {
                sourceIndex = static_cast<int>(i);
            }
            values[primaryInputs[i]] = ((fullMask >> sourceIndex) & 1) ? 1.0 : 0.0;
        }

        for (const auto& node : nodes) {
            int localMask = 0;
            for (size_t i = 0; i < node.inputs.size(); ++i) {
                const auto it = values.find(node.inputs[i]);
                const bool bit = (it != values.end() && it->second > 0.5);
                if (bit) {
                    localMask |= (1 << static_cast<int>(i));
                }
            }
            values[node.output] = evaluateNamesNodeTruth(
                node.sop, localMask, static_cast<int>(node.inputs.size()));
        }

        const auto outIt = values.find(primaryOutput);
        if (outIt != values.end() && outIt->second > 0.5) {
            truth |= (LutTruthTable{1} << fullMask);
        }
    }

    return truth & truthTableMaskForInputs(numInputs);
}

bool SynthesisFlow::validateBlifImplementsHex(const std::string& blifContent,
                                              const std::string& expectedHex,
                                              int numInputs) const {
    const LutTruthTable actual =
        computeHexFromBlifFragment(blifContent, numInputs);
    const LutTruthTable expected =
        hexToTruthTable(expectedHex) & truthTableMaskForInputs(numInputs);
    return actual == expected;
}

void SynthesisFlow::debugSingleHexCase(const std::string& hexFunc) {
    const int numInputs = inferNumInputsFromHexWidth(hexFunc);
    fs::create_directories("tmp_hex_debug");

    const fs::path rawPath = fs::path("tmp_hex_debug") / ("raw_" + hexFunc + ".blif");
    const fs::path abcPath = fs::path("tmp_hex_debug") / ("abc_" + hexFunc + ".blif");

    {
        std::ofstream raw(rawPath);
        raw << buildRawBlifFromHexFunc(hexFunc, numInputs);
    }

    const std::string abcScript =
        "read_blif " + rawPath.string() +
        "; strash; dc2; balance; rewrite; balance; rewrite; rewrite -z; "
          "balance; rewrite -z; balance; write_blif " + abcPath.string();
    const std::string cmd = this->abcPath_ + " -c \"" + abcScript + "\"";

    std::cout << "[Debug] raw BLIF: " << rawPath << "\n";
    std::cout << "[Debug] ABC cmd: " << cmd << "\n";
    std::cout << "[Debug] ABC return code: " << std::system(cmd.c_str()) << "\n";

    std::ifstream mapped(abcPath);
    if (!mapped.is_open()) {
        std::cout << "[Debug] No ABC output generated.\n";
        return;
    }

    std::string content{
        std::istreambuf_iterator<char>(mapped),
        std::istreambuf_iterator<char>()};
    std::cout << "[Debug] Implements expected hex: "
              << (validateBlifImplementsHex(content, hexFunc, numInputs)
                      ? "true"
                      : "false")
              << "\n";
}

}  // namespace fes
