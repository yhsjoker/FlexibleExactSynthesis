#pragma once

#include "fes/flow/ActivityPatternGenerator.h"
#include "fes/flow/RunManifest.h"

#include <filesystem>
#include <string>
#include <vector>

namespace fes::app {

struct GenerateOptions {
    std::filesystem::path configPath;
    int k = 4;
    int numFunctions = 222;
    std::string mode = "exhaustive";
    std::filesystem::path benchmarkDir;
    std::filesystem::path outputDir;
    ActivityPatternSpec activityPatternSpec;
    std::string activityModeName = "uniform";
    ResumePolicy resumePolicy = ResumePolicy::kRunAll;
    int satTimeoutMs = 10000;
    int optTimeoutMs = 60000;
    int caseTimeoutMs = 0;
    std::string abcPath;
    std::filesystem::path genlibPath;
    std::filesystem::path libertyPath;
    std::filesystem::path pythonScriptPath;
    std::filesystem::path standardCellCsvPath;
    unsigned workerCount = 0;
    bool verify = false;
    bool help = false;
};

GenerateOptions parseGenerateOptions(const std::vector<std::string>& args,
                                     int maxLutInputs);

std::filesystem::path resolveGenerateOutputDir(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& resultsRepoDir,
    const std::filesystem::path& requestedOutputDir,
    const std::filesystem::path& defaultOutputDir);

}  // namespace fes::app
