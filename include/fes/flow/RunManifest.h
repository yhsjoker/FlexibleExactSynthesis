#pragma once

#include <filesystem>
#include <map>
#include <string>

namespace fes {

enum class RunStatus {
    kPending,
    kRunning,
    kSuccess,
    kFailed,
    kTimeout,
    kSkipped,
    kResumed,
};

enum class ResumePolicy {
    kRunAll,
    kSkipCompleted,
    kResume,
    kRerunFailed,
    kRerunTimeout,
};

struct RunManifestEntry {
    std::string caseId;
    std::string kind;
    std::string functionId;
    std::string activityTag;
    std::string activityMode;
    RunStatus status = RunStatus::kPending;
    double runtimeMs = 0.0;
    std::string outputPath;
    std::string reason;
};

struct RunSummary {
    std::string command;
    std::string activityMode;
    std::filesystem::path outputCsv;
    std::filesystem::path manifestCsv;
    std::size_t totalCases = 0;
    std::size_t runCases = 0;
    std::size_t skippedCases = 0;
    std::size_t resumedCases = 0;
    std::size_t successCases = 0;
    std::size_t failedCases = 0;
    std::size_t timeoutCases = 0;
};

std::string runStatusToString(RunStatus status);
RunStatus runStatusFromString(const std::string& text);
std::string resumePolicyToString(ResumePolicy policy);
ResumePolicy resumePolicyFromString(const std::string& text);

class RunManifest {
public:
    explicit RunManifest(std::filesystem::path path);

    bool load();
    bool has(const std::string& caseId) const;
    const RunManifestEntry* find(const std::string& caseId) const;
    bool shouldRun(const std::string& caseId, ResumePolicy policy) const;
    bool isResumeAttempt(const std::string& caseId, ResumePolicy policy) const;

    void update(const RunManifestEntry& entry);
    bool flush() const;
    const std::map<std::string, RunManifestEntry>& entries() const {
        return entries_;
    }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
    std::map<std::string, RunManifestEntry> entries_;
};

bool writeRunSummaryJson(const std::filesystem::path& path,
                         const RunSummary& summary);

}  // namespace fes
