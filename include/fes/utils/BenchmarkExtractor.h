#ifndef FES_UTILS_BENCHMARK_EXTRACTOR_H
#define FES_UTILS_BENCHMARK_EXTRACTOR_H
#include <string>
#include <vector>
#include <map>
#include <filesystem>
#include <set>

namespace fes {

class BenchmarkExtractor {
public:
    // 构造函数：可以指定 abc 的可执行文件路径
    // 默认为 "abc" (假设已在环境变量 PATH 中)
    explicit BenchmarkExtractor(const std::string& abcPath = "abc");

    // 核心功能：处理指定文件夹下的所有 benchmark 文件 (.blif, .aig)
    // 1. 自动调用 ABC 将其映射为 4-LUT
    // 2. 解析映射后的临时文件
    // 3. 统计 4输入真值表频率
    void processDirectory(const std::string& folderPath);

    // 导出前 N 个最频繁出现的 HexFunc 到文件
    void exportTopHexFuncs(const std::string& outputPath, int topN);

private:
    std::string abcPath_;
    std::map<uint16_t, int> frequency_map_;
    std::set<uint16_t> guaranteed_funcs_;

    // 调用 ABC 将 inputFile 映射为 outputFile
    bool runAbcMapping(const std::filesystem::path& inputFile, const std::filesystem::path& outputFile);

    // 解析已映射的 BLIF 文件 (统计逻辑)
    std::map<uint16_t, int> processMappedFile(const std::filesystem::path& filePath);

    // 辅助计算真值表
    uint16_t computeTruthTable(int numInputs, const std::vector<std::string>& coverLines);
};

} // namespace fes

#endif