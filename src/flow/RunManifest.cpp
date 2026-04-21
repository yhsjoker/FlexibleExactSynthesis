#include "fes/flow/RunManifest.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace fes {
namespace {

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

std::string escapeJsonString(const std::string& input) {
    std::ostringstream out;
    for (char c : input) {
        if (c == '\\') out << "\\\\";
        else if (c == '"') out << "\\\"";
        else if (c == '\n') out << "\\n";
        else if (c == '\r') out << "\\r";
        else if (c == '\t') out << "\\t";
        else out << c;
    }
    return out.str();
}

std::string csvEscape(const std::string& input) {
    const bool needsQuotes =
        input.find_first_of(",\"\r\n") != std::string::npos;
    if (!needsQuotes) {
        return input;
    }

    std::string escaped = "\"";
    for (char c : input) {
        if (c == '"') {
            escaped += "\"\"";
        } else {
            escaped += c;
        }
    }
    escaped += "\"";
    return escaped;
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

std::string statusForSummary(const RunSummary& summary) {
    if (summary.timeoutCases > 0) {
        return "timeout";
    }
    if (summary.failedCases > 0) {
        return "failed";
    }
    if (summary.successCases > 0 || summary.skippedCases > 0) {
        return "success";
    }
    return "skipped";
}

}  // namespace

std::string runStatusToString(RunStatus status) {
    switch (status) {
        case RunStatus::kPending: return "pending";
        case RunStatus::kRunning: return "running";
        case RunStatus::kSuccess: return "success";
        case RunStatus::kFailed: return "failed";
        case RunStatus::kTimeout: return "timeout";
        case RunStatus::kSkipped: return "skipped";
        case RunStatus::kResumed: return "resumed";
    }
    return "failed";
}

RunStatus runStatusFromString(const std::string& text) {
    const std::string value = toLowerCopy(text);
    if (value == "pending") return RunStatus::kPending;
    if (value == "running") return RunStatus::kRunning;
    if (value == "success") return RunStatus::kSuccess;
    if (value == "failed") return RunStatus::kFailed;
    if (value == "timeout") return RunStatus::kTimeout;
    if (value == "skipped") return RunStatus::kSkipped;
    if (value == "resumed") return RunStatus::kResumed;
    return RunStatus::kFailed;
}

std::string resumePolicyToString(ResumePolicy policy) {
    switch (policy) {
        case ResumePolicy::kRunAll: return "run_all";
        case ResumePolicy::kSkipCompleted: return "skip_completed";
        case ResumePolicy::kResume: return "resume";
        case ResumePolicy::kRerunFailed: return "rerun_failed";
        case ResumePolicy::kRerunTimeout: return "rerun_timeout";
    }
    return "run_all";
}

ResumePolicy resumePolicyFromString(const std::string& text) {
    const std::string value = toLowerCopy(text);
    if (value == "run_all" || value == "all") {
        return ResumePolicy::kRunAll;
    }
    if (value == "skip_completed" || value == "skip-completed") {
        return ResumePolicy::kSkipCompleted;
    }
    if (value == "resume") {
        return ResumePolicy::kResume;
    }
    if (value == "rerun_failed" || value == "rerun-failed" ||
        value == "failed") {
        return ResumePolicy::kRerunFailed;
    }
    if (value == "rerun_timeout" || value == "rerun-timeout" ||
        value == "timeout") {
        return ResumePolicy::kRerunTimeout;
    }
    throw std::runtime_error("Unknown resume policy: " + text);
}

RunManifest::RunManifest(fs::path path)
    : path_(std::move(path)) {}

bool RunManifest::load() {
    entries_.clear();

    std::ifstream input(path_);
    if (!input.is_open()) {
        return false;
    }

    std::string line;
    bool isHeader = true;
    while (std::getline(input, line)) {
        if (isHeader) {
            isHeader = false;
            continue;
        }
        if (line.empty()) {
            continue;
        }

        const std::vector<std::string> fields = parseCsvRow(line);
        if (fields.size() < 9) {
            continue;
        }

        RunManifestEntry entry;
        entry.caseId = fields[0];
        entry.kind = fields[1];
        entry.functionId = fields[2];
        entry.activityTag = fields[3];
        entry.activityMode = fields[4];
        entry.status = runStatusFromString(fields[5]);
        try {
            entry.runtimeMs = std::stod(fields[6]);
        } catch (...) {
            entry.runtimeMs = 0.0;
        }
        entry.outputPath = fields[7];
        entry.reason = fields[8];

        if (!entry.caseId.empty()) {
            entries_[entry.caseId] = std::move(entry);
        }
    }
    return true;
}

bool RunManifest::has(const std::string& caseId) const {
    return entries_.find(caseId) != entries_.end();
}

const RunManifestEntry* RunManifest::find(const std::string& caseId) const {
    const auto it = entries_.find(caseId);
    return it == entries_.end() ? nullptr : &it->second;
}

bool RunManifest::shouldRun(const std::string& caseId,
                            ResumePolicy policy) const {
    const RunManifestEntry* entry = find(caseId);
    if (policy == ResumePolicy::kRunAll) {
        return true;
    }
    if (entry == nullptr) {
        return policy == ResumePolicy::kResume ||
               policy == ResumePolicy::kSkipCompleted;
    }
    if (policy == ResumePolicy::kSkipCompleted ||
        policy == ResumePolicy::kResume) {
        return entry->status != RunStatus::kSuccess;
    }
    if (policy == ResumePolicy::kRerunFailed) {
        return entry->status == RunStatus::kFailed;
    }
    if (policy == ResumePolicy::kRerunTimeout) {
        return entry->status == RunStatus::kTimeout;
    }
    return true;
}

bool RunManifest::isResumeAttempt(const std::string& caseId,
                                  ResumePolicy policy) const {
    if (policy == ResumePolicy::kRunAll) {
        return false;
    }
    const RunManifestEntry* entry = find(caseId);
    return entry != nullptr && shouldRun(caseId, policy);
}

void RunManifest::update(const RunManifestEntry& entry) {
    if (!entry.caseId.empty()) {
        entries_[entry.caseId] = entry;
    }
}

bool RunManifest::flush() const {
    if (path_.has_parent_path()) {
        fs::create_directories(path_.parent_path());
    }

    std::ofstream output(path_);
    if (!output.is_open()) {
        return false;
    }

    output << "case_id,kind,function_id,activity_tag,activity_mode,status,"
           << "runtime_ms,output_path,reason\n";
    for (const auto& item : entries_) {
        const RunManifestEntry& entry = item.second;
        output << csvEscape(entry.caseId) << ","
               << csvEscape(entry.kind) << ","
               << csvEscape(entry.functionId) << ","
               << csvEscape(entry.activityTag) << ","
               << csvEscape(entry.activityMode) << ","
               << runStatusToString(entry.status) << ","
               << std::fixed << std::setprecision(3) << entry.runtimeMs << ","
               << csvEscape(entry.outputPath) << ","
               << csvEscape(entry.reason) << "\n";
    }
    return true;
}

bool writeRunSummaryJson(const fs::path& path, const RunSummary& summary) {
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path());
    }

    std::ofstream output(path);
    if (!output.is_open()) {
        return false;
    }

    output << "{\n";
    output << "  \"command\": \"" << escapeJsonString(summary.command) << "\",\n";
    output << "  \"activity_mode\": \""
           << escapeJsonString(summary.activityMode) << "\",\n";
    output << "  \"status\": \"" << statusForSummary(summary) << "\",\n";
    output << "  \"total_cases\": " << summary.totalCases << ",\n";
    output << "  \"run_cases\": " << summary.runCases << ",\n";
    output << "  \"skipped_cases\": " << summary.skippedCases << ",\n";
    output << "  \"resumed_cases\": " << summary.resumedCases << ",\n";
    output << "  \"success_cases\": " << summary.successCases << ",\n";
    output << "  \"failed_cases\": " << summary.failedCases << ",\n";
    output << "  \"timeout_cases\": " << summary.timeoutCases << ",\n";
    output << "  \"output_csv\": \""
           << escapeJsonString(summary.outputCsv.string()) << "\",\n";
    output << "  \"manifest_csv\": \""
           << escapeJsonString(summary.manifestCsv.string()) << "\"\n";
    output << "}\n";
    return true;
}

}  // namespace fes
