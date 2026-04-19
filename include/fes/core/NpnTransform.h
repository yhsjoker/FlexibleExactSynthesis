#pragma once

#include <vector>

#include "fes/core/Types.h"

namespace fes {

// NPN recipe semantics:
//   original(realInputs) =
//       outputNegation ? !canonical(canonicalInputs)
//                      :  canonical(canonicalInputs)
//
// where canonicalInputs[i] is driven by realInputs[inputPermutation[i]] and is
// inverted when inputNegations[i] is true. canonicalHex stores only the low
// 2^numInputs rows used by the analyzed LUT.
struct NpnRecipe {
    LutTruthTable canonicalHex = 0;
    std::vector<int> inputPermutation;
    std::vector<bool> inputNegations;
    bool outputNegation = false;

    int totalNegationCount() const;
};

class NpnCanonizer {
public:
    static NpnRecipe computeCanonical(LutTruthTable tt, int numInputs);
};

}  // namespace fes
