#include "fes/utils/InnovusBatchEvaluator.h"
#include "fes/core/NpnTransform.h"
#include "fes/core/Types.h"
#include <fstream>
#include <iostream>
#include <filesystem>
#include <regex>
#include <cmath>
#include <chrono>
#include <set>
#include <map>
#include <unistd.h>
#include <sstream>
#include <vector>
#include <algorithm>
#include <iomanip>
#include <limits>
#include <random>

namespace fes {

// ABC "-K <n> -a" fragment, parameterized by the project-wide LUT constant.
// Used for every `if -K <n> -a` command string we shell out to.
constexpr double kInverterPowerPenalty = 0.001;

static inline std::string abcLutK() {
    return std::to_string(kLutMaxInputs);
}

static inline std::string truthTableHexKey(LutTruthTable tt) {
    std::stringstream hss;
    hss << std::uppercase << std::hex << std::setfill('0')
        << std::setw(kLutTruthTableHexDigits) << tt;
    return hss.str();
}

namespace {

std::string csvEscapeLocal(const std::string& input) {
    const bool needsQuotes =
        input.find_first_of(",\"\r\n") != std::string::npos;
    if (!needsQuotes) return input;

    std::string escaped = "\"";
    for (char c : input) {
        if (c == '"') escaped += "\"\"";
        else escaped += c;
    }
    escaped += "\"";
    return escaped;
}

std::string firstCsvField(const std::string& line) {
    std::string field;
    bool inQuotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (inQuotes) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    field += '"';
                    ++i;
                } else {
                    inQuotes = false;
                }
            } else {
                field += c;
            }
        } else if (c == ',') {
            break;
        } else if (c == '"') {
            inQuotes = true;
        } else {
            field += c;
        }
    }
    return field;
}

double parseCsvDoubleOr(const std::string& text, double fallback) {
    if (text.empty()) return fallback;
    try {
        return std::stod(text);
    } catch (...) {
        return fallback;
    }
}

std::map<std::string, std::string> loadLegacyCsvRows(
    const std::filesystem::path& csvPath) {
    std::map<std::string, std::string> rows;
    std::ifstream input(csvPath);
    if (!input.is_open()) return rows;

    std::string line;
    bool isHeader = true;
    while (std::getline(input, line)) {
        if (isHeader) {
            isHeader = false;
            continue;
        }
        if (line.empty()) continue;
        const std::string key = firstCsvField(line);
        if (!key.empty()) rows[key] = line;
    }
    return rows;
}

void writeLegacyCsvRows(const std::filesystem::path& csvPath,
                        const std::string& header,
                        const std::map<std::string, std::string>& rows) {
    if (csvPath.has_parent_path()) {
        std::filesystem::create_directories(csvPath.parent_path());
    }

    std::ofstream output(csvPath);
    if (!output.is_open()) return;
    output << header << "\n";
    for (const auto& [_, row] : rows) {
        output << row;
        if (row.empty() || row.back() != '\n') output << "\n";
    }
}

void removeDirectoryTree(const std::filesystem::path& path) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        std::filesystem::remove_all(path, ec);
    }
}

std::string standardValidationHeader() {
    return "Benchmark,"
           "Power_Orig(mW),Power_ABC(mW),Power_ABC_Local(mW),Power_PONO(mW),"
           "Gain_Power_vs_ABC(%),Gain_Power_vs_ABC_Local(%),"
           "Area_Orig,Area_ABC,Area_ABC_Local,Area_PONO,"
           "Gain_Area_vs_ABC(%),Gain_Area_vs_ABC_Local(%),"
           "Delay_Orig(ns),Delay_ABC(ns),Delay_ABC_Local(ns),Delay_PONO(ns),"
           "Gain_Delay_vs_ABC(%),Gain_Delay_vs_ABC_Local(%),"
           "Status";
}

std::string formatStandardValidationRow(const PPADiff& r) {
    std::ostringstream out;
    if (!r.success) {
        out << r.fileName << ",,,,,,,,,,,,,,,,,,,FAILED";
        return out.str();
    }

    auto calcGain = [](double base, double opt) {
        return (base > 0) ? (base - opt) / base * 100.0 : 0.0;
    };

    const double pGainAbc =
        calcGain(r.abcHighPPA.power_total, r.ponoPPA.power_total);
    const double pGainAbcLocal =
        calcGain(r.abcLocalPPA.power_total, r.ponoPPA.power_total);
    const double aGainAbc = calcGain(r.abcHighPPA.area, r.ponoPPA.area);
    const double aGainAbcLocal =
        calcGain(r.abcLocalPPA.area, r.ponoPPA.area);
    const double dGainAbc = calcGain(r.abcHighPPA.delay, r.ponoPPA.delay);
    const double dGainAbcLocal =
        calcGain(r.abcLocalPPA.delay, r.ponoPPA.delay);

    out << r.fileName << ","
        << r.origPPA.power_total << "," << r.abcHighPPA.power_total << ","
        << r.abcLocalPPA.power_total << ","
        << r.ponoPPA.power_total << ","
        << std::fixed << std::setprecision(2) << pGainAbc << "%,"
        << pGainAbcLocal << "%,"
        << r.origPPA.area << "," << r.abcHighPPA.area << ","
        << r.abcLocalPPA.area << ","
        << r.ponoPPA.area << ","
        << aGainAbc << "%,"
        << aGainAbcLocal << "%,"
        << r.origPPA.delay << "," << r.abcHighPPA.delay << ","
        << r.abcLocalPPA.delay << ","
        << r.ponoPPA.delay << ","
        << dGainAbc << "%,"
        << dGainAbcLocal << "%,"
        << "SUCCESS";
    return out.str();
}

std::string benchmarkCaseId(const std::string& filePath,
                            const std::string& benchmarksDir) {
    namespace fs = std::filesystem;
    try {
        fs::path rel = fs::relative(fs::path(filePath), fs::path(benchmarksDir));
        if (!rel.empty() && rel.native().find("..") != 0) {
            return rel.generic_string();
        }
    } catch (...) {
    }
    return fs::path(filePath).filename().string();
}

RunStatus evaluationStatus(bool success, double runtimeMs, int timeoutMs) {
    if (timeoutMs > 0 && runtimeMs > static_cast<double>(timeoutMs)) {
        return RunStatus::kTimeout;
    }
    return success ? RunStatus::kSuccess : RunStatus::kFailed;
}

std::string standardFailureReason(const PPADiff& diff) {
    if (diff.success) return "";
    if (!diff.origPPA.valid) return "Original evaluation failed";
    if (!diff.abcHighPPA.valid) return "ABC global evaluation failed";
    if (!diff.abcLocalPPA.valid) return "ABC local evaluation failed";
    if (!diff.ponoPPA.valid) return "PONO evaluation failed";
    return "Evaluation failed";
}

std::string timeoutReason(double runtimeMs, int timeoutMs) {
    std::ostringstream out;
    out << "Runtime " << std::fixed << std::setprecision(3) << runtimeMs
        << " ms exceeded case timeout " << timeoutMs << " ms";
    return out.str();
}

std::string runStatusForSummary(const RunSummary& summary) {
    if (summary.timeoutCases > 0) return "timeout";
    if (summary.failedCases > 0) return "failed";
    if (summary.successCases > 0 || summary.skippedCases > 0) return "success";
    return "skipped";
}

void writeEvaluationSummaryJson(const std::filesystem::path& path,
                                const RunSummary& summary,
                                const std::filesystem::path& casesCsv,
                                const std::filesystem::path& validationCsv) {
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }

    std::ofstream out(path);
    if (!out.is_open()) return;

    out << "{\n";
    out << "  \"command\": \"" << escapeJsonString(summary.command) << "\",\n";
    out << "  \"mode\": \"" << escapeJsonString(summary.activityMode) << "\",\n";
    out << "  \"status\": \"" << runStatusForSummary(summary) << "\",\n";
    out << "  \"total_cases\": " << summary.totalCases << ",\n";
    out << "  \"run_cases\": " << summary.runCases << ",\n";
    out << "  \"skipped_cases\": " << summary.skippedCases << ",\n";
    out << "  \"resumed_cases\": " << summary.resumedCases << ",\n";
    out << "  \"success_cases\": " << summary.successCases << ",\n";
    out << "  \"failed_cases\": " << summary.failedCases << ",\n";
    out << "  \"timeout_cases\": " << summary.timeoutCases << ",\n";
    out << "  \"validation_csv\": \""
        << escapeJsonString(validationCsv.string()) << "\",\n";
    out << "  \"cases_csv\": \"" << escapeJsonString(casesCsv.string()) << "\",\n";
    out << "  \"manifest_csv\": \""
        << escapeJsonString(summary.manifestCsv.string()) << "\"\n";
    out << "}\n";
}

struct EvaluationCaseRecord {
    std::string caseId;
    std::string benchmark;
    std::string status;
    std::string reason;
    std::string selectedStrategy;
    std::string outputCsv;
    double runtimeMs = 0.0;
    bool origValid = false;
    bool abcLocalValid = false;
    bool abcValid = false;
    bool ponoValid = false;
    PPAResult origPPA;
    PPAResult abcLocalPPA;
    PPAResult abcPPA;
    PPAResult ponoPPA;
};

void writeEvaluationCasesCsv(
    const std::filesystem::path& path,
    const std::vector<EvaluationCaseRecord>& records) {
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }

    std::ofstream out(path);
    if (!out.is_open()) return;

    out << "case_id,benchmark,status,reason,runtime_ms,"
        << "selected_strategy,output_csv,"
        << "orig_power,orig_area,orig_delay,"
        << "abc_local_power,abc_local_area,abc_local_delay,"
        << "abc_power,abc_area,abc_delay,"
        << "pono_power,pono_area,pono_delay\n";

    auto writePpa = [&](const PPAResult& ppa, bool valid) {
        if (valid) {
            out << ppa.power_total << "," << ppa.area << "," << ppa.delay;
        } else {
            out << "NA,NA,NA";
        }
    };

    for (const auto& r : records) {
        out << csvEscapeLocal(r.caseId) << ","
            << csvEscapeLocal(r.benchmark) << ","
            << csvEscapeLocal(r.status) << ","
            << csvEscapeLocal(r.reason) << ","
            << std::fixed << std::setprecision(3) << r.runtimeMs << ","
            << csvEscapeLocal(r.selectedStrategy) << ","
            << csvEscapeLocal(r.outputCsv) << ",";
        writePpa(r.origPPA, r.origValid);
        out << ",";
        writePpa(r.abcLocalPPA, r.abcLocalValid);
        out << ",";
        writePpa(r.abcPPA, r.abcValid);
        out << ",";
        writePpa(r.ponoPPA, r.ponoValid);
        out << "\n";
    }
}

}  // namespace

InnovusBatchEvaluator::InnovusBatchEvaluator(const std::string& lib, const std::string& py, const std::string& abc)
    : libPath_(lib), verifier_(py), abcPath_(abc) {
    outputRootPath_ = std::filesystem::path(libPath_);
    workDir_ = outputRootPath_ / "tmp_eval";
    std::filesystem::create_directories(workDir_);
    cec_ = std::make_unique<EquivalenceChecker>(cecLibrary_);
    loadOptimizationLibrary();
}

void InnovusBatchEvaluator::setOutputRootDir(const std::filesystem::path& outputRoot) {
    outputRootPath_ = outputRoot.empty() ? std::filesystem::path(libPath_)
                                         : outputRoot;
    workDir_ = outputRootPath_ / "tmp_eval";
}

std::vector<std::string> InnovusBatchEvaluator::findBlifFilesRecursive(const std::filesystem::path& folderPath) {
    std::vector<std::string> blifFiles;
    if (!std::filesystem::exists(folderPath)) return blifFiles;

    for (const auto& entry : std::filesystem::recursive_directory_iterator(folderPath)) {
        if (entry.is_regular_file() && entry.path().extension() == ".blif") {
            blifFiles.push_back(entry.path().string());
        }
    }
    std::sort(blifFiles.begin(), blifFiles.end());
    return blifFiles;
}

int countBlifInputs(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) return 0;

    std::string line;
    bool inInputSection = false;
    int inputCount = 0;

    while (std::getline(ifs, line)) {
        size_t first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos || line[first] == '#') continue;
        std::string trimmed = line.substr(first);

        if (trimmed.compare(0, 7, ".inputs") == 0) {
            inInputSection = true;
            trimmed = trimmed.substr(7); 
        }

        if (inInputSection) {
            bool hasContinuation = false;
            size_t backslashPos = trimmed.find('\\');
            if (backslashPos != std::string::npos) {
                hasContinuation = true;
                trimmed = trimmed.substr(0, backslashPos); 
            }

            std::stringstream ss(trimmed);
            std::string pinName;
            while (ss >> pinName) {
                if (!pinName.empty()) inputCount++;
            }

            if (!hasContinuation) break; 
        }
    }
    ifs.close();
    return inputCount;
}

void InnovusBatchEvaluator::runBatchVerification(const std::string& benchmarksDir) {
    namespace fs = std::filesystem;
    auto files = findBlifFilesRecursive(benchmarksDir);
    std::vector<EvaluationCaseRecord> caseRecords;

    const fs::path outputRoot =
        outputRootPath_.empty() ? fs::path(libPath_) : outputRootPath_;
    const fs::path validationCsv = outputRoot / "ppa_complete_validation.csv";
    const fs::path manifestCsv = outputRoot / "evaluation_manifest.csv";
    const fs::path casesCsv = outputRoot / "evaluation_cases.csv";
    const fs::path summaryJson = outputRoot / "evaluation_summary.json";

    fs::create_directories(outputRoot);
    fs::create_directories(workDir_);

    RunManifest manifest(manifestCsv);
    manifest.load();
    auto legacyRows = loadLegacyCsvRows(validationCsv);

    RunSummary summary;
    summary.command = "evaluate";
    summary.activityMode = "standard";
    summary.outputCsv = validationCsv;
    summary.manifestCsv = manifestCsv;
    summary.totalCases = files.size();

    std::cout << "\n>>> Phase 3: Unified Four-Method Physical Validation <<<" << std::endl;

    for (size_t i = 0; i < files.size(); ++i) {
        std::string filePath = files[i];
        std::string fileName = fs::path(filePath).filename().string();
        const std::string benchmarkId = benchmarkCaseId(filePath, benchmarksDir);
        std::string caseId = "standard:" + benchmarkId;

        const bool shouldRun = manifest.shouldRun(caseId, resumePolicy_);
        const bool isResumeAttempt =
            manifest.isResumeAttempt(caseId, resumePolicy_);
        
        std::cout << "====================================================" << std::endl;
        std::cout << "[" << (i+1) << "/" << files.size() << "] Benchmark: " << fileName << std::endl;

        if (!shouldRun) {
            const RunManifestEntry* previous = manifest.find(caseId);
            const std::string previousStatus =
                previous ? runStatusToString(previous->status) : "unknown";
            std::cout << "  >>> Result: SKIPPED (resume policy, previous status="
                      << previousStatus << ")" << std::endl << std::endl;
            ++summary.skippedCases;

            EvaluationCaseRecord record;
            record.caseId = caseId;
            record.benchmark = fileName;
            record.status = runStatusToString(RunStatus::kSkipped);
            record.reason = "Skipped by resume policy; previous status=" +
                            previousStatus;
            record.outputCsv = validationCsv.string();
            caseRecords.push_back(record);
            continue;
        }

        if (isResumeAttempt) {
            ++summary.resumedCases;
            RunManifestEntry resumedEntry;
            resumedEntry.caseId = caseId;
            resumedEntry.kind = "evaluate";
            resumedEntry.functionId = benchmarkId;
            resumedEntry.activityMode = "standard";
            resumedEntry.status = RunStatus::kResumed;
            resumedEntry.outputPath = validationCsv.string();
            resumedEntry.reason = "Resumed by policy " +
                                  resumePolicyToString(resumePolicy_);
            manifest.update(resumedEntry);
            manifest.flush();
        }
        ++summary.runCases;

        PPADiff diff;
        diff.fileName = fileName;
        const auto caseStart = std::chrono::steady_clock::now();
        
        int inputNum = countBlifInputs(filePath);
        auto workloads = verifier_.generateWorkloadSets(inputNum, 1, 1000);
        const WorkloadSet workload =
            workloads.empty() ? WorkloadSet{} : workloads.front();
        std::vector<double> avgProbs = workload.probs;
        if (avgProbs.size() < static_cast<size_t>(inputNum)) {
            avgProbs.resize(inputNum, 0.5);
        }

        auto printPPA = [](const std::string& label, const PPAResult& res) {
            if (res.valid) {
                std::cout << "    [" << label << "] Power: " << std::fixed << std::setprecision(3) << res.power_total << " mW, "
                          << "Area: " << res.area << ", Delay: " << res.delay << " ns" << std::endl;
            } else {
                std::cout << "    [" << label << "] Evaluation FAILED." << std::endl;
            }
        };
        auto evalOnce = [&](const std::string& blifPath) {
            return verifier_.getPPAResult(
                blifPath, workload.probs, workload.acts);
        };

        std::cout << "  - Evaluating Original..." << std::endl;
        diff.origPPA = evalOnce(filePath);
        printPPA("ORIG", diff.origPPA);

        std::cout << "  - Running ABC Script..." << std::endl;
        std::string abcHigh = runABCExhaustiveOpt(filePath); 
        if (!abcHigh.empty() && std::filesystem::exists(abcHigh)) {
            diff.abcHighPPA = evalOnce(abcHigh);
            printPPA("ABC ", diff.abcHighPPA);
        }

        std::cout << "  - Running ABC Local-Library Rewrite..." << std::endl;
        std::string mappedOrigin = run4LutMappingOnly(filePath);
        if (!mappedOrigin.empty() && std::filesystem::exists(mappedOrigin)) {
            const std::string abcLocalBlif =
                rewriteMappedBlifWithABCLibrarySimple(mappedOrigin, avgProbs);
            if (!abcLocalBlif.empty() && std::filesystem::exists(abcLocalBlif)) {
                diff.abcLocalPPA = evalOnce(abcLocalBlif);
                printPPA("ABC-L", diff.abcLocalPPA);
            }
        }

        std::cout << "  - Running PONO Optimization (Tournament Mode)..." << std::endl;
        
        // 引擎 A：激进模式 (Aggressive)
        std::string ponoAggBlif = rewriteBlifWithLibrary(filePath, avgProbs, true); 
        PPAResult aggPPA; aggPPA.valid = false;
        if (!ponoAggBlif.empty() && std::filesystem::exists(ponoAggBlif)) {
            aggPPA = evalOnce(ponoAggBlif);
        }

        // 引擎 B：保守模式 (Conservative)
        std::string ponoConsBlif = rewriteBlifWithLibrary(filePath, avgProbs, false);
        PPAResult consPPA; consPPA.valid = false;
        if (!ponoConsBlif.empty() && std::filesystem::exists(ponoConsBlif)) {
            consPPA = evalOnce(ponoConsBlif);
        }

        // 锦标赛决断：不看 ORIG，只选内部最优
        diff.ponoPPA.valid = false;
        std::string winningStrategy = "NONE";
        
        if (aggPPA.valid || consPPA.valid) {
            double pAgg = aggPPA.valid ? aggPPA.power_total : 1e9;
            double pCons = consPPA.valid ? consPPA.power_total : 1e9;
            
            if (pAgg <= pCons) {
                diff.ponoPPA = aggPPA;
                winningStrategy = "Aggressive";
            } else {
                diff.ponoPPA = consPPA;
                winningStrategy = "Conservative";
            }
            printPPA("PONO", diff.ponoPPA);
            std::cout << "    [Strategy Selected] " << winningStrategy << std::endl;
        }

        diff.success = (diff.origPPA.valid && diff.abcHighPPA.valid &&
                        diff.abcLocalPPA.valid && diff.ponoPPA.valid);
        legacyRows[fileName] = formatStandardValidationRow(diff);
        writeLegacyCsvRows(validationCsv, standardValidationHeader(), legacyRows);

        const auto caseEnd = std::chrono::steady_clock::now();
        const double runtimeMs =
            std::chrono::duration<double, std::milli>(caseEnd - caseStart)
                .count();
        const RunStatus status =
            evaluationStatus(diff.success, runtimeMs, caseTimeoutMs_);
        const std::string reason =
            status == RunStatus::kTimeout
                ? timeoutReason(runtimeMs, caseTimeoutMs_)
                : standardFailureReason(diff);

        if (status == RunStatus::kSuccess) {
            ++summary.successCases;
        } else if (status == RunStatus::kTimeout) {
            ++summary.timeoutCases;
        } else {
            ++summary.failedCases;
        }

        RunManifestEntry manifestEntry;
        manifestEntry.caseId = caseId;
        manifestEntry.kind = "evaluate";
        manifestEntry.functionId = benchmarkId;
        manifestEntry.activityMode = "standard";
        manifestEntry.status = status;
        manifestEntry.runtimeMs = runtimeMs;
        manifestEntry.outputPath = validationCsv.string();
        manifestEntry.reason = reason;
        manifest.update(manifestEntry);
        manifest.flush();

        EvaluationCaseRecord record;
        record.caseId = caseId;
        record.benchmark = fileName;
        record.status = runStatusToString(status);
        record.reason = reason;
        record.runtimeMs = runtimeMs;
        record.selectedStrategy = winningStrategy;
        record.outputCsv = validationCsv.string();
        record.origValid = diff.origPPA.valid;
        record.origPPA = diff.origPPA;
        record.abcLocalValid = diff.abcLocalPPA.valid;
        record.abcLocalPPA = diff.abcLocalPPA;
        record.abcValid = diff.abcHighPPA.valid;
        record.abcPPA = diff.abcHighPPA;
        record.ponoValid = diff.ponoPPA.valid;
        record.ponoPPA = diff.ponoPPA;
        caseRecords.push_back(record);

        if (diff.success) {
            double pGainABC = (diff.abcHighPPA.power_total > 0) ? (diff.abcHighPPA.power_total - diff.ponoPPA.power_total) / diff.abcHighPPA.power_total * 100.0 : 0.0;
            double pGainABCLocal = (diff.abcLocalPPA.power_total > 0) ? (diff.abcLocalPPA.power_total - diff.ponoPPA.power_total) / diff.abcLocalPPA.power_total * 100.0 : 0.0;
            double pGainOrig = (diff.origPPA.power_total > 0) ? (diff.origPPA.power_total - diff.ponoPPA.power_total) / diff.origPPA.power_total * 100.0 : 0.0;
            
            std::cout << "  >>> Result: SUCCESS" << std::endl;
            std::cout << "      Net Power Gain vs ABC: " << std::fixed << std::setprecision(2) << pGainABC << "%" << std::endl;
            std::cout << "      Net Power Gain vs ABC Local: " << std::fixed << std::setprecision(2) << pGainABCLocal << "%" << std::endl;
            std::cout << "      Total Power Gain vs Orig: " << std::fixed << std::setprecision(2) << pGainOrig << "%" << std::endl;
        } else {
            std::cout << "  >>> Result: FAILED" << std::endl;
        }
        if (status == RunStatus::kTimeout) {
            std::cout << "      Manifest status: TIMEOUT (" << reason << ")" << std::endl;
        }
        std::cout << std::endl;
    }

    writeLegacyCsvRows(validationCsv, standardValidationHeader(), legacyRows);
    writeEvaluationCasesCsv(casesCsv, caseRecords);
    writeEvaluationSummaryJson(summaryJson, summary, casesCsv, validationCsv);
    removeDirectoryTree(workDir_);
}

SingleBlifResult InnovusBatchEvaluator::analyzeSingleBlif(
    const std::string& inputBlifPath,
    const std::vector<double>& inputProbs,
    const std::vector<double>& inputActs,
    const std::filesystem::path& optimizedBlifOutputPath,
    bool includeOptimizedBlifContent) {
    namespace fs = std::filesystem;

    SingleBlifResult result;
    result.sourceBlifPath = inputBlifPath;
    result.inputProbs = inputProbs;
    result.inputActs = inputActs;

    const auto cleanupWorkDir = [&]() {
        removeDirectoryTree(workDir_);
    };
    const auto readWholeFile = [](const fs::path& path) {
        std::ifstream input(path);
        if (!input.is_open()) {
            return std::string{};
        }
        return std::string((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    };

    const auto start = std::chrono::steady_clock::now();
    try {
        if (inputBlifPath.empty() || !fs::exists(inputBlifPath)) {
            result.errorMessage = "Input BLIF file not found.";
        } else if (inputProbs.empty()) {
            result.errorMessage = "inputProbs must not be empty.";
        } else if (inputProbs.size() != inputActs.size()) {
            result.errorMessage =
                "inputProbs and inputActs must have the same length.";
        } else {
            fs::create_directories(workDir_);

            result.origPPA = verifier_.getPPAResult(inputBlifPath, inputProbs, inputActs);

            std::string abcHighBlif;
            if (result.origPPA.valid) {
                abcHighBlif = runABCExhaustiveOpt(inputBlifPath);
                if (!abcHighBlif.empty() && fs::exists(abcHighBlif)) {
                    result.abcHighPPA =
                        verifier_.getPPAResult(abcHighBlif, inputProbs, inputActs);
                }

                const std::string mappedOrigin = run4LutMappingOnly(inputBlifPath);
                if (!mappedOrigin.empty() && fs::exists(mappedOrigin)) {
                    const std::string abcLocalBlif =
                        rewriteMappedBlifWithABCLibrarySimple(mappedOrigin, inputProbs);
                    if (!abcLocalBlif.empty() && fs::exists(abcLocalBlif)) {
                        result.abcLocalPPA =
                            verifier_.getPPAResult(abcLocalBlif, inputProbs, inputActs);
                    }
                }

                const std::string ponoAggBlif =
                    rewriteBlifWithLibrary(inputBlifPath, inputProbs, true);
                PPAResult aggPPA;
                if (!ponoAggBlif.empty() && fs::exists(ponoAggBlif)) {
                    aggPPA = verifier_.getPPAResult(
                        ponoAggBlif, inputProbs, inputActs);
                }

                const std::string ponoConsBlif =
                    rewriteBlifWithLibrary(inputBlifPath, inputProbs, false);
                PPAResult consPPA;
                if (!ponoConsBlif.empty() && fs::exists(ponoConsBlif)) {
                    consPPA = verifier_.getPPAResult(
                        ponoConsBlif, inputProbs, inputActs);
                }

                std::string winningBlifPath;
                double pAgg = aggPPA.valid ? aggPPA.power_total : 1e18;
                double pCons = consPPA.valid ? consPPA.power_total : 1e18;
                if (aggPPA.valid || consPPA.valid) {
                    if (pAgg <= pCons) {
                        result.ponoPPA = aggPPA;
                        result.selectedStrategy = "Aggressive";
                        winningBlifPath = ponoAggBlif;
                    } else {
                        result.ponoPPA = consPPA;
                        result.selectedStrategy = "Conservative";
                        winningBlifPath = ponoConsBlif;
                    }
                }

                if (!winningBlifPath.empty() && fs::exists(winningBlifPath)) {
                    if (!optimizedBlifOutputPath.empty()) {
                        if (optimizedBlifOutputPath.has_parent_path()) {
                            fs::create_directories(
                                optimizedBlifOutputPath.parent_path());
                        }
                        fs::copy_file(winningBlifPath,
                                      optimizedBlifOutputPath,
                                      fs::copy_options::overwrite_existing);
                        result.optimizedBlifPath =
                            fs::absolute(optimizedBlifOutputPath).string();
                    }
                    if (includeOptimizedBlifContent) {
                        const fs::path contentPath = optimizedBlifOutputPath.empty()
                                                         ? fs::path(winningBlifPath)
                                                         : optimizedBlifOutputPath;
                        result.optimizedBlifContent = readWholeFile(contentPath);
                    }
                }
            }

            result.success = result.origPPA.valid && result.abcHighPPA.valid &&
                             result.abcLocalPPA.valid && result.ponoPPA.valid;
            if (!result.success && result.errorMessage.empty()) {
                PPADiff diff;
                diff.origPPA = result.origPPA;
                diff.abcHighPPA = result.abcHighPPA;
                diff.abcLocalPPA = result.abcLocalPPA;
                diff.ponoPPA = result.ponoPPA;
                diff.success = false;
                result.errorMessage = standardFailureReason(diff);
            }
        }
    } catch (const std::exception& e) {
        if (result.errorMessage.empty()) {
            result.errorMessage = e.what();
        }
    }

    const auto end = std::chrono::steady_clock::now();
    result.runtimeMs =
        std::chrono::duration<double, std::milli>(end - start).count();
    cleanupWorkDir();
    return result;
}

void InnovusBatchEvaluator::loadOptimizationLibrary() {
    namespace fs = std::filesystem;
    std::string csvPath = libPath_ + "/final_results.csv";
    std::string infoDirPath = libPath_ + "/detailed_infos";

    std::cout << "[Batch] Loading Library from: " << libPath_ << std::endl;

    std::ifstream file(csvPath);
    if (!file.is_open()) {
        std::cerr << "[Error] Could not open library index: " << csvPath << std::endl;
        return;
    }

    hexMappingLib_.clear();

    std::string line;
    bool isHeader = true;

    while (std::getline(file, line)) {
        if (isHeader) { isHeader = false; continue; } 
        if (line.find("Verification Failed") != std::string::npos || 
            line.find("Timeout") != std::string::npos) {
            continue;
        }

        std::stringstream ss(line);
        std::string hexFunc, probPattern, successStr, gatesStr, internalCostStr;
        
        std::getline(ss, hexFunc, ',');
        std::getline(ss, probPattern, ',');
        std::getline(ss, successStr, ',');
        std::getline(ss, gatesStr, ',');
        std::getline(ss, internalCostStr, ',');

        if (successStr == "true") {
            std::string fullFuncName = hexFunc + probPattern;
            fs::path blifFilePath = fs::path(infoDirPath) / hexFunc / ("pono_" + fullFuncName + ".blif");

            if (fs::exists(blifFilePath)) {
                std::ifstream bsf(blifFilePath);
                std::string content((std::istreambuf_iterator<char>(bsf)), (std::istreambuf_iterator<char>()));

                LibEntry entry;
                entry.blifContent = content;
                entry.score = parseCsvDoubleOr(
                    internalCostStr, std::numeric_limits<double>::infinity());

                std::vector<double> ideals;
                std::stringstream pSs(probPattern);
                std::string token;
                while (std::getline(pSs, token, '_')) {
                    if (!token.empty()) {
                        ideals.push_back(std::stod(token) / 100.0);
                    }
                }
                while (ideals.size() < static_cast<size_t>(kLutMaxInputs))
                    ideals.push_back(0.5);
                entry.idealActivities = ideals;

                hexMappingLib_[hexFunc].push_back(entry);
            }
        }
    }
    std::cout << "[Batch] Library loaded. Total unique HexFuncs: " << hexMappingLib_.size() << std::endl;
}

std::string InnovusBatchEvaluator::runABCExhaustiveOpt(const std::string& inputBlif) {
    namespace fs = std::filesystem;
    
    fs::path workDir = workDir_;
    if (!fs::exists(workDir)) fs::create_directories(workDir);

    std::string baseName = fs::path(inputBlif).stem().string();
    std::string outPath = (workDir / (baseName + "_abc_high.blif")).string();
    std::string logPath = (workDir / (baseName + "_abc_baseline.log")).string();

    // 弃用会造成面积膨胀的 balance，改用面积严格驱动的 strash + dc2 + resyn2a 组合
    std::string highOptSeq = "strash; dc2; balance; rewrite; balance; rewrite; "
                             "rewrite -z; balance; rewrite -z; balance; "
                             "if -K " + abcLutK() + " -a";
    std::string abcCmd = abcPath_ + " -c \"read_blif " + inputBlif +
                         "; " + highOptSeq + "; write_blif " + outPath +
                         "\" > " + logPath + " 2>&1";

    int ret = system(abcCmd.c_str());
    if (ret != 0 || !fs::exists(outPath)) return "";
    return verifyRewriteOrRevert(inputBlif, outPath, "ABC_exhaustive");
}

bool readLineSafe(std::ifstream& ifs, std::string& outLine) {
    if (!std::getline(ifs, outLine)) return false;
    while (!outLine.empty()) {
        size_t last = outLine.find_last_not_of(" \r\n\t");
        if (last != std::string::npos && outLine[last] == '\\') {
            outLine.erase(last);
            std::string nextPart;
            if (std::getline(ifs, nextPart)) {
                outLine += " " + nextPart;
                continue;
            }
        }
        break;
    }
    return true;
}

bool readLineSafe(std::istream& is, std::string& outLine) {
    if (!std::getline(is, outLine)) return false;
    while (!outLine.empty()) {
        size_t last = outLine.find_last_not_of(" \r\n\t");
        if (last != std::string::npos && outLine[last] == '\\') {
            outLine.erase(last);
            std::string nextPart;
            if (std::getline(is, nextPart)) {
                outLine += " " + nextPart;
                continue;
            }
        }
        break;
    }
    return true;
}

// Compute the canonical K-input truth table (LSB = minterm 0). The truth
// table width, row count, and cover-mask are all derived from
// kLutMaxInputs, so scaling to 6-input LUTs requires no changes here.
LutTruthTable getHexValue(const std::vector<std::string>& sop, int numInputs) {
    if (sop.empty()) return 0;
    std::stringstream ss(sop[0]);
    std::string firstCube, firstOut;
    ss >> firstCube >> firstOut;
    bool isCover0 = (firstOut == "0");

    LutTruthTable truthTable = isCover0 ? kLutTruthTableAllOnes : LutTruthTable{0};

    const int rows = 1 << numInputs;   // 2^numInputs, clamped by the caller
    for (const auto& row : sop) {
        std::stringstream rss(row);
        std::string cube, out;
        if (!(rss >> cube >> out)) continue;

        for (int i = 0; i < rows; ++i) {
            bool match = true;
            for (int j = 0; j < numInputs; ++j) {
                if (cube[j] == '-') continue;
                bool bitValue = ((i >> j) & 1) != 0;
                if ((cube[j] == '1' && !bitValue) || (cube[j] == '0' && bitValue)) {
                    match = false; break;
                }
            }
            if (match) {
                const LutTruthTable bit = LutTruthTable{1} << i;
                if (isCover0) truthTable &= ~bit;
                else          truthTable |= bit;
            }
        }
    }
    return truthTable;
}

// ============================================================================
// 辅助函数 1: computeSopOutputProb
// 计算一个 .names SOP 在给定输入概率下的输出为 1 的概率
// ============================================================================
double computeSopOutputProb(
    const std::vector<std::string>& sop,
    const std::vector<double>& inProbs)
{
    int cnt0 = 0, cnt1 = 0;
    for (const auto& row : sop) {
        if (row.size() < 2) continue;
        char v = row.back();
        if (v == '0') cnt0++;
        else if (v == '1') cnt1++;
    }
    bool isCover0 = (cnt0 > cnt1);
    char targetOut = isCover0 ? '0' : '1';

    int n = (int)inProbs.size();
    if (n == 0) return 0.5;

    // 小输入数时精确枚举
    if (n <= 14) {
        double probCovered = 0.0;
        int total = (1 << n);
        for (int mask = 0; mask < total; ++mask) {
            double mintermProb = 1.0;
            for (int i = 0; i < n; ++i) {
                mintermProb *= ((mask >> i) & 1) ? inProbs[i] : (1.0 - inProbs[i]);
            }

            bool covered = false;
            for (const auto& row : sop) {
                std::string cube, outVal;
                std::stringstream rss(row);
                if (!(rss >> cube >> outVal)) continue;
                if (outVal.empty() || outVal[0] != targetOut) continue;

                bool match = true;
                for (int i = 0; i < (int)cube.size() && i < n; ++i) {
                    if (cube[i] == '1' && !((mask >> i) & 1)) { match = false; break; }
                    if (cube[i] == '0' &&  ((mask >> i) & 1)) { match = false; break; }
                }
                if (match) {
                    covered = true;
                    break;
                }
            }

            if (covered) probCovered += mintermProb;
        }
        return isCover0 ? (1.0 - probCovered) : probCovered;
    }

    // 大输入数时独立立方体近似
    double probNotCovered = 1.0;
    for (const auto& row : sop) {
        std::string cube, outVal;
        std::stringstream rss(row);
        if (!(rss >> cube >> outVal)) continue;
        if (outVal.empty() || outVal[0] != targetOut) continue;

        double cubeProb = 1.0;
        for (size_t i = 0; i < cube.size() && i < inProbs.size(); ++i) {
            if (cube[i] == '1') cubeProb *= inProbs[i];
            else if (cube[i] == '0') cubeProb *= (1.0 - inProbs[i]);
        }
        probNotCovered *= (1.0 - cubeProb);
    }
    return isCover0 ? probNotCovered : (1.0 - probNotCovered);
}

// ============================================================================
// 辅助函数 2: estimateFragmentSwitching
// 在给定输入概率下，传播候选 BLIF 片段的信号概率，估算内部翻转
// ============================================================================

FragmentPowerInfo estimateFragmentSwitching(
    const std::string& blifContent,
    const std::vector<double>& inputProbs)
{
    FragmentPowerInfo info{0.0, 0, 0.0};

    std::vector<std::string> libInputNames;
    std::string libOutputName;

    {
        std::stringstream ss(blifContent);
        std::string line;
        while (std::getline(ss, line)) {
            if (line.compare(0, 7, ".inputs") == 0) {
                std::stringstream si(line.substr(7));
                std::string p;
                while (si >> p) {
                    if (p != "\\" && !p.empty()) libInputNames.push_back(p);
                }
            } else if (line.compare(0, 8, ".outputs") == 0) {
                std::stringstream so(line.substr(8));
                std::string p;
                while (so >> p) {
                    if (p != "\\" && !p.empty()) libOutputName = p;
                }
            }
        }
    }

    std::map<std::string, double> sigProb;
    for (size_t i = 0; i < libInputNames.size() && i < inputProbs.size(); ++i) {
        sigProb[libInputNames[i]] = inputProbs[i];
    }

    std::stringstream ss(blifContent);
    std::string line;
    std::string lastOutName;

    while (std::getline(ss, line)) {
        if (line.compare(0, 6, ".names") != 0) continue;

        std::stringstream ls(line);
        std::string tag, wire;
        std::vector<std::string> ports;
        ls >> tag;
        while (ls >> wire) {
            if (wire != "\\" && !wire.empty()) ports.push_back(wire);
        }

        if (ports.empty()) continue;
        std::string outName = ports.back();
        ports.pop_back();

        std::vector<std::string> sop;
        while (ss.peek() != EOF) {
            std::streampos pos = ss.tellg();
            std::string sopLine;
            if (!std::getline(ss, sopLine)) break;
            if (sopLine.empty()) continue;
            if (sopLine[0] == '.') {
                ss.seekg(pos);
                break;
            }
            sop.push_back(sopLine);
        }

        std::vector<double> gateInProbs;
        gateInProbs.reserve(ports.size());
        for (const auto& p : ports) {
            gateInProbs.push_back(sigProb.count(p) ? sigProb[p] : 0.5);
        }

        double outP = computeSopOutputProb(sop, gateInProbs);
        outP = std::clamp(outP, 0.0, 1.0);
        sigProb[outName] = outP;

        double toggle = 2.0 * outP * (1.0 - outP);
        info.totalSwitching += toggle;
        info.gateCount++;
        lastOutName = outName;
    }

    if (!libOutputName.empty() && sigProb.count(libOutputName)) {
        double p = sigProb[libOutputName];
        info.outputToggle = 2.0 * p * (1.0 - p);
    } else if (!lastOutName.empty() && sigProb.count(lastOutName)) {
        double p = sigProb[lastOutName];
        info.outputToggle = 2.0 * p * (1.0 - p);
    }

    return info;
}

// ============================================================================
// Legacy PONO aggressive/conservative engine.
// Thin adapter over rewriteBlifUnified: enables the internal LUT4 pre-map,
// drops the improvement threshold, and tunes the gate-weight knob.
// ============================================================================
std::string fes::InnovusBatchEvaluator::rewriteBlifWithLibrary(
    const std::string& originalBlifPath,
    const std::vector<double>& actualProbs,
    bool isAggressive)
{
    RewriteConfig cfg;
    cfg.preMapAbcSeq         = "strash; if -K " + abcLutK() + " -a";
    cfg.kSwitchWeight        = isAggressive ? 0.25 : 0.35;
    cfg.kGateWeight          = isAggressive ? 0.015 : 0.030;
    cfg.kOutputWeight        = isAggressive ? 0.05 : 0.10;
    cfg.kActivityWeight      = isAggressive ? 0.02 : 0.04;
    cfg.kLibraryScoreWeight  = isAggressive ? 0.03 : 0.06;
    cfg.enableNpn           = true;
    cfg.allowNegation       = true;
    cfg.useImprovementFilter = !isAggressive;
    cfg.kImproveMargin       = 0.005;
    cfg.cleanupAbcSeq =
        isAggressive
            ? ("strash; dc2; if -K " + abcLutK() + " -a; sweep; topo")
            : ("strash; dc2; balance; if -K " + abcLutK() +
               " -a; sweep; topo");
    cfg.tag                  = isAggressive ? "PONO_agg" : "PONO_cons";
    return rewriteBlifUnified(originalBlifPath, actualProbs,
                              hexMappingLib_, cfg);
}

// ============================================================================
// Shared rewrite core (unified from rewriteBlifWithLibrary and
// rewriteMappedBlifWithGivenLibrarySimple). Every experimental engine --
// PONO aggressive/conservative, ABC-local, PONO-local -- reaches this
// routine; the RewriteConfig struct captures everything they historically
// disagreed about (pre-mapping, scoring weights, improvement threshold,
// final ABC cleanup pass, log tag).
// ============================================================================
std::string fes::InnovusBatchEvaluator::rewriteBlifUnified(
    const std::string& inputBlifPath,
    const std::vector<double>& actualProbs,
    const std::map<std::string, std::vector<LibEntry>>& targetLib,
    const RewriteConfig& cfgIn)
{
    const RewriteConfig cfg = cfgIn;
    namespace fs = std::filesystem;

    fs::path workDir = workDir_;
    if (!fs::exists(workDir)) fs::create_directories(workDir);

    const std::string baseName = fs::path(inputBlifPath).stem().string();
    const std::string suffix   = "_" + cfg.tag;
    const std::string mappedBlif =
        (workDir / (baseName + "_lut4" + suffix + ".blif")).string();
    const std::string optBlif =
        (workDir / (baseName + "_opt" + suffix + ".blif")).string();
    const std::string cleanBlif =
        (workDir / (baseName + "_clean" + suffix + ".blif")).string();

    // Pre-mapping step: the PONO tournament runs `strash; if -K 4 -a` here;
    // the ABC local-library path passes an already-mapped BLIF and sets
    // preMapAbcSeq = "" to skip this.
    std::string workingBlif;
    if (cfg.preMapAbcSeq.empty()) {
        workingBlif = inputBlifPath;
    } else {
        std::string cmd = abcPath_ + " -c \"read_blif " + inputBlifPath +
                          "; " + cfg.preMapAbcSeq +
                          "; write_blif " + mappedBlif +
                          "\" > /dev/null 2>&1";
        if (system(cmd.c_str()) != 0 || !fs::exists(mappedBlif)) {
            std::cerr << "[Rewriter][" << cfg.tag << "] " << baseName
                      << ": pre-mapping failed, fallback to input.\n";
            return inputBlifPath;
        }
        workingBlif = mappedBlif;
    }

    // Collect primary IO names; used both to seed signal probabilities and
    // to keep fragment outputs from clashing with PI/PO nets.
    std::set<std::string> primaryIOs;
    std::vector<std::string> orderedInputs;
    {
        std::ifstream fin(workingBlif);
        std::string ln;
        while (readLineSafe(fin, ln)) {
            if (ln.compare(0, 7, ".inputs") == 0) {
                std::stringstream ss(ln.substr(7));
                std::string io;
                while (ss >> io) {
                    if (io != "\\" && !io.empty()) {
                        primaryIOs.insert(io);
                        orderedInputs.push_back(io);
                    }
                }
            } else if (ln.compare(0, 8, ".outputs") == 0) {
                std::stringstream ss(ln.substr(8));
                std::string io;
                while (ss >> io) {
                    if (io != "\\" && !io.empty()) primaryIOs.insert(io);
                }
            }
        }
    }

    std::map<std::string, double> signalProbs;
    for (size_t i = 0; i < orderedInputs.size(); ++i) {
        signalProbs[orderedInputs[i]] =
            (i < actualProbs.size()) ? actualProbs[i] : 0.5;
    }

    auto parseFragmentIO =
        [&](const std::string& fragment,
            std::vector<std::string>& libInputs,
            std::vector<std::string>& libOutputs,
            int& gateCount) {
            libInputs.clear();
            libOutputs.clear();
            gateCount = 0;
            std::stringstream ss(fragment);
            std::string ln;
            while (readLineSafe(ss, ln)) {
                if (ln.compare(0, 7, ".inputs") == 0) {
                    std::stringstream si(ln.substr(7));
                    std::string p;
                    while (si >> p) {
                        if (p != "\\" && !p.empty()) libInputs.push_back(p);
                    }
                } else if (ln.compare(0, 8, ".outputs") == 0) {
                    std::stringstream so(ln.substr(8));
                    std::string p;
                    while (so >> p) {
                        if (p != "\\" && !p.empty()) libOutputs.push_back(p);
                    }
                } else if (ln.compare(0, 6, ".names") == 0) {
                    gateCount++;
                }
            }
        };

    auto buildPortMap =
        [&](const std::vector<std::string>& libInputs,
            const std::vector<std::string>& libOutputs,
            const std::vector<std::string>& realPorts,
            const std::string& outNet)
            -> std::unordered_map<std::string, std::string> {
            std::unordered_map<std::string, std::string> portMap;
            for (size_t i = 0; i < libInputs.size() && i < realPorts.size();
                 ++i) {
                portMap[libInputs[i]] = realPorts[i];
            }
            for (const auto& o : libOutputs) portMap[o] = outNet;
            return portMap;
        };

    auto remapFragmentForEmit =
        [&](const std::string& fragment,
            const std::unordered_map<std::string, std::string>& portMap,
            const std::set<std::string>& pioSet,
            const std::string& localSuffix,
            const NpnRecipe& recipe,
            const std::vector<std::string>& canonicalPorts,
            const std::vector<std::string>& constZeroNets,
            const std::string& realOutNet) -> std::string {
            std::stringstream ss(fragment);
            std::string ln;
            std::string prelude;
            std::string processed;
            std::string epilogue;

            for (const auto& constNet : constZeroNets) {
                prelude += ".names " + constNet + "\n";
            }

            for (size_t i = 0; i < canonicalPorts.size(); ++i) {
                if (!recipe.inputNegations.empty() && recipe.inputNegations[i]) {
                    const std::string invWire =
                        "inv_in_" + std::to_string(i) + localSuffix;
                    prelude += ".names " + canonicalPorts[i] + " " + invWire + "\n";
                    prelude += "0 1\n";
                }
            }

            if (recipe.outputNegation) {
                const std::string invOutWire = "inv_out" + localSuffix;
                epilogue += ".names " + invOutWire + " " + realOutNet + "\n";
                epilogue += "0 1\n";
            }

            while (readLineSafe(ss, ln)) {
                if (ln.empty()) continue;
                if (ln[0] == '.' && ln.compare(0, 6, ".names") != 0) continue;
                bool isNamesLine = (ln.compare(0, 6, ".names") == 0);
                std::stringstream ls(ln);
                std::string word;
                std::string out;
                bool first = true;
                while (ls >> word) {
                    if (isNamesLine && !first) {
                        auto it = portMap.find(word);
                        if (it != portMap.end()) out += it->second + " ";
                        else if (pioSet.count(word)) out += word + " ";
                        else out += word + localSuffix + " ";
                    } else {
                        out += word + " ";
                    }
                    first = false;
                }
                processed += out + "\n";
            }
            return prelude + processed + epilogue;
        };

    // Unified score = kSwitchWeight * totalSwitching
    //               + kOutputWeight * outputToggle
    //               + kGateWeight * max(1, gateCount)
    //               + kActivityWeight * activityDistance
    //               + kLibraryScoreWeight * normalizedLibraryScore
    //               + totalNegationCount * kInverterPowerPenalty.
    // Zeroing the output/activity weights recovers the legacy PONO engine's
    // pure-switching formula.
    auto scoreCandidate =
        [&](const LibEntry& cand,
            const std::vector<double>& inProbs,
            double normalizedLibScore,
            int negationCount)
            -> std::pair<FragmentPowerInfo, double> {
            FragmentPowerInfo fpi =
                estimateFragmentSwitching(cand.blifContent, inProbs);
            double activityDist = 0.0;
            size_t activityN = std::min(cand.idealActivities.size(),
                                        inProbs.size());
            if (activityN > 0) {
                for (size_t i = 0; i < activityN; ++i) {
                    activityDist += std::abs(
                        cand.idealActivities[i] - inProbs[i]);
                }
                activityDist /= static_cast<double>(activityN);
            }
            double score = cfg.kSwitchWeight * fpi.totalSwitching
                         + cfg.kOutputWeight * fpi.outputToggle
                         + cfg.kGateWeight * std::max(1, fpi.gateCount)
                         + cfg.kActivityWeight * activityDist
                         + cfg.kLibraryScoreWeight * normalizedLibScore
                         + static_cast<double>(negationCount) *
                               kInverterPowerPenalty;
            return {fpi, score};
        };

    std::ifstream ifs(workingBlif);
    std::ofstream ofs(optBlif);

    std::string line, pendingLine;
    bool hasPending = false;

    int instanceId = 0;
    int totalNamesCount = 0;
    int replacedCount = 0;
    int rejectedByNoLib = 0;
    int rejectedByBadCand = 0;
    int rejectedByNoImprove = 0;
    int rejectedByNegationPolicy = 0;

    while (true) {
        if (hasPending) {
            line = pendingLine;
            hasPending = false;
        } else {
            if (!readLineSafe(ifs, line)) break;
        }

        if (line.compare(0, 6, ".names") != 0) {
            ofs << line << "\n";
            continue;
        }

        totalNamesCount++;

        std::stringstream ss(line);
        std::string tagWord, tok;
        std::vector<std::string> ports;
        ss >> tagWord;
        while (ss >> tok) {
            if (tok != "\\") ports.push_back(tok);
        }

        if (ports.empty()) {
            ofs << line << "\n";
            continue;
        }

        std::string outNet = ports.back();
        ports.pop_back();

        std::vector<std::string> sop;
        while (readLineSafe(ifs, pendingLine)) {
            if (pendingLine.empty()) continue;
            if (pendingLine[0] == '.') {
                hasPending = true;
                break;
            }
            sop.push_back(pendingLine);
        }

        auto writeOriginal = [&]() {
            ofs << ".names ";
            for (const auto& p : ports) ofs << p << " ";
            ofs << outNet << "\n";
            for (const auto& row : sop) ofs << row << "\n";
        };

        std::vector<double> localInputProbs;
        localInputProbs.reserve(ports.size());
        for (const auto& p : ports) {
            localInputProbs.push_back(
                signalProbs.count(p) ? signalProbs[p] : 0.5);
        }

        double outProb = computeSopOutputProb(sop, localInputProbs);
        outProb = std::clamp(outProb, 0.0, 1.0);
        signalProbs[outNet] = outProb;

        // LUTs wider than the library's K are rewritten only if the library
        // itself spans that width. Gate-count is bounded by kLutMaxInputs.
        if (ports.size() > static_cast<size_t>(kLutMaxInputs)) {
            rejectedByNoLib++;
            writeOriginal();
            continue;
        }

        LutTruthTable hexVal = getHexValue(sop, static_cast<int>(ports.size()));
        std::string hexKey = truthTableHexKey(hexVal);
        std::vector<std::string> matchedPorts = ports;
        std::vector<double> matchedInputProbs = localInputProbs;
        NpnRecipe emitRecipe;
        emitRecipe.canonicalHex = hexVal;
        emitRecipe.inputPermutation.resize(ports.size());
        emitRecipe.inputNegations.assign(ports.size(), false);
        for (size_t i = 0; i < ports.size(); ++i) {
            emitRecipe.inputPermutation[i] = static_cast<int>(i);
        }
        emitRecipe.outputNegation = false;
        int recipeNegationCount = 0;

        if (cfg.enableNpn) {
            emitRecipe = NpnCanonizer::computeCanonical(
                hexVal, static_cast<int>(ports.size()));
            recipeNegationCount = emitRecipe.totalNegationCount();
            hexKey = truthTableHexKey(emitRecipe.canonicalHex);

            matchedPorts.resize(ports.size());
            matchedInputProbs.resize(localInputProbs.size());
            for (size_t canonicalInput = 0; canonicalInput < ports.size();
                 ++canonicalInput) {
                const int realInput = emitRecipe.inputPermutation[canonicalInput];
                matchedPorts[canonicalInput] = ports[realInput];

                double prob = localInputProbs[realInput];
                if (emitRecipe.inputNegations[canonicalInput]) {
                    prob = 1.0 - prob;
                }
                matchedInputProbs[canonicalInput] = prob;
            }
        }

        auto libIt = targetLib.find(hexKey);
        if (libIt == targetLib.end()) {
            rejectedByNoLib++;
            writeOriginal();
            continue;
        }
        const auto& candidates = libIt->second;
        double finiteLibScoreMin = std::numeric_limits<double>::infinity();
        double finiteLibScoreMax = -std::numeric_limits<double>::infinity();
        for (const auto& cand : candidates) {
            if (!std::isfinite(cand.score)) continue;
            finiteLibScoreMin = std::min(finiteLibScoreMin, cand.score);
            finiteLibScoreMax = std::max(finiteLibScoreMax, cand.score);
        }
        const bool haveFiniteLibRange =
            std::isfinite(finiteLibScoreMin) &&
            std::isfinite(finiteLibScoreMax);

        if (cfg.enableNpn &&
            recipeNegationCount > 0 &&
            !cfg.allowNegation) {
            rejectedByNegationPolicy++;
            writeOriginal();
            continue;
        }

        // Score the incumbent LUT for the optional improvement filter.
        const double origToggle = 2.0 * outProb * (1.0 - outProb);
        const double origScore = cfg.kSwitchWeight * origToggle
                               + cfg.kOutputWeight * origToggle
                               + cfg.kGateWeight * 1.0;

        struct BestReplacement {
            int candIdx = -1;
            double score = std::numeric_limits<double>::infinity();
            int gateCount = std::numeric_limits<int>::max();
            int negationCount = 0;
            NpnRecipe recipe;
            std::string processedFragment;
        };

        BestReplacement best;

        for (int ci = 0; ci < (int)candidates.size(); ++ci) {
            const auto& cand = candidates[ci];
            double normalizedLibScore = 0.0;
            if (haveFiniteLibRange &&
                std::isfinite(cand.score) &&
                finiteLibScoreMax > finiteLibScoreMin + 1e-12) {
                normalizedLibScore =
                    (cand.score - finiteLibScoreMin) /
                    (finiteLibScoreMax - finiteLibScoreMin);
            }

            std::vector<std::string> libInputs, libOutputs;
            int fragmentGates = 0;
            parseFragmentIO(cand.blifContent, libInputs, libOutputs,
                            fragmentGates);

            if (libInputs.size() < ports.size()) continue;
            if (!cfg.allowPadMissingInputs &&
                libInputs.size() != ports.size()) {
                continue;
            }
            if (libOutputs.size() != 1) continue;
            if (fragmentGates <= 0) continue;

            std::vector<double> candidateInputProbs = matchedInputProbs;
            if (libInputs.size() > candidateInputProbs.size()) {
                if (!cfg.allowPadMissingInputs) continue;
                candidateInputProbs.resize(libInputs.size(), 0.0);
            }

            auto [fpi, score] = scoreCandidate(
                cand, candidateInputProbs, normalizedLibScore,
                recipeNegationCount);

            if (cfg.useImprovementFilter &&
                !(score + 1e-12 < origScore * (1.0 - cfg.kImproveMargin))) {
                continue;
            }

            bool better = false;
            if (score + 1e-12 < best.score) {
                better = true;
            } else if (std::abs(score - best.score) < 1e-12 &&
                       fragmentGates < best.gateCount) {
                better = true;
            }

            if (better) {
                std::string localSuffix = "_v" + std::to_string(instanceId);
                std::vector<std::string> emittedInputNets = matchedPorts;
                std::vector<std::string> constZeroNets;
                for (size_t i = 0; i < emittedInputNets.size(); ++i) {
                    if (!emitRecipe.inputNegations.empty() &&
                        emitRecipe.inputNegations[i]) {
                        emittedInputNets[i] =
                            "inv_in_" + std::to_string(i) + localSuffix;
                    }
                }
                for (size_t i = emittedInputNets.size();
                     i < libInputs.size(); ++i) {
                    const std::string constNet =
                        "const0_" + std::to_string(i) + localSuffix;
                    emittedInputNets.push_back(constNet);
                    constZeroNets.push_back(constNet);
                }
                const std::string emittedOutNet =
                    emitRecipe.outputNegation
                        ? "inv_out" + localSuffix
                        : outNet;
                auto portMap = buildPortMap(libInputs, libOutputs,
                                            emittedInputNets, emittedOutNet);
                best.candIdx = ci;
                best.score = score;
                best.gateCount = fragmentGates;
                best.negationCount = recipeNegationCount;
                best.recipe = emitRecipe;
                best.processedFragment = remapFragmentForEmit(
                    cand.blifContent, portMap, primaryIOs, localSuffix,
                    emitRecipe, matchedPorts, constZeroNets, outNet);
            }
        }

        if (best.candIdx < 0) {
            if (cfg.useImprovementFilter) rejectedByNoImprove++;
            else                          rejectedByBadCand++;
            writeOriginal();
            continue;
        }

        replacedCount++;
        instanceId++;
        ofs << "# " << cfg.tag << " LUT-Replace [" << hexKey
            << "] Negations=" << best.negationCount
            << " Score=" << std::scientific << std::setprecision(8)
            << best.score << "\n";
        ofs << best.processedFragment;
    }

    ifs.close();
    ofs.close();

    std::cout << "[Rewriter][" << cfg.tag << "] " << baseName
              << ": Total=" << totalNamesCount
              << ", Replaced=" << replacedCount
              << ", RejNoLib=" << rejectedByNoLib
              << ", RejBadCand=" << rejectedByBadCand;
    if (cfg.enableNpn && !cfg.allowNegation) {
        std::cout << ", RejNegationPolicy=" << rejectedByNegationPolicy;
    }
    if (cfg.useImprovementFilter) {
        std::cout << ", RejNoImprove=" << rejectedByNoImprove;
    }
    std::cout << "\n";

    std::string resultPath = optBlif;
    if (!cfg.cleanupAbcSeq.empty()) {
        std::string cleanCmd = abcPath_ + " -c \"read_blif " + optBlif +
                               "; " + cfg.cleanupAbcSeq +
                               "; write_blif " + cleanBlif +
                               "\" > /dev/null 2>&1";
        if (system(cleanCmd.c_str()) == 0 && fs::exists(cleanBlif)) {
            resultPath = cleanBlif;
        }
    }
    return verifyRewriteOrRevert(inputBlifPath, resultPath,
                                 cfg.tag + "_rewrite");
}

// 辅助函数：生成随机的临时文件名以防并发冲突
std::string generateTempFilename() {
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::system_clock::to_time_t(now);
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1000, 9999);
    
    std::stringstream ss;
    ss << "tmp_api_" << timestamp << "_" << dis(gen) << ".blif";
    return ss.str();
}

SingleOptResult InnovusBatchEvaluator::optimizeSingleBlifFromContent(
    const std::string& blifContent, 
    const std::vector<double>& actualProbs) 
{
    SingleOptResult result;
    result.success = false;

    if (blifContent.empty()) {
        result.errorMessage = "Input BLIF content is empty.";
        return result;
    }

    // 1. 将前端传来的内容写入隐蔽的临时文件
    namespace fs = std::filesystem;
    fs::path workDir = workDir_;
    if (!fs::exists(workDir)) fs::create_directories(workDir);
    const auto cleanupWorkDir = [&]() {
        removeDirectoryTree(workDir_);
    };
    
    std::string tempInputPath = (workDir / generateTempFilename()).string();
    std::ofstream ofs(tempInputPath);
    if (ofs.is_open()) {
        ofs << blifContent;
        ofs.close();
    } else {
        result.errorMessage = "Failed to create internal temp file.";
        cleanupWorkDir();
        return result;
    }

    // 2. 统计引脚并生成 Workloads (复用原逻辑，针对临时文件)
    int inputNum = countBlifInputs(tempInputPath);
    int numSets = 1;
    int numCycles = 1000;
    auto workloads = verifier_.generateWorkloadSets(inputNum, numSets, numCycles);

    for (auto& wl : workloads) {
        for (size_t i = 0; i < inputNum; ++i) {
            wl.probs[i] = (i < actualProbs.size()) ? actualProbs[i] : 0.5;
        }
    }

    // 3. 评估初始功耗
    PPAResult origPPA = verifier_.getAveragePPAResult(tempInputPath, workloads);
    if (origPPA.valid) {
        result.initialPower = origPPA.power_total;
    } else {
        result.errorMessage = "Original BLIF evaluation failed.";
        std::remove(tempInputPath.c_str()); // 清理
        cleanupWorkDir();
        return result;
    }

    // 4. 执行 PONO 优化
    std::string ponoBlifPath = rewriteBlifWithLibrary(tempInputPath, actualProbs, true);
    
    if (!ponoBlifPath.empty() && fs::exists(ponoBlifPath)) {
        // 5. 评估优化后功耗
        PPAResult ponoPPA = verifier_.getAveragePPAResult(ponoBlifPath, workloads);
        if (ponoPPA.valid) {
            result.optimizedPower = ponoPPA.power_total;
            result.success = true;
            
            // 6. 将优化后的文件内容读取到内存中，准备放入 JSON
            std::ifstream ifs(ponoBlifPath);
            if (ifs.is_open()) {
                std::string content((std::istreambuf_iterator<char>(ifs)),
                                    (std::istreambuf_iterator<char>()));
                result.optimizedBlifContent = content;
            }
        } else {
            result.errorMessage = "Optimized BLIF evaluation failed.";
        }
        
        // 清理生成的优化文件
        std::remove(ponoBlifPath.c_str());
    } else {
        result.errorMessage = "PONO Optimized BLIF was not generated.";
    }

    // 清理初始的临时输入文件
    std::remove(tempInputPath.c_str());
    cleanupWorkDir();

    return result;
}

fes::InnovusBatchEvaluator::InnovusBatchEvaluator(
    const std::string& lib,
    const std::string& abcLocalLib,
    const std::string& py,
    const std::string& abc)
    : libPath_(lib),
      abcLocalLibPath_(abcLocalLib),
      abcPath_(abc),
      verifier_(py) {
    outputRootPath_ = std::filesystem::path(libPath_);
    workDir_ = outputRootPath_ / "tmp_eval";
    std::filesystem::create_directories(workDir_);
    cec_ = std::make_unique<EquivalenceChecker>(cecLibrary_);
    loadOptimizationLibrary();
    loadABCOptimizationLibrary();
}

std::string fes::InnovusBatchEvaluator::verifyRewriteOrRevert(
    const std::string& inputPath,
    const std::string& rewrittenPath,
    const std::string& tag)
{
    namespace fs = std::filesystem;
    if (!verifyEnabled_) return rewrittenPath;
    if (rewrittenPath.empty() || !fs::exists(rewrittenPath)) {
        return rewrittenPath;
    }
    if (inputPath.empty() || !fs::exists(inputPath)) {
        // Without a known-good reference we can't verify; pass through.
        return rewrittenPath;
    }

    // Backend: ABC's native `cec`. The Z3 BLIF miter in EquivalenceChecker
    // is fine for tiny sub-circuits but chokes on full designs (thousands
    // of flat gates, netlists ABC emits in non-topological order). ABC's
    // combinational equivalence checker handles ordering and scale
    // transparently, so the rewrite pipeline delegates to it.
    fs::path workDir = workDir_;
    if (!fs::exists(workDir)) fs::create_directories(workDir);

    std::string stem = fs::path(rewrittenPath).stem().string();
    std::string logPath =
        (workDir / ("cec_" + tag + "_" + stem + ".log")).string();

    std::string cmd = abcPath_ + " -q \"cec " + inputPath + " " +
                      rewrittenPath + "\" > " + logPath + " 2>&1";
    int ret = std::system(cmd.c_str());

    // ABC returns 0 whether or not the two networks are equivalent, so
    // rely on the log verdict. Expected verdicts are
    // "Networks are equivalent" or "Networks are NOT EQUIVALENT".
    // Anything else (UNDECIDED, crash, missing verdict) is treated as
    // a verification failure so we never hand a suspect BLIF downstream.
    bool equivalent = false;
    bool haveVerdict = false;
    std::string verdictLine;
    std::ifstream logFile(logPath);
    if (logFile.is_open()) {
        std::string line;
        while (std::getline(logFile, line)) {
            if (line.find("Networks are equivalent") != std::string::npos) {
                equivalent = true;
                haveVerdict = true;
                verdictLine = line;
                break;
            }
            if (line.find("NOT EQUIVALENT") != std::string::npos ||
                line.find("are NOT EQUAL") != std::string::npos ||
                line.find("Networks are NOT") != std::string::npos) {
                equivalent = false;
                haveVerdict = true;
                verdictLine = line;
                break;
            }
            if (line.find("UNDECIDED") != std::string::npos) {
                equivalent = false;
                haveVerdict = true;
                verdictLine = line;
                break;
            }
        }
    }

    if (!haveVerdict) {
        std::cerr << "[CEC][Rewrite][" << tag << "] FAIL "
                  << fs::path(rewrittenPath).filename().string()
                  << " -> ABC cec produced no verdict (ret=" << ret
                  << ", log: " << logPath
                  << ") (reverting to input)" << std::endl;
        return inputPath;
    }

    if (equivalent) {
        std::cout << "[CEC][Rewrite][" << tag << "] OK "
                  << fs::path(rewrittenPath).filename().string()
                  << std::endl;
        return rewrittenPath;
    }

    std::cerr << "[CEC][Rewrite][" << tag << "] FAIL "
              << fs::path(rewrittenPath).filename().string()
              << " -> " << verdictLine
              << " (reverting to input)" << std::endl;
    return inputPath;
}

void fes::InnovusBatchEvaluator::loadABCOptimizationLibrary() {
    namespace fs = std::filesystem;

    if (abcLocalLibPath_.empty()) {
        std::cout << "[Batch] No ABC local library path provided. Skip loading ABC local lib." << std::endl;
        return;
    }

    std::string csvPath = abcLocalLibPath_ + "/final_results.csv";
    std::string infoDirPath = abcLocalLibPath_ + "/detailed_infos";

    std::cout << "[Batch] Loading ABC Local Library from: " << abcLocalLibPath_ << std::endl;

    std::ifstream file(csvPath);
    if (!file.is_open()) {
        std::cerr << "[Error] Could not open ABC library index: " << csvPath << std::endl;
        return;
    }

    abcHexMappingLib_.clear();

    std::string line;
    bool isHeader = true;

    while (std::getline(file, line)) {
        if (isHeader) {
            isHeader = false;
            continue;
        }

        if (line.find("Verification Failed") != std::string::npos ||
            line.find("Timeout") != std::string::npos) {
            continue;
        }

        std::stringstream ss(line);
        std::string hexFunc, probPattern, successStr, gatesStr, internalCostStr;

        std::getline(ss, hexFunc, ',');
        std::getline(ss, probPattern, ',');
        std::getline(ss, successStr, ',');
        std::getline(ss, gatesStr, ',');
        std::getline(ss, internalCostStr, ',');

        if (successStr == "true") {
            std::string fullFuncName = hexFunc + probPattern;
            fs::path blifFilePath = fs::path(infoDirPath) / hexFunc / ("abc_" + fullFuncName + ".blif");

            if (fs::exists(blifFilePath)) {
                std::ifstream bsf(blifFilePath);
                std::string content((std::istreambuf_iterator<char>(bsf)),
                                    (std::istreambuf_iterator<char>()));

                LibEntry entry;
                entry.blifContent = content;
                entry.score = parseCsvDoubleOr(
                    internalCostStr, std::numeric_limits<double>::infinity());

                std::vector<double> ideals;
                std::stringstream pSs(probPattern);
                std::string token;
                while (std::getline(pSs, token, '_')) {
                    if (!token.empty()) {
                        ideals.push_back(std::stod(token) / 100.0);
                    }
                }

                while (ideals.size() < static_cast<size_t>(kLutMaxInputs))
                    ideals.push_back(0.5);
                entry.idealActivities = ideals;

                abcHexMappingLib_[hexFunc].push_back(entry);
            }
        }
    }

    std::cout << "[Batch] ABC Library loaded. Total unique HexFuncs: "
              << abcHexMappingLib_.size() << std::endl;
}

std::string fes::InnovusBatchEvaluator::run4LutMappingOnly(const std::string& inputBlif) {
    namespace fs = std::filesystem;

    fs::path workDir = workDir_;
    if (!fs::exists(workDir)) fs::create_directories(workDir);

    std::string baseName = fs::path(inputBlif).stem().string();
    std::string outPath  = (workDir / (baseName + "_mapped_k4.blif")).string();
    std::string logPath  = (workDir / (baseName + "_mapped_k4.log")).string();

    std::string cmd = abcPath_ + " -c \"read_blif " + inputBlif +
                      "; strash; if -K " + abcLutK() + " -a; write_blif " + outPath +
                      "\" > " + logPath + " 2>&1";

    int ret = system(cmd.c_str());
    if (ret != 0 || !fs::exists(outPath)) return "";
    return verifyRewriteOrRevert(inputBlif, outPath, "ABC_lut4_map");
}

double fes::InnovusBatchEvaluator::computeSopOutputProb(
    const std::vector<std::string>& sop,
    const std::vector<double>& inProbs)
{
    int cnt0 = 0, cnt1 = 0;
    for (const auto& row : sop) {
        if (row.size() < 2) continue;
        char v = row.back();
        if (v == '0') cnt0++;
        else if (v == '1') cnt1++;
    }

    bool isCover0 = (cnt0 > cnt1);
    char targetOut = isCover0 ? '0' : '1';

    int n = (int)inProbs.size();
    if (n == 0) return 0.5;

    if (n <= 14) {
        double probCovered = 0.0;
        int total = (1 << n);

        for (int mask = 0; mask < total; ++mask) {
            double mintermProb = 1.0;
            for (int i = 0; i < n; ++i) {
                mintermProb *= ((mask >> i) & 1) ? inProbs[i] : (1.0 - inProbs[i]);
            }

            bool covered = false;
            for (const auto& row : sop) {
                std::string cube, outVal;
                std::stringstream rss(row);
                if (!(rss >> cube >> outVal)) continue;
                if (outVal.empty() || outVal[0] != targetOut) continue;

                bool match = true;
                for (int i = 0; i < (int)cube.size() && i < n; ++i) {
                    if (cube[i] == '1' && !((mask >> i) & 1)) { match = false; break; }
                    if (cube[i] == '0' &&  ((mask >> i) & 1)) { match = false; break; }
                }

                if (match) {
                    covered = true;
                    break;
                }
            }

            if (covered) probCovered += mintermProb;
        }

        return isCover0 ? (1.0 - probCovered) : probCovered;
    }

    double probNotCovered = 1.0;
    for (const auto& row : sop) {
        std::string cube, outVal;
        std::stringstream rss(row);
        if (!(rss >> cube >> outVal)) continue;
        if (outVal.empty() || outVal[0] != targetOut) continue;

        double cubeProb = 1.0;
        for (size_t i = 0; i < cube.size() && i < inProbs.size(); ++i) {
            if (cube[i] == '1') cubeProb *= inProbs[i];
            else if (cube[i] == '0') cubeProb *= (1.0 - inProbs[i]);
        }
        probNotCovered *= (1.0 - cubeProb);
    }

    return isCover0 ? probNotCovered : (1.0 - probNotCovered);
}

FragmentPowerInfo fes::InnovusBatchEvaluator::estimateFragmentSwitching(
    const std::string& blifContent,
    const std::vector<double>& inputProbs)
{
    FragmentPowerInfo info{0.0, 0, 0.0};

    std::vector<std::string> libInputNames;
    std::string libOutputName;

    {
        std::stringstream ss(blifContent);
        std::string line;
        while (std::getline(ss, line)) {
            if (line.compare(0, 7, ".inputs") == 0) {
                std::stringstream si(line.substr(7));
                std::string p;
                while (si >> p) {
                    if (p != "\\" && !p.empty()) libInputNames.push_back(p);
                }
            } else if (line.compare(0, 8, ".outputs") == 0) {
                std::stringstream so(line.substr(8));
                std::string p;
                while (so >> p) {
                    if (p != "\\" && !p.empty()) libOutputName = p;
                }
            }
        }
    }

    std::map<std::string, double> sigProb;
    for (size_t i = 0; i < libInputNames.size() && i < inputProbs.size(); ++i) {
        sigProb[libInputNames[i]] = inputProbs[i];
    }

    std::stringstream ss(blifContent);
    std::string line;
    std::string lastOutName;

    while (std::getline(ss, line)) {
        if (line.compare(0, 6, ".names") != 0) continue;

        std::stringstream ls(line);
        std::string tag, wire;
        std::vector<std::string> ports;
        ls >> tag;
        while (ls >> wire) {
            if (wire != "\\" && !wire.empty()) ports.push_back(wire);
        }

        if (ports.empty()) continue;
        std::string outName = ports.back();
        ports.pop_back();

        std::vector<std::string> sop;
        while (ss.peek() != EOF) {
            std::streampos pos = ss.tellg();
            std::string sopLine;
            if (!std::getline(ss, sopLine)) break;
            if (sopLine.empty()) continue;
            if (sopLine[0] == '.') {
                ss.seekg(pos);
                break;
            }
            sop.push_back(sopLine);
        }

        std::vector<double> gateInProbs;
        gateInProbs.reserve(ports.size());
        for (const auto& p : ports) {
            gateInProbs.push_back(sigProb.count(p) ? sigProb[p] : 0.5);
        }

        double outP = computeSopOutputProb(sop, gateInProbs);
        outP = std::clamp(outP, 0.0, 1.0);
        sigProb[outName] = outP;

        double toggle = 2.0 * outP * (1.0 - outP);
        info.totalSwitching += toggle;
        info.gateCount++;
        lastOutName = outName;
    }

    if (!libOutputName.empty() && sigProb.count(libOutputName)) {
        double p = sigProb[libOutputName];
        info.outputToggle = 2.0 * p * (1.0 - p);
    } else if (!lastOutName.empty() && sigProb.count(lastOutName)) {
        double p = sigProb[lastOutName];
        info.outputToggle = 2.0 * p * (1.0 - p);
    }

    return info;
}

// ============================================================================
// Thin adapter over rewriteBlifUnified: inputs are already LUT4-mapped, so we
// skip the internal pre-mapping. For the ABC local-library baseline we replace
// every LUT that is covered by the library; the local score only chooses which
// ABC fragment to use when multiple implementations exist for the same truth
// table, and never gates replacement itself.
// ============================================================================
std::string fes::InnovusBatchEvaluator::rewriteMappedBlifWithGivenLibrarySimple(
    const std::string& mappedBlifPath,
    const std::vector<double>& actualProbs,
    const std::map<std::string, std::vector<LibEntry>>& targetLib,
    const std::string& tag)
{
    RewriteConfig cfg;
    cfg.preMapAbcSeq         = "";  // input is already LUT4-mapped
    cfg.kGateWeight          = 0.10;
    cfg.kOutputWeight        = 0.35;
    cfg.kActivityWeight      = 0.08;
    cfg.kLibraryScoreWeight  = 0.08;
    cfg.enableNpn           = false;
    cfg.allowNegation       = false;
    cfg.useImprovementFilter = false;
    cfg.kImproveMargin       = 0.0;
    cfg.allowPadMissingInputs = true;
    cfg.cleanupAbcSeq        = "strash; dc2; balance; if -K " + abcLutK() +
                               " -a; sweep; topo";
    cfg.tag                  = tag;
    return rewriteBlifUnified(mappedBlifPath, actualProbs, targetLib, cfg);
}

std::string fes::InnovusBatchEvaluator::rewriteMappedBlifWithABCLibrarySimple(
    const std::string& mappedBlifPath,
    const std::vector<double>& actualProbs)
{
    if (abcHexMappingLib_.empty()) {
        std::cerr << "[ABC-LIB] abcHexMappingLib_ is empty.\n";
        return "";
    }
    return rewriteMappedBlifWithGivenLibrarySimple(
        mappedBlifPath, actualProbs, abcHexMappingLib_, "ABC_LIB");
}



} // namespace fes
