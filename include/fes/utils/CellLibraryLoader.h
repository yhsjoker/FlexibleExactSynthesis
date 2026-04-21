#pragma once

#include <string>
#include <vector>

namespace fes {

struct StandardCell {
    std::string name;
    double area = 0.0;
    double leakage = 0.0;
    std::string expression;
};

class CellLibraryLoader {
public:
    static std::vector<StandardCell> loadOrGenerate(
        const std::string& csvPath,
        const std::string& libPath,
        int maxInputs);
};

}  // namespace fes
