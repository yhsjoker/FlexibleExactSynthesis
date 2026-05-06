#include "fes/utils/CellLibraryLoader.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef FES_PROJECT_ROOT
#define FES_PROJECT_ROOT "."
#endif

namespace fes {

namespace {

std::string trimCopy(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }

    const size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
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
            fields.push_back(current);
            current.clear();
        } else if (c == '"') {
            inQuotes = true;
        } else {
            current += c;
        }
    }

    fields.push_back(current);
    return fields;
}

int countExpressionInputs(const std::string& expression) {
    static const std::regex identRe(R"([A-Za-z_][A-Za-z0-9_]*)");
    std::unordered_set<std::string> identifiers;

    for (std::sregex_iterator it(expression.begin(), expression.end(), identRe);
         it != std::sregex_iterator();
         ++it) {
        identifiers.insert(it->str());
    }

    return static_cast<int>(identifiers.size());
}

std::filesystem::path compileTimeProjectRoot() {
    return std::filesystem::weakly_canonical(
        std::filesystem::path(FES_PROJECT_ROOT));
}

std::filesystem::path resolveParserScriptPath(
    const std::string& parserScriptPath) {
    namespace fs = std::filesystem;
    fs::path scriptPath = parserScriptPath.empty()
                              ? fs::path("scripts") / "parse_liberty.py"
                              : fs::path(parserScriptPath);
    if (scriptPath.is_absolute()) {
        return scriptPath.lexically_normal();
    }
    return (compileTimeProjectRoot() / scriptPath).lexically_normal();
}

}  // namespace

std::vector<StandardCell> CellLibraryLoader::loadOrGenerate(
    const std::string& csvPath,
    const std::string& libPath,
    int maxInputs,
    const std::string& parserScriptPath) {
    namespace fs = std::filesystem;

    const fs::path csvFile(csvPath);
    const fs::path libFile(libPath);
    std::vector<StandardCell> cells;

    if (!fs::exists(csvFile)) {
        if (!fs::exists(libFile)) {
            std::cerr << "[CellLibraryLoader] Missing CSV and Liberty file: "
                      << csvFile << ", " << libFile << "\n";
            return cells;
        }

        const fs::path scriptPath = resolveParserScriptPath(parserScriptPath);
        if (!fs::exists(scriptPath)) {
            std::cerr << "[CellLibraryLoader] Parser script not found: "
                      << scriptPath << "\n";
            return cells;
        }

        if (csvFile.has_parent_path()) {
            fs::create_directories(csvFile.parent_path());
        }

        std::stringstream cmd;
        cmd << "python3 "
            << shellQuote(scriptPath.string()) << " "
            << shellQuote(libFile.string()) << " "
            << shellQuote(csvFile.string());

        std::cout << "[CellLibraryLoader] Generating " << csvFile
                  << " from " << libFile << "...\n";
        const int ret = std::system(cmd.str().c_str());
        if (ret != 0 || !fs::exists(csvFile)) {
            std::cerr << "[CellLibraryLoader] Failed to generate CSV: "
                      << csvFile << "\n";
            return cells;
        }
    }

    std::ifstream csvStream(csvFile);
    if (!csvStream.is_open()) {
        std::cerr << "[CellLibraryLoader] Could not open CSV: "
                  << csvFile << "\n";
        return cells;
    }

    std::string line;
    bool isFirstRow = true;
    while (std::getline(csvStream, line)) {
        line = trimCopy(line);
        if (line.empty()) {
            continue;
        }

        if (isFirstRow) {
            isFirstRow = false;
            if (line.rfind("CellName,", 0) == 0) {
                continue;
            }
        }

        const std::vector<std::string> fields = parseCsvRow(line);
        if (fields.size() != 4) {
            continue;
        }

        try {
            StandardCell cell;
            cell.name = trimCopy(fields[0]);
            cell.area = std::stod(trimCopy(fields[1]));
            cell.leakage = std::stod(trimCopy(fields[2]));
            cell.expression = trimCopy(fields[3]);

            const int inputCount = countExpressionInputs(cell.expression);
            const bool withinLimit =
                maxInputs <= 0 || inputCount <= maxInputs;

            if (!cell.name.empty() && !cell.expression.empty() && withinLimit) {
                cells.push_back(std::move(cell));
            }
        } catch (const std::exception&) {
            continue;
        }
    }

    std::cout << "[CellLibraryLoader] Loaded " << cells.size()
              << " standard cells (maxInputs=" << maxInputs << ").\n";
    return cells;
}

}  // namespace fes
