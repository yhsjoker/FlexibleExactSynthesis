#ifndef FES_UTILS_BLIFWRITER_H
#define FES_UTILS_BLIFWRITER_H

#include "../core/CircuitGraph.h"
#include "../core/GateType.h"
#include <vector>
#include <string>

namespace fes {
    class BlifWriter {
    public:
        static void write(const std::string& filename, 
                          const std::string& modelName,
                          const CircuitGraph& graph, 
                          const std::vector<GateType>& library);
    };
}
#endif