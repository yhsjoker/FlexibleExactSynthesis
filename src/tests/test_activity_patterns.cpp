#include "fes/flow/ActivityPatternGenerator.h"

#include <cmath>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace fes {
namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void requireNear(double lhs, double rhs, const std::string& message) {
    require(std::abs(lhs - rhs) <=
                ActivityPatternGenerator::kComparisonTolerance,
            message);
}

void requirePattern(const std::vector<double>& actual,
                    const std::vector<double>& expected,
                    const std::string& label) {
    require(actual.size() == expected.size(), label + " width mismatch.");
    for (std::size_t i = 0; i < actual.size(); ++i) {
        requireNear(actual[i], expected[i], label + " value mismatch.");
    }
}

void verifyUniformSweepCountAndOrdering() {
    const auto patterns = generateActivityPatterns(
        4, ActivityPatternSpec::UniformSweep());

    require(patterns.size() == 9, "Uniform sweep should produce 9 patterns.");
    requirePattern(patterns.front(), {0.1, 0.1, 0.1, 0.1},
                   "Uniform first");
    requirePattern(patterns[1], {0.2, 0.2, 0.2, 0.2},
                   "Uniform second");
    requirePattern(patterns.back(), {0.9, 0.9, 0.9, 0.9},
                   "Uniform last");
}

void verifyCartesianGridCountAndOrdering() {
    const auto patterns = generateActivityPatterns(
        4,
        ActivityPatternSpec::CartesianGrid({0.2, 0.4, 0.6, 0.8}));

    require(patterns.size() == 256,
            "4-input cartesian grid with four levels should produce 256 patterns.");
    requirePattern(patterns[0], {0.2, 0.2, 0.2, 0.2},
                   "Cartesian first");
    requirePattern(patterns[1], {0.2, 0.2, 0.2, 0.4},
                   "Cartesian second");
    requirePattern(patterns[3], {0.2, 0.2, 0.2, 0.8},
                   "Cartesian fourth");
    requirePattern(patterns[4], {0.2, 0.2, 0.4, 0.2},
                   "Cartesian fifth");
    requirePattern(patterns.back(), {0.8, 0.8, 0.8, 0.8},
                   "Cartesian last");
}

void verifyNoDuplicatePatterns() {
    const auto patterns = generateActivityPatterns(
        2,
        ActivityPatternSpec::CartesianGrid({0.2, 0.2, 0.4}));

    require(patterns.size() == 4,
            "Duplicate levels should not create duplicate cartesian patterns.");

    std::set<std::string> keys;
    for (const auto& pattern : patterns) {
        const std::string key =
            ActivityPatternGenerator::stablePatternKey(pattern);
        require(keys.insert(key).second, "Generated duplicate pattern key.");
    }
}

void verifyConfigurableInputWidth() {
    const auto uniform = generateActivityPatterns(
        5, ActivityPatternSpec::UniformSweep());
    require(uniform.size() == 9, "5-input uniform sweep count mismatch.");
    require(uniform.front().size() == 5,
            "5-input uniform sweep width mismatch.");

    const auto cartesian = generateActivityPatterns(
        3,
        ActivityPatternSpec::CartesianGrid({0.25, 0.75}));
    require(cartesian.size() == 8, "3-input cartesian grid count mismatch.");
    require(cartesian.front().size() == 3,
            "3-input cartesian grid width mismatch.");
    requirePattern(cartesian[1], {0.25, 0.25, 0.75},
                   "3-input cartesian ordering");
}

}  // namespace
}  // namespace fes

int main() {
    try {
        fes::verifyUniformSweepCountAndOrdering();
        fes::verifyCartesianGridCountAndOrdering();
        fes::verifyNoDuplicatePatterns();
        fes::verifyConfigurableInputWidth();
        std::cout << "All activity pattern tests passed." << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "test_activity_patterns failed: " << ex.what()
                  << std::endl;
        return 1;
    }
}
