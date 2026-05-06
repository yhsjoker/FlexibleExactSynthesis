#include "fes/app/AppConfig.h"
#include "fes/app/GenerateCli.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace fes {
namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void requireThrows(const std::vector<std::string>& args,
                   const std::string& label) {
    try {
        (void)app::parseGenerateOptions(args, 6);
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(label + " should fail.");
}

void verifyDefaultUniform() {
    const app::GenerateOptions opts =
        app::parseGenerateOptions({"--mode", "exhaustive"}, 6);
    const auto patterns = generateActivityPatterns(
        4, opts.activityPatternSpec);
    require(patterns.size() == 9,
            "Default generate options should preserve 9-pattern uniform sweep.");
    require(opts.mode == "exhaustive", "Function generation mode changed.");
}

void verifyCartesianCli() {
    const app::GenerateOptions opts = app::parseGenerateOptions(
        {"--activity-mode", "cartesian",
         "--activity-levels", "0.2,0.4,0.6,0.8"},
        6);
    const auto patterns = generateActivityPatterns(
        4, opts.activityPatternSpec);
    require(patterns.size() == 256,
            "Cartesian CLI mode should produce 4^4 patterns.");
    require(patterns[1][3] == 0.4,
            "Cartesian CLI mode should preserve deterministic ordering.");
}

void verifyExplicitCli() {
    const app::GenerateOptions opts = app::parseGenerateOptions(
        {"--activity-mode", "explicit",
         "--activity-explicit", "0.1,0.2,0.3,0.4;0.4,0.3,0.2,0.1"},
        6);
    const auto patterns = generateActivityPatterns(
        4, opts.activityPatternSpec);
    require(patterns.size() == 2,
            "Explicit CLI mode should preserve the explicit pattern count.");
    require(patterns[0][1] == 0.2 && patterns[1][0] == 0.4,
            "Explicit CLI mode parsed incorrect values.");
}

void verifyInvalidCombinations() {
    requireThrows({"--activity-mode", "cartesian"},
                  "cartesian without levels");
    requireThrows({"--activity-mode", "explicit"},
                  "explicit without explicit patterns");
    requireThrows({"--activity-mode", "explicit",
                   "--activity-levels", "0.2,0.4",
                   "--activity-explicit", "0.1,0.2"},
                  "explicit with levels");
    requireThrows({"--activity-mode", "unknown"},
                  "unknown activity mode");
}

void writeTextFile(const fs::path& path, const std::string& text) {
    std::ofstream output(path);
    if (!output.is_open()) {
        throw std::runtime_error("Failed to create test config.");
    }
    output << text;
}

void verifyConfigParsingAndCliPrecedence() {
    const fs::path path =
        fs::temp_directory_path() / "pono_generate_config_test.json";
    writeTextFile(
        path,
        "{\n"
        "  \"command\": \"generate\",\n"
        "  \"tools\": {\"abc_path\": \"/tmp/abc_from_tools\"},\n"
        "  \"dependencies\": {\n"
        "    \"abc_path\": \"/tmp/abc_from_dependencies\"\n"
        "  },\n"
        "  \"run\": {\"resume_policy\": \"resume\", \"case_timeout_ms\": 42,\n"
        "           \"threads\": 8, \"max_worker_memory_mb\": 1024,\n"
        "           \"max_total_memory_mb\": 3072},\n"
        "  \"generate\": {\n"
        "    \"k\": 4,\n"
        "    \"num_functions\": 7,\n"
        "    \"function_source\": \"exhaustive\",\n"
        "    \"output_dir\": \"results_repo/from_config\",\n"
        "    \"timeouts_ms\": {\"sat\": 123, \"optimization\": 456},\n"
        "    \"activity\": {\"mode\": \"cartesian\", \"levels\": [0.2, 0.4]}\n"
        "  }\n"
        "}\n");

    const app::GenerateOptions configOnly =
        app::parseGenerateOptions({"--config", path.string()}, 6);
    require(configOnly.numFunctions == 7,
            "Config num_functions was not parsed.");
    require(configOnly.resumePolicy == ResumePolicy::kResume,
            "Config resume policy was not parsed.");
    require(configOnly.caseTimeoutMs == 42,
            "Config case timeout was not parsed.");
    require(configOnly.workerCount == 8,
            "Config run.threads was not parsed.");
    require(configOnly.maxWorkerMemoryMb == 1024 &&
                configOnly.maxTotalMemoryMb == 3072,
            "Config memory budget fields were not parsed.");
    require(configOnly.satTimeoutMs == 123 && configOnly.optTimeoutMs == 456,
            "Grouped generation timeout settings were not parsed.");
    require(configOnly.abcPath == "/tmp/abc_from_dependencies",
            "dependencies.abc_path should override legacy tools.abc_path.");
    require(generateActivityPatterns(3, configOnly.activityPatternSpec).size() == 8,
            "Config cartesian activity was not parsed.");

    const app::GenerateOptions overridden =
        app::parseGenerateOptions(
            {"--config", path.string(), "--num", "2", "--activity-mode", "uniform"},
            6);
    require(overridden.numFunctions == 2,
            "CLI --num should override config num_functions.");
    require(generateActivityPatterns(4, overridden.activityPatternSpec).size() == 9,
            "CLI --activity-mode uniform should override config activity.");

    fs::remove(path);
}

void verifyInvalidConfigDetection() {
    const fs::path path =
        fs::temp_directory_path() / "pono_generate_invalid_config_test.json";
    writeTextFile(
        path,
        "{\n"
        "  \"command\": \"generate\",\n"
        "  \"generate\": {\n"
        "    \"activity\": {\"mode\": \"cartesian\"}\n"
        "  }\n"
        "}\n");
    requireThrows({"--config", path.string()},
                  "cartesian config without levels");
    fs::remove(path);
}

void verifyEvaluationConfigParsing() {
    const fs::path path =
        fs::temp_directory_path() / "pono_evaluate_config_test.json";
    writeTextFile(
        path,
        "{\n"
        "  \"command\": \"benchmark\",\n"
        "  \"dependencies\": {\"abc_path\": \"/tmp/abc_for_eval\"},\n"
        "  \"run\": {\"resume_policy\": \"run_all\", \"threads\": 4,\n"
        "           \"max_worker_memory_mb\": 2048,\n"
        "           \"max_total_memory_mb\": 4096},\n"
        "  \"benchmark\": {\n"
        "    \"benchmark_dir\": \"/tmp/benches\",\n"
        "    \"library_dir\": \"results_repo/lib\",\n"
        "    \"verify\": true,\n"
        "    \"mode\": \"standard\",\n"
        "    \"timeouts_ms\": {\"case\": 700}\n"
        "  }\n"
        "}\n");

    const app::EvaluationOptions opts =
        app::parseEvaluationOptions({"--config", path.string()}, 6);
    require(opts.benchmarkDir == fs::path("/tmp/benches"),
            "Evaluation benchmark_dir was not parsed.");
    require(opts.libraryDir == fs::path("results_repo/lib"),
            "Evaluation library_dir was not parsed.");
    require(opts.verify, "Evaluation verify was not parsed.");
    require(opts.abcPath == "/tmp/abc_for_eval",
            "Evaluation dependencies.abc_path was not parsed.");
    require(opts.caseTimeoutMs == 700,
            "Evaluation grouped case timeout was not parsed.");
    require(opts.workerCount == 4,
            "Evaluation run.threads was not parsed.");
    require(opts.maxWorkerMemoryMb == 2048 &&
                opts.maxTotalMemoryMb == 4096,
            "Evaluation memory budget fields were not parsed.");
    fs::remove(path);
}

void verifyMemoryBudgetWorkerReduction() {
    require(app::computeEffectiveWorkerCount(8, 1024, 3072) == 3,
            "Memory budget should reduce worker count by total/per-worker.");
    require(app::computeEffectiveWorkerCount(8, 0, 3072) == 8,
            "Missing per-worker budget should preserve requested workers.");
    require(app::computeEffectiveWorkerCount(8, 1024, 0) == 8,
            "Missing total budget should preserve requested workers.");
    require(app::computeEffectiveWorkerCount(8, 4096, 1024) == 1,
            "Budget below one worker estimate should still allow one worker.");
    require(app::computeEffectiveWorkerCount(0, 1024, 4096) == 1,
            "Requested zero workers should normalize to one in the helper.");
}

void verifyRemovedSynthLibOption() {
    requireThrows({"--synth-lib", "resources/synth_2input.csv"},
                  "removed --synth-lib option");
}

void verifyOutputPathResolution() {
    const fs::path projectRoot = "/repo";
    const fs::path resultsRepo = projectRoot / "results_repo";
    const fs::path defaultOut = resultsRepo / "generate_exhaustive_k4_stamp";

    require(app::resolveGenerateOutputDir(
                projectRoot, resultsRepo, "", defaultOut) == defaultOut,
            "Empty --out should use default results_repo path.");
    require(app::resolveGenerateOutputDir(
                projectRoot, resultsRepo, "activity", defaultOut) ==
                resultsRepo / "activity",
            "Relative --out should be rooted under results_repo.");
    require(app::resolveGenerateOutputDir(
                projectRoot, resultsRepo, "results_repo/activity", defaultOut) ==
                resultsRepo / "activity",
            "results_repo-prefixed --out should be rooted under project root.");
    require(app::resolveGenerateOutputDir(
                projectRoot, resultsRepo, "/tmp/activity", defaultOut) ==
                fs::path("/tmp/activity"),
            "Absolute --out should remain an explicit output override.");

    try {
        (void)app::resolveGenerateOutputDir(
            projectRoot, resultsRepo, "../outside", defaultOut);
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error("Escaping relative --out path should fail.");
}

}  // namespace
}  // namespace fes

int main() {
    try {
        fes::verifyDefaultUniform();
        fes::verifyCartesianCli();
        fes::verifyExplicitCli();
        fes::verifyInvalidCombinations();
        fes::verifyConfigParsingAndCliPrecedence();
        fes::verifyInvalidConfigDetection();
        fes::verifyEvaluationConfigParsing();
        fes::verifyMemoryBudgetWorkerReduction();
        fes::verifyRemovedSynthLibOption();
        fes::verifyOutputPathResolution();
        std::cout << "All generate CLI tests passed." << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "test_generate_cli failed: " << ex.what() << std::endl;
        return 1;
    }
}
