#pragma once

#include "fes/app/GenerateCli.h"
#include "fes/flow/RunManifest.h"

#include <filesystem>
#include <string>
#include <vector>

namespace fes::app {

struct EvaluationOptions {
    std::filesystem::path configPath;
    std::filesystem::path benchmarkDir;
    std::filesystem::path libraryDir;
    std::filesystem::path abcLocalLibraryDir;
    bool mappedFourWay = false;
    bool verify = false;
    bool help = false;
    ResumePolicy resumePolicy = ResumePolicy::kRunAll;
    int caseTimeoutMs = 0;
    std::string abcPath;
    std::filesystem::path pythonScriptPath;
    unsigned workerCount = 0;
    unsigned maxWorkerMemoryMb = 0;
    unsigned maxTotalMemoryMb = 0;
};

struct AppConfig {
    std::string command;
    GenerateOptions generate;
    EvaluationOptions evaluation;
};

AppConfig loadAppConfig(const std::filesystem::path& configPath,
                        int maxLutInputs);

EvaluationOptions parseEvaluationOptions(
    const std::vector<std::string>& args,
    int maxLutInputs);

}  // namespace fes::app
