#include "fes/core/CircuitGraph.h"
#include <iostream>

namespace fes {

    CircuitGraph::CircuitGraph(int nIn, int nOut) {
        inputs_.reserve(nIn);
        outputs_.reserve(nOut);
    }

    void CircuitGraph::addInput(int id, const std::string& name) {
        std::string actualName = name.empty() ? "pi" + std::to_string(id) : name;
        nodes_[id] = {id, NodeType::INPUT, actualName, "", {}};
        inputs_.push_back(id);
    }

    void CircuitGraph::addGate(int id, const std::string& gateType, const std::vector<int>& fanins) {
        std::string name = "n" + std::to_string(id);
        nodes_[id] = {id, NodeType::GATE, name, gateType, fanins};
    }

    void CircuitGraph::setOutputs(const std::vector<int>& outputIds) {
        outputs_ = outputIds;
    }

    const Node& CircuitGraph::getNode(int id) const {
        return nodes_.at(id);
    }

    const std::vector<int>& CircuitGraph::getInputs() const { return inputs_; }
    const std::vector<int>& CircuitGraph::getOutputs() const { return outputs_; }
    const std::map<int, Node>& CircuitGraph::getAllNodes() const { return nodes_; }

    void CircuitGraph::print() const {
        std::cout << "=== Circuit Graph ===" << std::endl;
        std::cout << "Inputs: ";
        for (int id : inputs_) {
            if (nodes_.count(id)) std::cout << nodes_.at(id).name << " ";
        }
        std::cout << "\n";

        for (const auto& [id, node] : nodes_) {
            if (node.type == NodeType::GATE) {
                std::cout << "  " << node.name << " = " << node.gateType << "(";
                for (size_t i = 0; i < node.fanins.size(); ++i) {
                    if (nodes_.count(node.fanins[i])) {
                        std::cout << nodes_.at(node.fanins[i]).name;
                    } else {
                        std::cout << "?" << node.fanins[i];
                    }
                    if (i < node.fanins.size() - 1) std::cout << ", ";
                }
                std::cout << ")\n";
            }
        }

        std::cout << "Outputs: ";
        for (int id : outputs_) {
             if (nodes_.count(id)) std::cout << nodes_.at(id).name << " ";
        }
        std::cout << "\n=====================" << std::endl;
    }

} // namespace fes