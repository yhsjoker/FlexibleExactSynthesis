#include "fes/flow/RunManifest.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace fes {
namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

RunManifestEntry makeEntry(const std::string& caseId, RunStatus status) {
    RunManifestEntry entry;
    entry.caseId = caseId;
    entry.kind = "generate";
    entry.functionId = "HEX";
    entry.activityTag = "_10_10";
    entry.activityMode = "uniform";
    entry.status = status;
    entry.runtimeMs = 1.0;
    entry.outputPath = "out.blif";
    entry.reason = "";
    return entry;
}

void verifyStatusRoundTrip() {
    const fs::path path =
        fs::temp_directory_path() / "pono_test_run_manifest.csv";
    fs::remove(path);

    RunManifest manifest(path);
    manifest.update(makeEntry("success_case", RunStatus::kSuccess));
    manifest.update(makeEntry("failed_case", RunStatus::kFailed));
    manifest.update(makeEntry("timeout_case", RunStatus::kTimeout));
    require(manifest.flush(), "Manifest flush failed.");

    RunManifest loaded(path);
    require(loaded.load(), "Manifest load failed.");
    require(loaded.find("success_case")->status == RunStatus::kSuccess,
            "Success status did not round-trip.");
    require(loaded.find("failed_case")->status == RunStatus::kFailed,
            "Failed status did not round-trip.");
    require(loaded.find("timeout_case")->status == RunStatus::kTimeout,
            "Timeout status did not round-trip.");
    fs::remove(path);
}

void verifyResumeFiltering() {
    RunManifest manifest("/tmp/nonexistent_pono_manifest.csv");
    manifest.update(makeEntry("success_case", RunStatus::kSuccess));
    manifest.update(makeEntry("failed_case", RunStatus::kFailed));
    manifest.update(makeEntry("timeout_case", RunStatus::kTimeout));

    require(!manifest.shouldRun("success_case", ResumePolicy::kResume),
            "Resume should skip completed cases.");
    require(manifest.shouldRun("failed_case", ResumePolicy::kResume),
            "Resume should rerun failed cases.");
    require(manifest.shouldRun("timeout_case", ResumePolicy::kResume),
            "Resume should rerun timeout cases.");
    require(manifest.shouldRun("failed_case", ResumePolicy::kRerunFailed),
            "rerun_failed should rerun failed cases.");
    require(!manifest.shouldRun("timeout_case", ResumePolicy::kRerunFailed),
            "rerun_failed should not rerun timeout cases.");
    require(manifest.shouldRun("timeout_case", ResumePolicy::kRerunTimeout),
            "rerun_timeout should rerun timeout cases.");
    require(!manifest.shouldRun("failed_case", ResumePolicy::kRerunTimeout),
            "rerun_timeout should not rerun failed cases.");
}

}  // namespace
}  // namespace fes

int main() {
    try {
        fes::verifyStatusRoundTrip();
        fes::verifyResumeFiltering();
        std::cout << "All run manifest tests passed." << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "test_run_manifest failed: " << ex.what() << std::endl;
        return 1;
    }
}
