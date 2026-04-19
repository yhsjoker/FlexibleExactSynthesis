#ifndef FES_CORE_CIRCUITGRAPH_H
#define FES_CORE_CIRCUITGRAPH_H

#include <vector>
#include <string>
#include <map>

namespace fes {

    enum class NodeType {
        INPUT,
        GATE,
        CONST0,
        CONST1
    };

    struct Node {
        int id;
        NodeType type;
        std::string name;
        std::string gateType;
        std::vector<int> fanins;
    };

    class CircuitGraph {
    private:
        std::map<int, Node> nodes_; 
        std::vector<int> inputs_;
        std::vector<int> outputs_;

    public:
        CircuitGraph() = default;
        CircuitGraph(int nIn, int nOut);

        // 声明函数，移除实现
        void addInput(int id, const std::string& name = "");
        void addGate(int id, const std::string& gateType, const std::vector<int>& fanins);
        void setOutputs(const std::vector<int>& outputIds);

        const Node& getNode(int id) const;
        const std::vector<int>& getInputs() const;
        const std::vector<int>& getOutputs() const;
        const std::map<int, Node>& getAllNodes() const;

        void print() const; // 这是一个比较重的函数，一定要移到 cpp
    };

} // namespace fes

#endif // FES_CORE_CIRCUITGRAPH_H