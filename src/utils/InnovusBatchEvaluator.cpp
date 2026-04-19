#include "fes/utils/InnovusBatchEvaluator.h"
#include "fes/core/NpnTransform.h"
#include "fes/core/Types.h"
#include <fstream>
#include <iostream>
#include <filesystem>
#include <regex>
#include <cmath>
#include <set>
#include <map>
#include <unistd.h>
#include <sstream>
#include <vector>
#include <algorithm>
#include <iomanip>
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

InnovusBatchEvaluator::InnovusBatchEvaluator(const std::string& lib, const std::string& py, const std::string& abc)
    : libPath_(lib), verifier_(py), abcPath_(abc) {
    cec_ = std::make_unique<EquivalenceChecker>(cecLibrary_);
    loadOptimizationLibrary();
}

std::vector<std::string> InnovusBatchEvaluator::findBlifFilesRecursive(const std::filesystem::path& folderPath) {
    std::vector<std::string> blifFiles;
    if (!std::filesystem::exists(folderPath)) return blifFiles;

    for (const auto& entry : std::filesystem::recursive_directory_iterator(folderPath)) {
        if (entry.is_regular_file() && entry.path().extension() == ".blif") {
            blifFiles.push_back(entry.path().string());
        }
    }
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
    auto files = findBlifFilesRecursive(benchmarksDir);
    std::vector<PPADiff> results;

    std::cout << "\n>>> Phase 3: Triple-Path Physical Validation (Dual-Engine Mode) <<<" << std::endl;

    for (size_t i = 0; i < files.size(); ++i) {
        std::string filePath = files[i];
        std::string fileName = std::filesystem::path(filePath).filename().string();
        
        std::cout << "====================================================" << std::endl;
        std::cout << "[" << (i+1) << "/" << files.size() << "] Benchmark: " << fileName << std::endl;

        PPADiff diff;
        diff.fileName = fileName;
        
        int inputNum = countBlifInputs(filePath);
        auto workloads = verifier_.generateWorkloadSets(inputNum, 1, 1000);

        std::vector<double> avgProbs(inputNum, 0.0);
        for (const auto& wl : workloads) { for (int j = 0; j < inputNum; ++j) { avgProbs[j] += wl.probs[j]; } }
        for (int j = 0; j < inputNum; ++j) { avgProbs[j] /= 1; }

        auto printPPA = [](const std::string& label, const PPAResult& res) {
            if (res.valid) {
                std::cout << "    [" << label << "] Power: " << std::fixed << std::setprecision(3) << res.power_total << " mW, "
                          << "Area: " << res.area << ", Delay: " << res.delay << " ns" << std::endl;
            } else {
                std::cout << "    [" << label << "] Evaluation FAILED." << std::endl;
            }
        };

        std::cout << "  - Evaluating Original..." << std::endl;
        diff.origPPA = verifier_.getAveragePPAResult(filePath, workloads); 
        printPPA("ORIG", diff.origPPA);

        std::cout << "  - Running ABC Script..." << std::endl;
        std::string abcHigh = runABCExhaustiveOpt(filePath); 
        if (!abcHigh.empty() && std::filesystem::exists(abcHigh)) {
            diff.abcHighPPA = verifier_.getAveragePPAResult(abcHigh, workloads);
            printPPA("ABC ", diff.abcHighPPA);
        }

        std::cout << "  - Running PONO Optimization (Tournament Mode)..." << std::endl;
        
        // 引擎 A：激进模式 (Aggressive)
        std::string ponoAggBlif = rewriteBlifWithLibrary(filePath, avgProbs, true); 
        PPAResult aggPPA; aggPPA.valid = false;
        if (!ponoAggBlif.empty() && std::filesystem::exists(ponoAggBlif)) {
            aggPPA = verifier_.getAveragePPAResult(ponoAggBlif, workloads);
        }

        // 引擎 B：保守模式 (Conservative)
        std::string ponoConsBlif = rewriteBlifWithLibrary(filePath, avgProbs, false);
        PPAResult consPPA; consPPA.valid = false;
        if (!ponoConsBlif.empty() && std::filesystem::exists(ponoConsBlif)) {
            consPPA = verifier_.getAveragePPAResult(ponoConsBlif, workloads);
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

        diff.success = (diff.origPPA.valid && diff.abcHighPPA.valid && diff.ponoPPA.valid);
        results.push_back(diff);

        if (diff.success) {
            double pGainABC = (diff.abcHighPPA.power_total > 0) ? (diff.abcHighPPA.power_total - diff.ponoPPA.power_total) / diff.abcHighPPA.power_total * 100.0 : 0.0;
            double pGainOrig = (diff.origPPA.power_total > 0) ? (diff.origPPA.power_total - diff.ponoPPA.power_total) / diff.origPPA.power_total * 100.0 : 0.0;
            
            std::cout << "  >>> Result: SUCCESS" << std::endl;
            std::cout << "      Net Power Gain vs ABC: " << std::fixed << std::setprecision(2) << pGainABC << "%" << std::endl;
            std::cout << "      Total Power Gain vs Orig: " << std::fixed << std::setprecision(2) << pGainOrig << "%" << std::endl;
        } else {
            std::cout << "  >>> Result: FAILED" << std::endl;
        }
        std::cout << std::endl;
        exportResultsToCsv(results);
    }
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

    std::string line;
    bool isHeader = true;

    while (std::getline(file, line)) {
        if (isHeader) { isHeader = false; continue; } 
        if (line.find("Verification Failed") != std::string::npos || 
            line.find("Timeout") != std::string::npos) {
            continue;
        }

        std::stringstream ss(line);
        std::string hexFunc, probPattern, successStr;
        
        std::getline(ss, hexFunc, ',');
        std::getline(ss, probPattern, ',');
        std::getline(ss, successStr, ',');

        if (successStr == "true") {
            std::string fullFuncName = hexFunc + probPattern;
            fs::path blifFilePath = fs::path(infoDirPath) / hexFunc / ("pono_" + fullFuncName + ".blif");

            if (fs::exists(blifFilePath)) {
                std::ifstream bsf(blifFilePath);
                std::string content((std::istreambuf_iterator<char>(bsf)), (std::istreambuf_iterator<char>()));

                LibEntry entry;
                entry.blifContent = content;
                entry.score = 0.0; 

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
    
    // 【修复点】：在执行之前确保 tmp_eval 目录存在
    fs::path workDir = fs::current_path() / "tmp_eval";
    if (!fs::exists(workDir)) fs::create_directories(workDir);

    std::string baseName = fs::path(inputBlif).stem().string();
    std::string outPath = (workDir / (baseName + "_abc_high.blif")).string();

    // 弃用会造成面积膨胀的 balance，改用面积严格驱动的 strash + dc2 + resyn2a 组合
    std::string highOptSeq = "strash; dc2; balance; rewrite; balance; rewrite; "
                             "rewrite -z; balance; rewrite -z; balance; "
                             "if -K " + abcLutK() + " -a";
    std::string abcCmd = abcPath_ + " -c \"read_blif " + inputBlif +
                         "; " + highOptSeq + "; write_blif " + outPath + "\" > tmp_eval/abc_baseline.log 2>&1";

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
    cfg.kGateWeight          = isAggressive ? 0.002 : 0.005;
    cfg.kOutputWeight        = 0.0;
    cfg.kActivityWeight      = 0.0;
    cfg.enableNpn           = true;
    cfg.allowNegation       = true;
    cfg.useImprovementFilter = false;
    cfg.cleanupAbcSeq        = "sweep; topo";
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

    fs::path workDir = fs::current_path() / "tmp_eval";
    if (!fs::exists(workDir)) fs::create_directories(workDir);

    const std::string baseName = fs::path(inputBlifPath).stem().string();
    const std::string suffix   = "_" + cfg.tag;
    const std::string mappedBlif =
        (workDir / (baseName + "_lut4" + suffix + ".blif")).string();
    const std::string optBlif =
        (workDir / (baseName + "_opt" + suffix + ".blif")).string();
    const std::string cleanBlif =
        (workDir / (baseName + "_clean" + suffix + ".blif")).string();

    // Pre-mapping step: legacy PONO engines run `strash; if -K 4 -a` here;
    // the mapped-origin flows pass an already-mapped BLIF and set
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
            const std::string& realOutNet) -> std::string {
            std::stringstream ss(fragment);
            std::string ln;
            std::string prelude;
            std::string processed;
            std::string epilogue;

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

    // Unified score = totalSwitching + kOutputWeight * outputToggle
    //               + kGateWeight * max(1, gateCount)
    //               + kActivityWeight * activityDistance
    //               + totalNegationCount * kInverterPowerPenalty.
    // Zeroing the output/activity weights recovers the legacy PONO engine's
    // pure-switching formula.
    auto scoreCandidate =
        [&](const LibEntry& cand,
            const std::vector<double>& inProbs,
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
            double score = fpi.totalSwitching
                         + cfg.kOutputWeight * fpi.outputToggle
                         + cfg.kGateWeight * std::max(1, fpi.gateCount)
                         + cfg.kActivityWeight * activityDist
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

        if (cfg.enableNpn &&
            recipeNegationCount > 0 &&
            !cfg.allowNegation) {
            rejectedByNegationPolicy++;
            writeOriginal();
            continue;
        }

        // Score the incumbent LUT for the optional improvement filter.
        const double origToggle = 2.0 * outProb * (1.0 - outProb);
        const double origScore = origToggle
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

            std::vector<std::string> libInputs, libOutputs;
            int fragmentGates = 0;
            parseFragmentIO(cand.blifContent, libInputs, libOutputs,
                            fragmentGates);

            if (libInputs.size() != ports.size()) continue;
            if (libOutputs.size() != 1) continue;
            if (fragmentGates <= 0) continue;

            auto [fpi, score] = scoreCandidate(
                cand, matchedInputProbs, recipeNegationCount);

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
                for (size_t i = 0; i < emittedInputNets.size(); ++i) {
                    if (!emitRecipe.inputNegations.empty() &&
                        emitRecipe.inputNegations[i]) {
                        emittedInputNets[i] =
                            "inv_in_" + std::to_string(i) + localSuffix;
                    }
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
                    emitRecipe, matchedPorts, outNet);
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

void InnovusBatchEvaluator::exportResultsToCsv(const std::vector<PPADiff>& results) {
    std::string csvPath = libPath_ + "/ppa_complete_validation.csv";
    std::ofstream ofs(csvPath);

    if (!ofs.is_open()) return;

    ofs << "Benchmark,"
        << "Power_Orig(mW),Power_ABC(mW),Power_PONO(mW),Gain_Power_vs_ABC(%),"
        << "Area_Orig,Area_ABC,Area_PONO,Gain_Area_vs_ABC(%),"
        << "Delay_Orig(ns),Delay_ABC(ns),Delay_PONO(ns),Gain_Delay_vs_ABC(%),"
        << "Status\n";

    for (const auto& r : results) {
        if (!r.success) {
            ofs << r.fileName << ",,,,,,,,,,,,,FAILED\n";
            continue;
        }

        auto calcGain = [](double base, double opt) { 
            return (base > 0) ? (base - opt) / base * 100.0 : 0.0; 
        };

        double pGain = calcGain(r.abcHighPPA.power_total, r.ponoPPA.power_total);
        double aGain = calcGain(r.abcHighPPA.area, r.ponoPPA.area);
        double dGain = calcGain(r.abcHighPPA.delay, r.ponoPPA.delay);

        ofs << r.fileName << ","
            << r.origPPA.power_total << "," << r.abcHighPPA.power_total << "," << r.ponoPPA.power_total << ","
            << std::fixed << std::setprecision(2) << pGain << "%,"
            << r.origPPA.area << "," << r.abcHighPPA.area << "," << r.ponoPPA.area << ","
            << aGain << "%,"
            << r.origPPA.delay << "," << r.abcHighPPA.delay << "," << r.ponoPPA.delay << ","
            << dGain << "%,"
            << "SUCCESS\n";
    }
    ofs.close();
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
    fs::path workDir = fs::current_path() / "tmp_eval";
    if (!fs::exists(workDir)) fs::create_directories(workDir);
    
    std::string tempInputPath = (workDir / generateTempFilename()).string();
    std::ofstream ofs(tempInputPath);
    if (ofs.is_open()) {
        ofs << blifContent;
        ofs.close();
    } else {
        result.errorMessage = "Failed to create internal temp file.";
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
    fs::path workDir = fs::current_path() / "tmp_eval";
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
        std::string hexFunc, probPattern, successStr;

        std::getline(ss, hexFunc, ',');
        std::getline(ss, probPattern, ',');
        std::getline(ss, successStr, ',');

        if (successStr == "true") {
            std::string fullFuncName = hexFunc + probPattern;
            fs::path blifFilePath = fs::path(infoDirPath) / hexFunc / ("abc_" + fullFuncName + ".blif");

            if (fs::exists(blifFilePath)) {
                std::ifstream bsf(blifFilePath);
                std::string content((std::istreambuf_iterator<char>(bsf)),
                                    (std::istreambuf_iterator<char>()));

                LibEntry entry;
                entry.blifContent = content;
                entry.score = 0.0;

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

    fs::path workDir = fs::current_path() / "tmp_eval";
    if (!fs::exists(workDir)) fs::create_directories(workDir);

    std::string baseName = fs::path(inputBlif).stem().string();
    std::string outPath  = (workDir / (baseName + "_mapped_k4.blif")).string();

    std::string cmd = abcPath_ + " -c \"read_blif " + inputBlif +
                      "; strash; if -K " + abcLutK() + " -a; write_blif " + outPath +
                      "\" > tmp_eval/mapped_k4.log 2>&1";

    int ret = system(cmd.c_str());
    if (ret != 0 || !fs::exists(outPath)) return "";
    return verifyRewriteOrRevert(inputBlif, outPath, "ABC_lut4_map");
}

std::string fes::InnovusBatchEvaluator::runABCGlobalStrongOnMapped(const std::string& mappedBlif) {
    namespace fs = std::filesystem;

    fs::path workDir = fs::current_path() / "tmp_eval";
    if (!fs::exists(workDir)) fs::create_directories(workDir);

    std::string baseName = fs::path(mappedBlif).stem().string();
    std::string outPath  = (workDir / (baseName + "_abc_global_strong.blif")).string();
    std::string logPath  = (workDir / (baseName + "_abc_global_strong.log")).string();

    // 不依赖 abc.rc，直接展开强脚本
    std::string seq =
        "strash; "
        "dc2; "
        "balance; rewrite; balance; rewrite; rewrite -z; balance; rewrite -z; balance; "
        "if -K " + abcLutK() + " -a";

    std::string cmd = abcPath_ + " -c \"read_blif " + mappedBlif +
                      "; " + seq +
                      "; write_blif " + outPath +
                      "\" > " + logPath + " 2>&1";

    int ret = system(cmd.c_str());

    if (ret != 0 || !fs::exists(outPath)) {
        std::cerr << "[ABC-G] Failed on " << baseName
                  << " | log: " << logPath << std::endl;
        return "";
    }

    return verifyRewriteOrRevert(mappedBlif, outPath, "ABC_global_strong");
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
// skip the internal pre-mapping; enable the improvement filter and the heavy
// ABC cleanup pass that the legacy mapped-origin flow relied on.
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
    cfg.enableNpn           = false;
    cfg.allowNegation       = false;
    cfg.useImprovementFilter = true;
    cfg.kImproveMargin       = 0.01;
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

std::string fes::InnovusBatchEvaluator::rewriteMappedBlifWithPONOLibrarySimple(
    const std::string& mappedBlifPath,
    const std::vector<double>& actualProbs)
{
    if (hexMappingLib_.empty()) {
        std::cerr << "[PONO-LIB] hexMappingLib_ is empty.\n";
        return "";
    }
    return rewriteMappedBlifWithGivenLibrarySimple(
        mappedBlifPath, actualProbs, hexMappingLib_, "PONO_LIB");
}

void fes::InnovusBatchEvaluator::exportMappedFourWayResultsToCsv(
    const std::vector<MappedFourWayResult>& results)
{
    std::string csvPath = libPath_ + "/ppa_mapped_four_way_validation.csv";
    std::ofstream ofs(csvPath);
    if (!ofs.is_open()) return;

    ofs << "Benchmark,"
        << "Power_MappedOrig(mW),Area_MappedOrig,Delay_MappedOrig(ns),"
        << "Power_ABC_Local(mW),Area_ABC_Local,Delay_ABC_Local(ns),"
        << "Power_ABC_Global(mW),Area_ABC_Global,Delay_ABC_Global(ns),"
        << "Power_PONO_Local(mW),Area_PONO_Local,Delay_PONO_Local(ns),"
        << "Gain_Power_ABC_Local_vs_MappedOrig(%),"
        << "Gain_Power_ABC_Global_vs_MappedOrig(%),"
        << "Gain_Power_PONO_vs_MappedOrig(%),"
        << "Gain_Power_PONO_vs_ABC_Local(%),"
        << "Gain_Power_PONO_vs_ABC_Global(%),"
        << "Status\n";

    auto calcGain = [](double base, double opt) {
        return (base > 0) ? (base - opt) / base * 100.0 : 0.0;
    };

    for (const auto& r : results) {
        if (!r.success) {
            ofs << r.fileName << ",";

            if (r.mappedOrigValid) ofs << r.mappedOrigPPA.power_total << "," << r.mappedOrigPPA.area << "," << r.mappedOrigPPA.delay << ",";
            else ofs << "NA,NA,NA,";

            if (r.abcLocalValid) ofs << r.abcLocalPPA.power_total << "," << r.abcLocalPPA.area << "," << r.abcLocalPPA.delay << ",";
            else ofs << "NA,NA,NA,";

            if (r.abcGlobalValid) ofs << r.abcGlobalPPA.power_total << "," << r.abcGlobalPPA.area << "," << r.abcGlobalPPA.delay << ",";
            else ofs << "NA,NA,NA,";

            if (r.ponoLocalValid) ofs << r.ponoLocalPPA.power_total << "," << r.ponoLocalPPA.area << "," << r.ponoLocalPPA.delay << ",";
            else ofs << "NA,NA,NA,";

            ofs << "NA,NA,NA,NA,NA,PARTIAL\n";
            continue;
        }

        double gABC_L_vs_M = calcGain(r.mappedOrigPPA.power_total, r.abcLocalPPA.power_total);
        double gABC_G_vs_M = calcGain(r.mappedOrigPPA.power_total, r.abcGlobalPPA.power_total);
        double gPONO_vs_M  = calcGain(r.mappedOrigPPA.power_total, r.ponoLocalPPA.power_total);
        double gPONO_vs_AL = calcGain(r.abcLocalPPA.power_total, r.ponoLocalPPA.power_total);
        double gPONO_vs_AG = calcGain(r.abcGlobalPPA.power_total, r.ponoLocalPPA.power_total);

        ofs << r.fileName << ","
            << r.mappedOrigPPA.power_total << "," << r.mappedOrigPPA.area << "," << r.mappedOrigPPA.delay << ","
            << r.abcLocalPPA.power_total   << "," << r.abcLocalPPA.area   << "," << r.abcLocalPPA.delay   << ","
            << r.abcGlobalPPA.power_total  << "," << r.abcGlobalPPA.area  << "," << r.abcGlobalPPA.delay  << ","
            << r.ponoLocalPPA.power_total  << "," << r.ponoLocalPPA.area  << "," << r.ponoLocalPPA.delay  << ","
            << std::fixed << std::setprecision(2)
            << gABC_L_vs_M << ","
            << gABC_G_vs_M << ","
            << gPONO_vs_M  << ","
            << gPONO_vs_AL << ","
            << gPONO_vs_AG << ","
            << "SUCCESS\n";
    }
}

static int countBlifInputsLocal(const std::string& blifPath) {
    std::ifstream ifs(blifPath);
    if (!ifs.is_open()) return 0;

    std::string line;
    while (std::getline(ifs, line)) {
        if (line.rfind(".inputs", 0) == 0) {
            std::stringstream ss(line.substr(7));
            std::string tok;
            int cnt = 0;
            while (ss >> tok) {
                if (tok != "\\" && !tok.empty()) cnt++;
            }
            return cnt;
        }
    }
    return 0;
}

void fes::InnovusBatchEvaluator::runBatchVerificationMappedFourWay(const std::string& benchmarksDir) {
    auto files = findBlifFilesRecursive(benchmarksDir);
    std::vector<MappedFourWayResult> results;

    std::ofstream cmpCsv(libPath_ + "/ppa_orig_vs_mapped_compare.csv");
    if (cmpCsv.is_open()) {
        cmpCsv << "Benchmark,"
               << "Power_Orig(mW),Area_Orig,Delay_Orig(ns),"
               << "Power_MappedOrig(mW),Area_MappedOrig,Delay_MappedOrig(ns),"
               << "Power_ABC_Local(mW),Area_ABC_Local,Delay_ABC_Local(ns),"
               << "Power_ABC_Global(mW),Area_ABC_Global,Delay_ABC_Global(ns),"
               << "Power_PONO_Local(mW),Area_PONO_Local,Delay_PONO_Local(ns),"
               << "Gain_Mapped_vs_Orig(%),Gain_ABC_Local_vs_Orig(%),Gain_ABC_Global_vs_Orig(%),Gain_PONO_vs_Orig(%),"
               << "Best_Label,Best_Power(mW),Status\n";
    }

    std::cout << "\n>>> Phase 3B: Four-Way Validation on LUT4-Mapped Origin <<<" << std::endl;

    auto calcGain = [](double base, double opt) {
        return (base > 0) ? (base - opt) / base * 100.0 : 0.0;
    };

    for (size_t i = 0; i < files.size(); ++i) {
        std::string filePath = files[i];
        std::string fileName = std::filesystem::path(filePath).filename().string();

        std::cout << "====================================================" << std::endl;
        std::cout << "[" << (i + 1) << "/" << files.size() << "] Benchmark: " << fileName << std::endl;

        MappedFourWayResult diff;
        diff.fileName = fileName;

        auto printPPA = [](const std::string& label, const PPAResult& res) {
            if (res.valid) {
                std::cout << "    [" << label << "] Power: "
                          << std::fixed << std::setprecision(3) << res.power_total
                          << " mW, Area: " << res.area
                          << ", Delay: " << res.delay << " ns" << std::endl;
            } else {
                std::cout << "    [" << label << "] Evaluation FAILED." << std::endl;
            }
        };

        std::cout << "  - Generating LUT4 mapped origin..." << std::endl;
        std::string mappedOrigin = run4LutMappingOnly(filePath);
        if (mappedOrigin.empty() || !std::filesystem::exists(mappedOrigin)) {
            std::cout << "  >>> Result: FAILED (mapping failed)" << std::endl << std::endl;
            if (cmpCsv.is_open()) cmpCsv << fileName << ",NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,FAILED\n";
            results.push_back(diff);
            exportMappedFourWayResultsToCsv(results);
            continue;
        }

        int origInputNum = countBlifInputs(filePath);
        int mappedInputNum = countBlifInputs(mappedOrigin);
        int inputNum = mappedInputNum > 0 ? mappedInputNum : origInputNum;

        std::cout << "    [INFO] Inputs(orig=" << origInputNum
                  << ", mapped=" << mappedInputNum
                  << ", used=" << inputNum << ")" << std::endl;

        if (inputNum <= 0) {
            std::cout << "  >>> Result: FAILED (invalid input count)" << std::endl << std::endl;
            if (cmpCsv.is_open()) cmpCsv << fileName << ",NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,FAILED\n";
            results.push_back(diff);
            exportMappedFourWayResultsToCsv(results);
            continue;
        }

        const int kNumWorkloadSets = 4;
        auto workloads = verifier_.generateWorkloadSets(inputNum, kNumWorkloadSets, 1000);

        std::vector<double> avgProbs(inputNum, 0.0);
        for (const auto& wl : workloads) {
            for (int j = 0; j < inputNum && j < (int)wl.probs.size(); ++j) {
                avgProbs[j] += wl.probs[j];
            }
        }
        double workloadCount = std::max<size_t>(1, workloads.size());
        for (int j = 0; j < inputNum; ++j) {
            avgProbs[j] /= workloadCount;
        }

        std::cout << "  - Evaluating True Original..." << std::endl;
        PPAResult origPPA;
        bool origValid = false;
        if (origInputNum == inputNum) {
            origPPA = verifier_.getAveragePPAResult(filePath, workloads);
        } else {
            auto origWorkloads = verifier_.generateWorkloadSets(origInputNum, kNumWorkloadSets, 1000);
            origPPA = verifier_.getAveragePPAResult(filePath, origWorkloads);
        }
        origValid = origPPA.valid;
        printPPA("ORIG", origPPA);

        std::cout << "  - Evaluating Mapped-Origin..." << std::endl;
        diff.mappedOrigPPA = verifier_.getAveragePPAResult(mappedOrigin, workloads);
        diff.mappedOrigValid = diff.mappedOrigPPA.valid;
        printPPA("M-ORG", diff.mappedOrigPPA);

        std::cout << "  - Running ABC local-library rewrite..." << std::endl;
        std::string abcLocalBlif = rewriteMappedBlifWithABCLibrarySimple(mappedOrigin, avgProbs);
        if (!abcLocalBlif.empty() && std::filesystem::exists(abcLocalBlif)) {
            diff.abcLocalPPA = verifier_.getAveragePPAResult(abcLocalBlif, workloads);
            diff.abcLocalValid = diff.abcLocalPPA.valid;
            printPPA("ABC-L", diff.abcLocalPPA);
        }

        std::cout << "  - Running ABC global strong flow..." << std::endl;
        std::string abcGlobalBlif = runABCGlobalStrongOnMapped(mappedOrigin);
        if (!abcGlobalBlif.empty() && std::filesystem::exists(abcGlobalBlif)) {
            diff.abcGlobalPPA = verifier_.getAveragePPAResult(abcGlobalBlif, workloads);
            diff.abcGlobalValid = diff.abcGlobalPPA.valid;
            printPPA("ABC-G", diff.abcGlobalPPA);
        }

        std::cout << "  - Running PONO local-library rewrite..." << std::endl;
        std::string ponoLocalBlif = rewriteMappedBlifWithPONOLibrarySimple(mappedOrigin, avgProbs);
        if (!ponoLocalBlif.empty() && std::filesystem::exists(ponoLocalBlif)) {
            diff.ponoLocalPPA = verifier_.getAveragePPAResult(ponoLocalBlif, workloads);
            diff.ponoLocalValid = diff.ponoLocalPPA.valid;
            printPPA("PONO ", diff.ponoLocalPPA);
        }

        diff.success =
            diff.mappedOrigValid &&
            diff.abcLocalValid &&
            diff.abcGlobalValid &&
            diff.ponoLocalValid;

        results.push_back(diff);

        std::string bestLabel = origValid ? "ORIG" : "M-ORG";
        double bestPower = origValid ? origPPA.power_total : (diff.mappedOrigValid ? diff.mappedOrigPPA.power_total : std::numeric_limits<double>::infinity());
        auto tryUpdateBest = [&](const std::string& label, const PPAResult& ppa, bool valid) {
            if (valid && ppa.power_total < bestPower) {
                bestPower = ppa.power_total;
                bestLabel = label;
            }
        };
        tryUpdateBest("M-ORG", diff.mappedOrigPPA, diff.mappedOrigValid);
        tryUpdateBest("ABC-L", diff.abcLocalPPA, diff.abcLocalValid);
        tryUpdateBest("ABC-G", diff.abcGlobalPPA, diff.abcGlobalValid);
        tryUpdateBest("PONO", diff.ponoLocalPPA, diff.ponoLocalValid);

        if (origValid && diff.mappedOrigValid) {
            std::cout << "      Mapped-Origin vs True Original: "
                      << std::fixed << std::setprecision(2)
                      << calcGain(origPPA.power_total, diff.mappedOrigPPA.power_total) << "%" << std::endl;
        }
        if (origValid && diff.abcLocalValid) {
            std::cout << "      ABC-Local vs True Original: "
                      << calcGain(origPPA.power_total, diff.abcLocalPPA.power_total) << "%" << std::endl;
        }
        if (origValid && diff.abcGlobalValid) {
            std::cout << "      ABC-Global vs True Original: "
                      << calcGain(origPPA.power_total, diff.abcGlobalPPA.power_total) << "%" << std::endl;
        }
        if (origValid && diff.ponoLocalValid) {
            std::cout << "      PONO vs True Original: "
                      << calcGain(origPPA.power_total, diff.ponoLocalPPA.power_total) << "%" << std::endl;
        }

        if (diff.success) {
            std::cout << "  >>> Result: SUCCESS" << std::endl;
            std::cout << "      PONO vs Mapped-Origin: "
                      << std::fixed << std::setprecision(2)
                      << calcGain(diff.mappedOrigPPA.power_total, diff.ponoLocalPPA.power_total) << "%" << std::endl;
            std::cout << "      PONO vs ABC-Local: "
                      << calcGain(diff.abcLocalPPA.power_total, diff.ponoLocalPPA.power_total) << "%" << std::endl;
            std::cout << "      PONO vs ABC-Global: "
                      << calcGain(diff.abcGlobalPPA.power_total, diff.ponoLocalPPA.power_total) << "%" << std::endl;
            std::cout << "      Lowest-power observed: " << bestLabel << " ("
                      << std::fixed << std::setprecision(3) << bestPower << " mW)" << std::endl;
        } else {
            std::cout << "  >>> Result: PARTIAL/FAILED" << std::endl;
            if (std::isfinite(bestPower)) {
                std::cout << "      Lowest-power observed among valid runs: " << bestLabel << " ("
                          << std::fixed << std::setprecision(3) << bestPower << " mW)" << std::endl;
            }
        }

        if (cmpCsv.is_open()) {
            auto writePPA = [&](const PPAResult& r, bool valid) {
                if (valid) cmpCsv << r.power_total << "," << r.area << "," << r.delay << ",";
                else cmpCsv << "NA,NA,NA,";
            };

            cmpCsv << fileName << ",";
            writePPA(origPPA, origValid);
            writePPA(diff.mappedOrigPPA, diff.mappedOrigValid);
            writePPA(diff.abcLocalPPA, diff.abcLocalValid);
            writePPA(diff.abcGlobalPPA, diff.abcGlobalValid);
            writePPA(diff.ponoLocalPPA, diff.ponoLocalValid);

            if (origValid && diff.mappedOrigValid) cmpCsv << calcGain(origPPA.power_total, diff.mappedOrigPPA.power_total) << ",";
            else cmpCsv << "NA,";
            if (origValid && diff.abcLocalValid) cmpCsv << calcGain(origPPA.power_total, diff.abcLocalPPA.power_total) << ",";
            else cmpCsv << "NA,";
            if (origValid && diff.abcGlobalValid) cmpCsv << calcGain(origPPA.power_total, diff.abcGlobalPPA.power_total) << ",";
            else cmpCsv << "NA,";
            if (origValid && diff.ponoLocalValid) cmpCsv << calcGain(origPPA.power_total, diff.ponoLocalPPA.power_total) << ",";
            else cmpCsv << "NA,";

            if (std::isfinite(bestPower)) cmpCsv << bestLabel << "," << bestPower << ",";
            else cmpCsv << "NA,NA,";
            cmpCsv << (diff.success ? "SUCCESS" : "PARTIAL") << "\n";
            cmpCsv.flush();
        }

        std::cout << std::endl;
        exportMappedFourWayResultsToCsv(results);
    }
}


} // namespace fes
