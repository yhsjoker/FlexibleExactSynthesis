#include "fes/utils/SynthesisLibraryLoader.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void verifyLibraryDerivationFromStandardCells() {
    const std::vector<fes::StandardCell> cells = {
        {"INV_X1", 0.532, 14.35, "!A"},
        {"NAND2_X1", 0.798, 17.39, "!(A1 & A2)"},
        {"XOR2_X1", 1.596, 36.16, "A ^ B"},
    };

    const auto library =
        fes::SynthesisLibraryLoader::deriveFromStandardCells(cells);
    require(library.size() == 3,
            "Derived synthesis library size mismatch.");

    require(library[0].name == "INV_X1" && library[0].numInputs == 1,
            "INV gate derivation failed.");
    require(library[0].truthTable == 0x1,
            "INV truth table mismatch.");

    require(library[1].name == "NAND2_X1" && library[1].numInputs == 2,
            "NAND2 gate derivation failed.");
    require(library[1].truthTable == 0x7,
            "NAND2 truth table mismatch.");
    require(library[1].area == 0.798 && library[1].staticPower == 17.39,
            "NAND2 metadata was not preserved.");

    require(library[2].name == "XOR2_X1" && library[2].numInputs == 2,
            "XOR2 gate derivation failed.");
    require(library[2].truthTable == 0x6,
            "XOR2 truth table mismatch.");
}

void verifyEmptyRowsAreSkipped() {
    const std::vector<fes::StandardCell> cells = {
        {"", 0.0, 0.0, ""},
        {"BUF_X1", 0.4, 1.0, "A"},
    };

    const auto library =
        fes::SynthesisLibraryLoader::deriveFromStandardCells(cells);
    require(library.size() == 1 && library[0].name == "BUF_X1",
            "Empty standard-cell rows should be ignored.");
}

}  // namespace

int main() {
    try {
        verifyLibraryDerivationFromStandardCells();
        verifyEmptyRowsAreSkipped();
        std::cout << "All synthesis library loader tests passed." << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "test_synthesis_library_loader failed: " << ex.what()
                  << std::endl;
        return 1;
    }
}
