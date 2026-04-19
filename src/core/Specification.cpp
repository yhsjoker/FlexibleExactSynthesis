#include "fes/core/Specification.h"
#include <stdexcept>

namespace fes {

    Specification::Specification(int in, int out) 
        : numInputs(in), numOutputs(out) {}

    void Specification::setTruthTable(uint64_t tt) {
        truthTable.clear();
        truthTable.push_back(tt);
    }

    void Specification::setInputProbabilities(const std::vector<double>& probs) {
        if ((int)probs.size() != numInputs) {
            throw std::runtime_error("Input probabilities size mismatch (must equal numInputs).");
        }
        inputProbabilities = probs;
    }

} // namespace fes