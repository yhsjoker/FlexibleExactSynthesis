#include "fes/core/NpnTransform.h"

#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace fes {

namespace {

struct CandidateRecipe {
    LutTruthTable canonicalHex = 0;
    std::vector<int> inputPermutation;
    int inputNegationMask = 0;
    bool outputNegation = false;
};

LutTruthTable truthTableMaskForInputs(int numInputs) {
    const int rows = 1 << numInputs;
    if (rows >= 64) {
        return ~LutTruthTable{0};
    }
    return (LutTruthTable{1} << rows) - 1;
}

int countInputNegations(int inputNegationMask, int numInputs) {
    int count = 0;
    for (int i = 0; i < numInputs; ++i) {
        if (((inputNegationMask >> i) & 1) != 0) {
            ++count;
        }
    }
    return count;
}

bool hasFewerNegations(const CandidateRecipe& lhs,
                       const CandidateRecipe& rhs,
                       int numInputs) {
    const int lhsCount =
        countInputNegations(lhs.inputNegationMask, numInputs) +
        (lhs.outputNegation ? 1 : 0);
    const int rhsCount =
        countInputNegations(rhs.inputNegationMask, numInputs) +
        (rhs.outputNegation ? 1 : 0);
    return lhsCount < rhsCount;
}

bool lexicographicallyLessInputNegations(int lhsMask,
                                         int rhsMask,
                                         int numInputs) {
    for (int i = 0; i < numInputs; ++i) {
        const bool lhs = ((lhsMask >> i) & 1) != 0;
        const bool rhs = ((rhsMask >> i) & 1) != 0;
        if (lhs != rhs) {
            return !lhs && rhs;
        }
    }
    return false;
}

bool isPreferredRecipe(const CandidateRecipe& candidate,
                       const CandidateRecipe& best,
                       int numInputs) {
    if (candidate.canonicalHex != best.canonicalHex) {
        return candidate.canonicalHex < best.canonicalHex;
    }

    const bool candidateFewerNegations =
        hasFewerNegations(candidate, best, numInputs);
    const bool bestFewerNegations =
        hasFewerNegations(best, candidate, numInputs);
    if (candidateFewerNegations != bestFewerNegations) {
        return candidateFewerNegations;
    }

    if (candidate.outputNegation != best.outputNegation) {
        return !candidate.outputNegation && best.outputNegation;
    }

    const bool candidateLessNegations =
        lexicographicallyLessInputNegations(
            candidate.inputNegationMask,
            best.inputNegationMask,
            numInputs);
    const bool bestLessNegations =
        lexicographicallyLessInputNegations(
            best.inputNegationMask,
            candidate.inputNegationMask,
            numInputs);
    if (candidateLessNegations != bestLessNegations) {
        return candidateLessNegations;
    }

    return std::lexicographical_compare(
        candidate.inputPermutation.begin(),
        candidate.inputPermutation.end(),
        best.inputPermutation.begin(),
        best.inputPermutation.end());
}

LutTruthTable applyInputTransform(LutTruthTable tt,
                                  const std::vector<int>& inputPermutation,
                                  int inputNegationMask,
                                  int numInputs) {
    const int rows = 1 << numInputs;
    LutTruthTable transformed = 0;

    for (int canonicalRow = 0; canonicalRow < rows; ++canonicalRow) {
        int realRow = 0;
        for (int canonicalInput = 0; canonicalInput < numInputs; ++canonicalInput) {
            const bool canonicalBit =
                ((canonicalRow >> canonicalInput) & 1) != 0;
            const bool realBit =
                canonicalBit ^
                (((inputNegationMask >> canonicalInput) & 1) != 0);
            realRow |= static_cast<int>(realBit)
                       << inputPermutation[canonicalInput];
        }

        if (((tt >> realRow) & LutTruthTable{1}) != 0) {
            transformed |= LutTruthTable{1} << canonicalRow;
        }
    }

    return transformed;
}

std::vector<bool> makeInputNegations(int inputNegationMask, int numInputs) {
    std::vector<bool> inputNegations(numInputs, false);
    for (int i = 0; i < numInputs; ++i) {
        inputNegations[i] = ((inputNegationMask >> i) & 1) != 0;
    }
    return inputNegations;
}

NpnRecipe toPublicRecipe(const CandidateRecipe& candidate, int numInputs) {
    NpnRecipe recipe;
    recipe.canonicalHex = candidate.canonicalHex;
    recipe.inputPermutation = candidate.inputPermutation;
    recipe.inputNegations =
        makeInputNegations(candidate.inputNegationMask, numInputs);
    recipe.outputNegation = candidate.outputNegation;
    return recipe;
}

}  // namespace

int NpnRecipe::totalNegationCount() const {
    return static_cast<int>(std::count(
               inputNegations.begin(), inputNegations.end(), true)) +
           (outputNegation ? 1 : 0);
}

NpnRecipe NpnCanonizer::computeCanonical(LutTruthTable tt, int numInputs) {
    if (numInputs < 0 || numInputs > kLutMaxInputs) {
        throw std::invalid_argument(
            "NpnCanonizer::computeCanonical numInputs out of range.");
    }

    const LutTruthTable mask = truthTableMaskForInputs(numInputs);
    const int inputNegationVariants = 1 << numInputs;

    tt &= mask;

    CandidateRecipe bestRecipe;
    bool hasBestRecipe = false;

    std::vector<int> inputPermutation(numInputs);
    std::iota(inputPermutation.begin(), inputPermutation.end(), 0);

    // Exact search is cheap here: even at 6 inputs the full NPN orbit is only
    // 6! * 2^6 * 2 = 92,160 transformed truth tables.
    do {
        for (int inputNegationMask = 0;
             inputNegationMask < inputNegationVariants;
             ++inputNegationMask) {
            const LutTruthTable positivePolarity =
                applyInputTransform(
                    tt, inputPermutation, inputNegationMask, numInputs);
            const LutTruthTable negativePolarity =
                (~positivePolarity) & mask;

            CandidateRecipe candidates[2];
            candidates[0].canonicalHex = positivePolarity;
            candidates[0].inputPermutation = inputPermutation;
            candidates[0].inputNegationMask = inputNegationMask;
            candidates[0].outputNegation = false;

            candidates[1].canonicalHex = negativePolarity;
            candidates[1].inputPermutation = inputPermutation;
            candidates[1].inputNegationMask = inputNegationMask;
            candidates[1].outputNegation = true;

            for (const CandidateRecipe& candidate : candidates) {
                if (!hasBestRecipe ||
                    isPreferredRecipe(candidate, bestRecipe, numInputs)) {
                    bestRecipe = candidate;
                    hasBestRecipe = true;
                }
            }
        }
    } while (std::next_permutation(
        inputPermutation.begin(), inputPermutation.end()));

    return toPublicRecipe(bestRecipe, numInputs);
}

}  // namespace fes
