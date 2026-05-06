#include "fes/app/GenerateCli.h"

#include "fes/app/AppConfig.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace fes::app {
namespace {

std::string trimCopy(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }

    const size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
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

double parseActivityValue(const std::string& text,
                          const std::string& optionName) {
    try {
        size_t consumed = 0;
        const double value = std::stod(trimCopy(text), &consumed);
        if (consumed != trimCopy(text).size()) {
            throw std::runtime_error("");
        }
        return ActivityPatternGenerator::stableActivityValue(value);
    } catch (const std::exception& e) {
        throw std::runtime_error(
            "Invalid activity value for " + optionName + ": " + text +
            " (" + e.what() + ")");
    }
}

std::vector<double> parseActivityLevels(const std::string& text,
                                        const std::string& optionName) {
    std::vector<double> levels;
    std::stringstream input(text);
    std::string token;
    while (std::getline(input, token, ',')) {
        token = trimCopy(token);
        if (token.empty()) {
            throw std::runtime_error(optionName + " contains an empty value.");
        }
        levels.push_back(parseActivityValue(token, optionName));
    }

    if (levels.empty()) {
        throw std::runtime_error(optionName + " must contain at least one value.");
    }
    return levels;
}

std::vector<std::vector<double>> parseExplicitActivityPatterns(
    const std::string& text) {
    std::vector<std::vector<double>> patterns;
    std::stringstream input(text);
    std::string patternText;
    while (std::getline(input, patternText, ';')) {
        patternText = trimCopy(patternText);
        if (patternText.empty()) {
            throw std::runtime_error(
                "--activity-explicit contains an empty pattern.");
        }
        patterns.push_back(
            parseActivityLevels(patternText, "--activity-explicit"));
    }

    if (patterns.empty()) {
        throw std::runtime_error(
            "--activity-explicit must contain at least one pattern.");
    }
    return patterns;
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

fs::path joinRelativeOutputPath(const fs::path& projectRoot,
                                const fs::path& resultsRepoDir,
                                const fs::path& requestedOutputDir) {
    fs::path requested = requestedOutputDir.lexically_normal();
    if (requested.empty() || requested == ".") {
        return resultsRepoDir;
    }

    fs::path candidate;
    auto first = requested.begin();
    if (first != requested.end() &&
        *first == resultsRepoDir.filename()) {
        candidate = projectRoot / requested;
    } else {
        candidate = resultsRepoDir / requested;
    }
    candidate = candidate.lexically_normal();

    if (!isPathWithinOrEqual(resultsRepoDir, candidate)) {
        throw std::runtime_error(
            "--out must stay under results_repo when passed as a relative path.");
    }
    return candidate;
}

fs::path findConfigPath(const std::vector<std::string>& args) {
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string prefix = "--config=";
        if (args[i] == "--config") {
            if (i + 1 >= args.size()) {
                throw std::runtime_error("Missing value for option --config");
            }
            return args[i + 1];
        }
        if (args[i].rfind(prefix, 0) == 0) {
            return args[i].substr(prefix.size());
        }
    }
    return {};
}

}  // namespace

GenerateOptions parseGenerateOptions(const std::vector<std::string>& args,
                                     int maxLutInputs) {
    const fs::path configPath = findConfigPath(args);
    GenerateOptions opts;
    if (!configPath.empty()) {
        AppConfig config = loadAppConfig(configPath, maxLutInputs);
        if (!config.command.empty() && config.command != "generate") {
            throw std::runtime_error(
                "Config command is not compatible with 'generate': " +
                config.command);
        }
        opts = config.generate;
        opts.configPath = configPath;
    }

    std::string activityMode = "uniform";
    if (opts.activityModeName != "uniform") {
        activityMode = opts.activityModeName;
    }
    bool hasActivityModeFlag = false;
    bool hasActivityLevels = false;
    bool hasActivityExplicit = false;
    std::vector<double> activityLevels;
    std::vector<std::vector<double>> explicitPatterns;

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
        if (arg == "--resume") {
            opts.resumePolicy = ResumePolicy::kResume;
            continue;
        }
        if (arg == "--skip-completed") {
            opts.resumePolicy = ResumePolicy::kSkipCompleted;
            continue;
        }
        if (consumeOption(args, i, "--config", &value)) {
            opts.configPath = value;
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
        if (consumeOption(args, i, "--activity-mode", &value)) {
            activityMode = toLowerCopy(trimCopy(value));
            opts.activityModeName = activityMode;
            hasActivityModeFlag = true;
            continue;
        }
        if (consumeOption(args, i, "--activity-levels", &value)) {
            activityLevels = parseActivityLevels(value, "--activity-levels");
            hasActivityLevels = true;
            continue;
        }
        if (consumeOption(args, i, "--activity-explicit", &value)) {
            explicitPatterns = parseExplicitActivityPatterns(value);
            hasActivityExplicit = true;
            continue;
        }
        if (consumeOption(args, i, "--rerun", &value)) {
            opts.resumePolicy = resumePolicyFromString("rerun_" + value);
            continue;
        }
        if (consumeOption(args, i, "--sat-timeout-ms", &value)) {
            opts.satTimeoutMs = parseIntOption(value, "--sat-timeout-ms");
            continue;
        }
        if (consumeOption(args, i, "--opt-timeout-ms", &value)) {
            opts.optTimeoutMs = parseIntOption(value, "--opt-timeout-ms");
            continue;
        }
        if (consumeOption(args, i, "--case-timeout-ms", &value)) {
            opts.caseTimeoutMs = parseIntOption(value, "--case-timeout-ms");
            continue;
        }

        throw std::runtime_error("Unknown generate option: " + arg);
    }

    if (opts.k <= 0 || opts.k > maxLutInputs) {
        throw std::runtime_error(
            "--k must be in the range [1, " + std::to_string(maxLutInputs) + "]");
    }
    if (opts.numFunctions < 0) {
        throw std::runtime_error("--num must be non-negative.");
    }
    if (opts.mode != "exhaustive" && opts.mode != "benchmark") {
        throw std::runtime_error("--mode must be 'exhaustive' or 'benchmark'.");
    }
    if (opts.satTimeoutMs < 0 || opts.optTimeoutMs < 0 ||
        opts.caseTimeoutMs < 0) {
        throw std::runtime_error("Timeout values must be non-negative.");
    }

    if (activityMode != "uniform" &&
        activityMode != "cartesian" &&
        activityMode != "explicit") {
        throw std::runtime_error(
            "--activity-mode must be 'uniform', 'cartesian', or 'explicit'.");
    }
    if (hasActivityExplicit && activityMode != "explicit") {
        throw std::runtime_error(
            "--activity-explicit requires --activity-mode explicit.");
    }
    if (hasActivityLevels && activityMode == "explicit") {
        throw std::runtime_error(
            "--activity-levels cannot be used with --activity-mode explicit.");
    }

    if (activityMode == "cartesian") {
        if (!hasActivityLevels) {
            if (!hasActivityModeFlag && !configPath.empty() &&
                opts.activityModeName == "cartesian") {
                return opts;
            }
            throw std::runtime_error(
                "--activity-mode cartesian requires --activity-levels.");
        }
        opts.activityPatternSpec =
            ActivityPatternSpec::CartesianGrid(activityLevels);
        opts.activityModeName = "cartesian";
    } else if (activityMode == "explicit") {
        if (!hasActivityExplicit) {
            if (!hasActivityModeFlag && !configPath.empty() &&
                opts.activityModeName == "explicit") {
                return opts;
            }
            throw std::runtime_error(
                "--activity-mode explicit requires --activity-explicit.");
        }
        opts.activityPatternSpec =
            ActivityPatternSpec::ExplicitList(explicitPatterns);
        opts.activityModeName = "explicit";
    } else if (hasActivityLevels) {
        opts.activityPatternSpec =
            ActivityPatternSpec::UniformSweep(activityLevels);
        opts.activityModeName = "uniform";
    } else if (activityMode == "uniform" && hasActivityModeFlag) {
        opts.activityPatternSpec = ActivityPatternSpec::UniformSweep();
        opts.activityModeName = "uniform";
    }

    return opts;
}

fs::path resolveGenerateOutputDir(const fs::path& projectRoot,
                                  const fs::path& resultsRepoDir,
                                  const fs::path& requestedOutputDir,
                                  const fs::path& defaultOutputDir) {
    if (requestedOutputDir.empty()) {
        return defaultOutputDir.lexically_normal();
    }
    if (requestedOutputDir.is_absolute()) {
        return requestedOutputDir.lexically_normal();
    }
    return joinRelativeOutputPath(
        projectRoot, resultsRepoDir, requestedOutputDir);
}

unsigned computeEffectiveWorkerCount(unsigned requestedWorkerCount,
                                     unsigned maxWorkerMemoryMb,
                                     unsigned maxTotalMemoryMb) {
    const unsigned requested = std::max(1u, requestedWorkerCount);
    if (maxWorkerMemoryMb == 0 || maxTotalMemoryMb == 0) {
        return requested;
    }

    const unsigned budgetedWorkers =
        std::max(1u, maxTotalMemoryMb / maxWorkerMemoryMb);
    return std::max(1u, std::min(requested, budgetedWorkers));
}

}  // namespace fes::app
