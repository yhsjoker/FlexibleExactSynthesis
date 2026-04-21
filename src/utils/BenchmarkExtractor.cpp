
#include "fes/utils/BenchmarkExtractor.h"
#include "fes/core/NpnTransform.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iomanip>
#include <cstdlib>
#include <filesystem>
#include <cctype>

namespace fes {

namespace {

std::string trimCopy(const std::string& s) {
    size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

LutTruthTable truthTableMaskForInputs(int numInputs) {
    const int rows = 1 << numInputs;
    if (rows >= 64) return ~LutTruthTable{0};
    return (LutTruthTable{1} << rows) - 1;
}

bool readLogicalLine(std::istream& is, std::string& outLine) {
    outLine.clear();

    std::string line;
    if (!std::getline(is, line)) return false;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    outLine = line;

    while (!outLine.empty()) {
        size_t last = outLine.find_last_not_of(" \t\r\n");
        if (last == std::string::npos) {
            outLine.clear();
            break;
        }
        if (outLine[last] != '\\') break;

        outLine.erase(last);
        std::string nextPart;
        if (!std::getline(is, nextPart)) break;
        if (!nextPart.empty() && nextPart.back() == '\r') nextPart.pop_back();
        outLine += " " + nextPart;
    }
    return true;
}

} // namespace

BenchmarkExtractor::BenchmarkExtractor(const std::string& abcPath, int lutInputs)
    : abcPath_(abcPath), lutInputs_(lutInputs) {}

void BenchmarkExtractor::processDirectory(const std::string& folderPath) {
    namespace fs = std::filesystem;

    if (!fs::exists(folderPath)) {
        std::cerr << "[Extractor] Error: Directory does not exist: " << folderPath << std::endl;
        return;
    }

    int processedCount = 0;
    int skippedCount = 0;

    const int LOCAL_TOP_K = 10;

    fs::path tempDir = fs::temp_directory_path() / "fes_abc_temp";
    if (!fs::exists(tempDir)) fs::create_directory(tempDir);

    std::cout << "[Extractor] Scanning directory: " << folderPath << " ..." << std::endl;

    for (const auto& entry : fs::recursive_directory_iterator(folderPath)) {
        if (entry.path().extension() == ".blif" || entry.path().extension() == ".aig") {
            std::string tempFileName = entry.path().stem().string() + "_mapped.blif";
            fs::path tempFilePath = tempDir / tempFileName;

            if (runAbcMapping(entry.path(), tempFilePath)) {
                auto local_map = processMappedFile(tempFilePath);

                std::vector<std::pair<TruthKey, int>> local_sorted(
                    local_map.begin(), local_map.end());
                std::sort(local_sorted.begin(), local_sorted.end(),
                    [](const auto& a, const auto& b) { return a.second > b.second; });

                int added = 0;
                for (const auto& pair : local_sorted) {
                    if (added >= LOCAL_TOP_K) break;
                    guaranteed_funcs_.insert(pair.first);
                    added++;
                }

                for (const auto& pair : local_map) {
                    frequency_map_[pair.first] += pair.second;
                }

                processedCount++;
                fs::remove(tempFilePath);
            } else {
                skippedCount++;
            }
        }
    }

    fs::remove(tempDir);
    std::cout << "[Extractor] Done. Processed " << processedCount
              << " files. Skipped " << skippedCount << " files." << std::endl;
    std::cout << "[Extractor] Guaranteed Benchmark-Specific Funcs: "
              << guaranteed_funcs_.size() << std::endl;
}

bool BenchmarkExtractor::runAbcMapping(const std::filesystem::path& inputFile,
                                       const std::filesystem::path& outputFile) {
    std::stringstream cmd;
    cmd << abcPath_ << " -c \"read " << inputFile.string()
        << "; strash; if -K " << lutInputs_ << " -a; write_blif "
        << outputFile.string() << "\"";

#ifdef _WIN32
    cmd << " > NUL 2>&1";
#else
    cmd << " > /dev/null 2>&1";
#endif

    int ret = std::system(cmd.str().c_str());
    if (ret != 0) {
        std::cerr << "[Extractor] ABC Warning: Failed to map "
                  << inputFile.filename() << std::endl;
        return false;
    }
    return std::filesystem::exists(outputFile);
}

std::map<BenchmarkExtractor::TruthKey, int> BenchmarkExtractor::processMappedFile(
    const std::filesystem::path& filePath) {
    std::map<TruthKey, int> local_freq;
    std::ifstream file(filePath);
    if (!file.is_open()) return local_freq;

    std::string rawLine;
    bool inGate = false;
    int numInputs = 0;
    std::vector<std::string> coverLines;

    auto flushGate = [&]() {
        if (inGate && numInputs >= 0 && numInputs <= lutInputs_) {
            LutTruthTable tt = computeTruthTable(numInputs, coverLines);
            tt &= truthTableMaskForInputs(numInputs);
            const NpnRecipe recipe =
                NpnCanonizer::computeCanonical(tt, numInputs);
            local_freq[{numInputs, recipe.canonicalHex}]++;
        }
        inGate = false;
        numInputs = 0;
        coverLines.clear();
    };

    while (readLogicalLine(file, rawLine)) {
        std::string line = trimCopy(rawLine);
        if (line.empty()) continue;
        if (line[0] == '#') continue;

        if (line.rfind(".names", 0) == 0) {
            flushGate();

            inGate = true;
            coverLines.clear();

            std::stringstream ss(line);
            std::string token;
            int tokenCount = 0;
            while (ss >> token) {
                if (token != "\\") tokenCount++;
            }

            // ".names in1 in2 ... out" => inputs = tokenCount - 2
            numInputs = std::max(0, tokenCount - 2);
        } else if (line[0] == '.') {
            flushGate();
        } else if (inGate) {
            coverLines.push_back(line);
        }
    }

    flushGate();
    return local_freq;
}

LutTruthTable BenchmarkExtractor::computeTruthTable(
    int numInputs,
    const std::vector<std::string>& coverLines) {
    if (numInputs < 0 || numInputs > lutInputs_) return 0;
    if (coverLines.empty()) return 0;

    // 和 InnovusBatchEvaluator::getHexValue() 保持一致：
    // 1) bit0 对应 cube[0]
    // 2) 用第一条有效 SOP 行判断当前是 cover-1 还是 cover-0
    bool foundFirstRow = false;
    bool isCover0 = false;

    for (const auto& row : coverLines) {
        std::stringstream ss(row);
        std::string firstCube, firstOut;
        if (ss >> firstCube >> firstOut) {
            isCover0 = (firstOut == "0");
            foundFirstRow = true;
            break;
        }
    }

    if (!foundFirstRow) return 0;

    LutTruthTable truthTable = isCover0 ? kLutTruthTableAllOnes : LutTruthTable{0};
    const int rows = 1 << numInputs;   // 2^numInputs, bounded by kLutMaxInputs

    for (const auto& row : coverLines) {
        std::stringstream rss(row);
        std::string cube, outVal;
        if (!(rss >> cube >> outVal)) continue;

        for (int mask = 0; mask < rows; ++mask) {
            bool match = true;
            for (int j = 0; j < numInputs; ++j) {
                if (j >= static_cast<int>(cube.size())) break;
                if (cube[j] == '-') continue;

                bool bitValue = ((mask >> j) & 1) != 0;
                if ((cube[j] == '1' && !bitValue) ||
                    (cube[j] == '0' &&  bitValue)) {
                    match = false;
                    break;
                }
            }

            if (match) {
                const LutTruthTable bit = LutTruthTable{1} << mask;
                if (isCover0) truthTable &= ~bit;
                else          truthTable |= bit;
            }
        }
    }

    return truthTable;
}

void BenchmarkExtractor::exportTopHexFuncs(const std::string& outputPath, int topN) {
    const std::vector<RankedTruthTable> sorted_funcs = getTopTruthTables(topN);
    std::ofstream outFile(outputPath);
    outFile << "HexFunc,NumInputs,Frequency,Rank\n";

    int count = 0;

    for (const auto& func : sorted_funcs) {
        const TruthKey key{func.numInputs, func.truthTable};
        if (guaranteed_funcs_.count(key)) {
            outFile << std::hex << std::uppercase
                    << std::setw(kLutTruthTableHexDigits)
                    << std::setfill('0') << func.truthTable;
            outFile << "," << std::dec << func.numInputs;
            outFile << "," << func.frequency;
            outFile << "," << (count + 1) << "\n";
            count++;
        }
    }

    for (const auto& func : sorted_funcs) {
        if (topN > 0 && count >= topN) break;
        const TruthKey key{func.numInputs, func.truthTable};
        if (!guaranteed_funcs_.count(key)) {
            outFile << std::hex << std::uppercase
                    << std::setw(kLutTruthTableHexDigits)
                    << std::setfill('0') << func.truthTable;
            outFile << "," << std::dec << func.numInputs;
            outFile << "," << func.frequency;
            outFile << "," << (count + 1) << "\n";
            count++;
        }
    }

    std::cout << "[Extractor] Exported total " << count
              << " hexfuncs to " << outputPath << std::endl;
}

std::vector<RankedTruthTable> BenchmarkExtractor::getTopTruthTables(int topN) const {
    std::vector<std::pair<TruthKey, int>> sorted_funcs(
        frequency_map_.begin(), frequency_map_.end());
    std::sort(sorted_funcs.begin(), sorted_funcs.end(),
        [](const auto& a, const auto& b) {
            if (a.second != b.second) return a.second > b.second;
            if (a.first.numInputs != b.first.numInputs) {
                return a.first.numInputs < b.first.numInputs;
            }
            return a.first.truthTable < b.first.truthTable;
        });

    std::vector<RankedTruthTable> ranked;
    for (const auto& pair : sorted_funcs) {
        ranked.push_back(
            RankedTruthTable{
                pair.first.truthTable,
                pair.first.numInputs,
                pair.second});
        if (topN > 0 && static_cast<int>(ranked.size()) >= topN) break;
    }
    return ranked;
}

} // namespace fes
