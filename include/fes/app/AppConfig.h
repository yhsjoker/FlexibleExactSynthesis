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
    std::filesystem::path outputDir;
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

struct SingleBlifOptions {
    std::filesystem::path configPath;
    std::filesystem::path blifPath;
    std::filesystem::path libraryDir;
    std::filesystem::path abcLocalLibraryDir;
    std::filesystem::path outputDir;
    std::filesystem::path resultJsonPath;
    std::vector<double> inputProbs;
    std::vector<double> inputActs;
    bool jsonStdout = false;
    bool verify = false;
    bool help = false;
    bool emitBlifContent = false;
    std::string abcPath;
    std::filesystem::path pythonScriptPath;
};

struct AppConfig {
    std::string command;
    GenerateOptions generate;
    EvaluationOptions evaluation;
    SingleBlifOptions singleOptimize;
    SingleBlifOptions singleEvaluate;
};

AppConfig loadAppConfig(const std::filesystem::path& configPath,
                        int maxLutInputs);

EvaluationOptions parseEvaluationOptions(
    const std::vector<std::string>& args,
    int maxLutInputs);

SingleBlifOptions parseSingleBlifOptions(const std::vector<std::string>& args,
                                         int maxLutInputs,
                                         const std::string& commandName);

}  // namespace fes::app
