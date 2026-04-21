#pragma once

#include <string>
#include <vector>

namespace fes {

enum class ActivityPatternMode {
    kUniformSweep,
    kCartesianGrid,
    kExplicitList,
};

struct ActivityPatternSpec {
    ActivityPatternSpec();

    static ActivityPatternSpec UniformSweep(std::vector<double> levels = {});
    static ActivityPatternSpec CartesianGrid(std::vector<double> levels);
    static ActivityPatternSpec ExplicitList(
        std::vector<std::vector<double>> patterns);

    ActivityPatternMode mode;
    std::vector<double> levels;
    std::vector<std::vector<double>> explicitPatterns;
};

class ActivityPatternGenerator {
public:
    // Values are quantized to 1e-9 for duplicate detection and stable
    // serialization. This keeps deterministic behavior for decimal activity
    // levels while avoiding direct floating-point equality checks.
    static constexpr double kComparisonTolerance = 1e-9;

    static std::vector<double> defaultUniformSweepLevels();
    static std::vector<std::vector<double>> generate(
        int numInputs,
        const ActivityPatternSpec& spec = ActivityPatternSpec());

    static double stableActivityValue(double value);
    static std::string stableValueString(double value);
    static std::string stablePatternKey(const std::vector<double>& pattern);
};

// Customization entry point for library activity generation. Prefer passing a
// custom ActivityPatternSpec here instead of editing the main synthesis flow.
std::vector<std::vector<double>> generateActivityPatterns(
    int numInputs,
    const ActivityPatternSpec& spec = ActivityPatternSpec());

}  // namespace fes
