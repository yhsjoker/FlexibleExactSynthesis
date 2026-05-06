#pragma once

#include "fes/core/GateType.h"
#include "fes/utils/CellLibraryLoader.h"

#include <vector>

namespace fes {

class SynthesisLibraryLoader {
public:
    static std::vector<GateType> deriveFromStandardCells(
        const std::vector<StandardCell>& cells);
};

}  // namespace fes
