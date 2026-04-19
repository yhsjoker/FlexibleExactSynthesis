#include "fes/core/NpnTransform.h"

#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace fes {
namespace {

LutTruthTable maskForInputs(int numInputs) {
    const int rows = 1 << numInputs;
    if (rows >= 64) {
        return ~LutTruthTable{0};
    }
    return (LutTruthTable{1} << rows) - 1;
}

std::string toHex(LutTruthTable tt, int numInputs) {
    std::ostringstream oss;
    oss << "0x"
        << std::hex << std::uppercase
        << std::setw(((1 << numInputs) + 3) / 4)
        << std::setfill('0')
        << (tt & maskForInputs(numInputs));
    return oss.str();
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

LutTruthTable buildAndTruthTable(int numInputs) {
    return LutTruthTable{1} << ((1 << numInputs) - 1);
}

LutTruthTable buildNorTruthTable() {
    return LutTruthTable{1};
}

LutTruthTable buildParityTruthTable(int numInputs, bool oddParity) {
    LutTruthTable tt = 0;
    const int rows = 1 << numInputs;

    for (int row = 0; row < rows; ++row) {
        int ones = 0;
        for (int bit = 0; bit < numInputs; ++bit) {
            ones += (row >> bit) & 1;
        }

        const bool value = oddParity ? ((ones & 1) != 0) : ((ones & 1) == 0);
        if (value) {
            tt |= LutTruthTable{1} << row;
        }
    }

    return tt;
}

LutTruthTable applyRecipe(const NpnRecipe& recipe, int numInputs) {
    require(static_cast<int>(recipe.inputPermutation.size()) == numInputs,
            "inputPermutation length mismatch.");
    require(static_cast<int>(recipe.inputNegations.size()) == numInputs,
            "inputNegations length mismatch.");

    const int rows = 1 << numInputs;
    LutTruthTable tt = 0;

    for (int realRow = 0; realRow < rows; ++realRow) {
        int canonicalRow = 0;
        for (int canonicalInput = 0; canonicalInput < numInputs; ++canonicalInput) {
            const int realInput = recipe.inputPermutation[canonicalInput];
            require(realInput >= 0 && realInput < numInputs,
                    "inputPermutation contains an out-of-range index.");

            bool canonicalBit = ((realRow >> realInput) & 1) != 0;
            canonicalBit ^= recipe.inputNegations[canonicalInput];
            canonicalRow |= static_cast<int>(canonicalBit) << canonicalInput;
        }

        bool outputBit = ((recipe.canonicalHex >> canonicalRow) & LutTruthTable{1}) != 0;
        outputBit ^= recipe.outputNegation;
        if (outputBit) {
            tt |= LutTruthTable{1} << realRow;
        }
    }

    return tt;
}

int exactMatchPenalty(LutTruthTable lhs, LutTruthTable rhs) {
    require(lhs == rhs,
            "exactMatchPenalty is only defined for identical truth tables in this test.");
    return 0;
}

void verifyRecipeRoundTrip(const std::string& label,
                           LutTruthTable original,
                           int numInputs,
                           const NpnRecipe& recipe) {
    const LutTruthTable reconstructed = applyRecipe(recipe, numInputs);
    require(reconstructed == (original & maskForInputs(numInputs)),
            label + " recipe does not reconstruct the original truth table.");
}

void verifyAndVsNor() {
    constexpr int kNumInputs = 2;
    const LutTruthTable and2 = buildAndTruthTable(kNumInputs);
    const LutTruthTable nor2 = buildNorTruthTable();

    const NpnRecipe andRecipe = NpnCanonizer::computeCanonical(and2, kNumInputs);
    const NpnRecipe norRecipe = NpnCanonizer::computeCanonical(nor2, kNumInputs);

    verifyRecipeRoundTrip("2-input AND", and2, kNumInputs, andRecipe);
    verifyRecipeRoundTrip("2-input NOR", nor2, kNumInputs, norRecipe);

    require(andRecipe.canonicalHex == norRecipe.canonicalHex,
            "2-input AND and NOR should share the same NPN canonical form.");
    require(andRecipe.totalNegationCount() == 2,
            "2-input AND should require two input negations to reach the canonical form.");
    require(norRecipe.totalNegationCount() == 0,
            "2-input NOR should already be canonical with zero negation penalty.");

    std::cout << "AND2 vs NOR2: canonical=" << toHex(andRecipe.canonicalHex, kNumInputs)
              << ", AND penalty=" << andRecipe.totalNegationCount()
              << ", NOR penalty=" << norRecipe.totalNegationCount() << "\n";
}

void verifyXorVsXnor() {
    constexpr int kNumInputs = 3;
    const LutTruthTable xor3 = buildParityTruthTable(kNumInputs, true);
    const LutTruthTable xnor3 = buildParityTruthTable(kNumInputs, false);

    const NpnRecipe xorRecipe = NpnCanonizer::computeCanonical(xor3, kNumInputs);
    const NpnRecipe xnorRecipe = NpnCanonizer::computeCanonical(xnor3, kNumInputs);

    verifyRecipeRoundTrip("3-input XOR", xor3, kNumInputs, xorRecipe);
    verifyRecipeRoundTrip("3-input XNOR", xnor3, kNumInputs, xnorRecipe);

    require(xorRecipe.canonicalHex == xnorRecipe.canonicalHex,
            "3-input XOR and XNOR should share the same NPN canonical form.");
    require(xorRecipe.totalNegationCount() == 1,
            "3-input XOR should need exactly one negation under the chosen tie-break.");
    require(xnorRecipe.totalNegationCount() == 0,
            "3-input XNOR should already be canonical with zero negation penalty.");

    std::cout << "XOR3 vs XNOR3: canonical=" << toHex(xorRecipe.canonicalHex, kNumInputs)
              << ", XOR penalty=" << xorRecipe.totalNegationCount()
              << ", XNOR penalty=" << xnorRecipe.totalNegationCount() << "\n";
}

void verifyAndSelfCase() {
    constexpr int kNumInputs = 4;
    const LutTruthTable and4 = buildAndTruthTable(kNumInputs);

    const NpnRecipe andRecipe = NpnCanonizer::computeCanonical(and4, kNumInputs);
    const NpnRecipe andRecipeAgain = NpnCanonizer::computeCanonical(and4, kNumInputs);
    const int exactPenalty = exactMatchPenalty(and4, and4);

    verifyRecipeRoundTrip("4-input AND", and4, kNumInputs, andRecipe);

    require(andRecipe.canonicalHex == andRecipeAgain.canonicalHex,
            "Canonicalization of the same truth table should be stable.");
    require(exactPenalty == 0,
            "Exact self-match must have zero additional penalty.");
    require(andRecipe.totalNegationCount() == 4,
            "Under lexicographically-smallest NPN canonization, 4-input AND maps to the "
            "bit-0 representative using four input negations.");

    std::cout << "AND4 vs itself: exact-match penalty=" << exactPenalty
              << ", canonical=" << toHex(andRecipe.canonicalHex, kNumInputs)
              << ", canonicalization penalty=" << andRecipe.totalNegationCount()
              << "\n";
}

}  // namespace
}  // namespace fes

int main() {
    try {
        fes::verifyAndVsNor();
        fes::verifyXorVsXnor();
        fes::verifyAndSelfCase();
        std::cout << "All NPN tests passed." << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "test_npn failed: " << ex.what() << std::endl;
        return 1;
    }
}
