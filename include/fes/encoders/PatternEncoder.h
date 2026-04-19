#ifndef FES_ENCODERS_PATTERNENCODER_H
#define FES_ENCODERS_PATTERNENCODER_H

#include "../interfaces/IEncoder.h"
#include <map>
#include <vector>

namespace fes {

    class PatternEncoder : public IEncoder {
    public:
        // Key Definitions (保持不变，移到 public 方便访问)
        struct SelectionKey {
            int gateId, inputIdx, sourceId;
            bool operator<(const SelectionKey& o) const {
                return std::tie(gateId, inputIdx, sourceId) < std::tie(o.gateId, o.inputIdx, o.sourceId);
            }
        };
        struct TypeKey {
            int gateId, typeIdx;
            bool operator<(const TypeKey& o) const {
                return std::tie(gateId, typeIdx) < std::tie(o.gateId, o.typeIdx);
            }
        };

    private:
        int numGates_;
        std::map<SelectionKey, Lit> sel_vars_;
        std::map<TypeKey, Lit> type_vars_;
        
        // 【删除】sim_vars_ (不再需要)

    public:
        PatternEncoder(int initialGates = 1);
        ~PatternEncoder() override = default;

        bool encode(ISolver* solver, 
                    const Specification& spec, 
                    const std::vector<GateType>& library) override;

        CircuitGraph decode(ISolver* solver) override;

        int getNumGates() const override { return numGates_; }
        void setNumGates(int n) override { numGates_ = n; }
        
    private:
        // 内部逻辑
        void createVariables(ISolver* solver, const std::vector<GateType>& lib, int numInputs);
        void encodeTopology(ISolver* solver, int numInputs);
        void encodeTypeConsistency(ISolver* solver, const std::vector<GateType>& lib);
        void encodeFunctionality(ISolver* solver, const Specification& spec, const std::vector<GateType>& lib);
        
        // 【新增】编码概率传播 (替代 encodePower)
        void encodeProbability(ISolver* solver, const Specification& spec, const std::vector<GateType>& lib);
    };

} // namespace fes

#endif // FES_ENCODERS_PATTERNENCODER_H