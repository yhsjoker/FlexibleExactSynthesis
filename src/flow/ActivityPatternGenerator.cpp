#include "fes/flow/ActivityPatternGenerator.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fes {
namespace {

constexpr int kLegacyUniformLevelCount = 9;
constexpr double kLegacyUniformStep = 0.1;
constexpr long long kStableActivityScale = 1000000000LL;

double canonicalizeActivityValue(double value) {
    if (!std::isfinite(value)) {
        throw std::runtime_error("Activity pattern value must be finite.");
    }
    if (value < -ActivityPatternGenerator::kComparisonTolerance ||
        value > 1.0 + ActivityPatternGenerator::kComparisonTolerance) {
        throw std::runtime_error(
            "Activity pattern value must be in the normalized range [0, 1].");
    }

    const double clipped = std::min(1.0, std::max(0.0, value));
    const double scaled =
        std::round(clipped * static_cast<double>(kStableActivityScale));
    return scaled / static_cast<double>(kStableActivityScale);
}

std::vector<double> normalizeLevels(const std::vector<double>& levels) {
    if (levels.empty()) {
        throw std::runtime_error("Activity pattern levels must not be empty.");
    }

    std::vector<double> normalized;
    normalized.reserve(levels.size());
    std::set<std::string> seen;
    for (double level : levels) {
        const double value = canonicalizeActivityValue(level);
        const std::string key =
            ActivityPatternGenerator::stableValueString(value);
        if (seen.insert(key).second) {
            normalized.push_back(value);
        }
    }
    return normalized;
}

void appendUniquePattern(std::vector<std::vector<double>>* patterns,
                         std::set<std::string>* seen,
                         const std::vector<double>& pattern) {
    std::vector<double> normalized;
    normalized.reserve(pattern.size());
    for (double value : pattern) {
        normalized.push_back(canonicalizeActivityValue(value));
    }

    const std::string key =
        ActivityPatternGenerator::stablePatternKey(normalized);
    if (seen->insert(key).second) {
        patterns->push_back(std::move(normalized));
    }
}

void generateCartesianRecursive(int pin,
                                int numInputs,
                                const std::vector<double>& levels,
                                std::vector<double>* current,
                                std::vector<std::vector<double>>* patterns,
                                std::set<std::string>* seen) {
    if (pin == numInputs) {
        appendUniquePattern(patterns, seen, *current);
        return;
    }

    for (double level : levels) {
        (*current)[pin] = level;
        generateCartesianRecursive(
            pin + 1, numInputs, levels, current, patterns, seen);
    }
}

std::size_t cartesianPatternCount(int numInputs, std::size_t levelCount) {
    std::size_t count = 1;
    for (int i = 0; i < numInputs; ++i) {
        if (levelCount != 0 &&
            count >
                std::numeric_limits<std::size_t>::max() / levelCount) {
            return 0;
        }
        count *= levelCount;
    }
    return count;
}

}  // namespace

ActivityPatternSpec::ActivityPatternSpec()
    : mode(ActivityPatternMode::kUniformSweep),
      levels(ActivityPatternGenerator::defaultUniformSweepLevels()) {}

ActivityPatternSpec ActivityPatternSpec::UniformSweep(
    std::vector<double> levels) {
    ActivityPatternSpec spec;
    spec.mode = ActivityPatternMode::kUniformSweep;
    if (!levels.empty()) {
        spec.levels = std::move(levels);
    }
    return spec;
}

ActivityPatternSpec ActivityPatternSpec::CartesianGrid(
    std::vector<double> levels) {
    ActivityPatternSpec spec;
    spec.mode = ActivityPatternMode::kCartesianGrid;
    spec.levels = std::move(levels);
    return spec;
}

ActivityPatternSpec ActivityPatternSpec::ExplicitList(
    std::vector<std::vector<double>> patterns) {
    ActivityPatternSpec spec;
    spec.mode = ActivityPatternMode::kExplicitList;
    spec.explicitPatterns = std::move(patterns);
    return spec;
}

std::vector<double> ActivityPatternGenerator::defaultUniformSweepLevels() {
    std::vector<double> levels;
    levels.reserve(kLegacyUniformLevelCount);
    for (int i = 1; i <= kLegacyUniformLevelCount; ++i) {
        levels.push_back(kLegacyUniformStep * static_cast<double>(i));
    }
    return levels;
}

std::vector<std::vector<double>> ActivityPatternGenerator::generate(
    int numInputs,
    const ActivityPatternSpec& spec) {
    if (numInputs <= 0) {
        return {{}};
    }

    std::vector<std::vector<double>> patterns;
    std::set<std::string> seen;

    switch (spec.mode) {
        case ActivityPatternMode::kUniformSweep: {
            const std::vector<double> levels = normalizeLevels(spec.levels);
            patterns.reserve(levels.size());
            for (double level : levels) {
                appendUniquePattern(
                    &patterns, &seen, std::vector<double>(numInputs, level));
            }
            break;
        }
        case ActivityPatternMode::kCartesianGrid: {
            const std::vector<double> levels = normalizeLevels(spec.levels);
            const std::size_t expected =
                cartesianPatternCount(numInputs, levels.size());
            if (expected > 0) {
                patterns.reserve(expected);
            }

            std::vector<double> current(numInputs, 0.0);
            generateCartesianRecursive(
                0, numInputs, levels, &current, &patterns, &seen);
            break;
        }
        case ActivityPatternMode::kExplicitList: {
            patterns.reserve(spec.explicitPatterns.size());
            for (const auto& pattern : spec.explicitPatterns) {
                if (static_cast<int>(pattern.size()) != numInputs) {
                    throw std::runtime_error(
                        "Explicit activity pattern width does not match "
                        "the configured input count.");
                }
                appendUniquePattern(&patterns, &seen, pattern);
            }
            break;
        }
    }

    return patterns;
}

double ActivityPatternGenerator::stableActivityValue(double value) {
    return canonicalizeActivityValue(value);
}

std::string ActivityPatternGenerator::stableValueString(double value) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(9)
        << canonicalizeActivityValue(value);
    return oss.str();
}

std::string ActivityPatternGenerator::stablePatternKey(
    const std::vector<double>& pattern) {
    std::ostringstream oss;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (i != 0) {
            oss << "|";
        }
        oss << stableValueString(pattern[i]);
    }
    return oss.str();
}

std::vector<std::vector<double>> generateActivityPatterns(
    int numInputs,
    const ActivityPatternSpec& spec) {
    return ActivityPatternGenerator::generate(numInputs, spec);
}

}  // namespace fes
