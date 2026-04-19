#ifndef FES_UTILS_INNOVUS_VERIFIER_H
#define FES_UTILS_INNOVUS_VERIFIER_H

#include <string>
#include <vector>

namespace fes {

// [新增] 用于存储单组测试向量提取出的概率和翻转率
struct WorkloadSet {
    std::vector<double> probs;
    std::vector<double> acts;
};

/**
 * @brief 完整的 PPA (Power, Performance, Area) 统计数据结构
 */
struct PPAResult {
    // === Power (mW) ===
    double power_total = -1.0;      // 总功耗
    double power_internal = -1.0;   // 内部功耗
    double power_switching = -1.0;  // 翻转功耗
    double power_leakage = -1.0;    // 漏功耗
    
    // === Area (um^2) ===
    double area = -1.0;             // 总面积
    
    // === Performance / Delay (ns) ===
    double delay = -1.0;            // 关键路径延迟 (Arrival Time)

    // === Status ===
    bool valid = false;             // 数据是否有效

    // 调试打印用
    std::string toString() const {
        return "P_Total: " + std::to_string(power_total) + " mW | " +
               "Area: " + std::to_string(area) + " um^2 | " +
               "Delay: " + std::to_string(delay) + " ns";
    }
};

class InnovusVerifier {
public:
    /**
     * @param pythonScriptPath Python 脚本的路径 (例如 scripts/single_power_run.py)
     */
    InnovusVerifier(const std::string& pythonScriptPath);

    /**
     * @brief 执行完整的 PPA 分析 (单次执行)
     * @param blifFilePath BLIF 文件的本地路径
     * @param probs        输入引脚的静态概率 (Static Probability / Duty), 0.0~1.0
     * @param acts         输入引脚的翻转率 (Switching Activity), 通常 0.0~1.0
     * 注意：probs 和 acts 的顺序必须与 BLIF 文件中 .inputs 的定义顺序一致！
     * @return PPAResult   包含所有指标的结构体
     */
    PPAResult getPPAResult(const std::string& blifFilePath, 
                           const std::vector<double>& probs,
                           const std::vector<double>& acts);

    /**
     * @brief [新增] 批量生成统一的随机测试工作负载向量
     * @param numInputs    输入引脚的数量
     * @param numSets      测试的工作负载组数 (默认 10 组)
     * @param numCycles    每组工作负载模拟的时钟周期数 (默认 1000)
     * @return std::vector<WorkloadSet> 包含生成的多组概率和翻转率
     */
    std::vector<WorkloadSet> generateWorkloadSets(int numInputs, 
                                                  int numSets = 10, 
                                                  int numCycles = 1000);

    /**
     * @brief [修改] 使用统一的预生成工作负载执行多次 PPA 分析并取平均值
     * @param blifFilePath BLIF 文件的本地路径
     * @param workloads    预先生成的统一工作负载向量集
     * @return PPAResult   返回多次评估的平均 PPA 结果
     */
    PPAResult getAveragePPAResult(const std::string& blifFilePath, 
                                  const std::vector<WorkloadSet>& workloads);

private:
    std::string scriptPath_;

    // 执行 Shell 命令并获取输出
    std::string execCommand(const std::string& cmd);
    
    // 生成临时的 .act 文件供 Python 读取
    bool writeTempActFile(const std::string& filepath, 
                          const std::vector<double>& probs, 
                          const std::vector<double>& acts);

    // 内部工具：生成随机工作负载向量并计算对应的静态概率(probs)和翻转率(acts)
    void generateRandomWorkloads(int numInputs, int numCycles, 
                                 std::vector<double>& probs, 
                                 std::vector<double>& acts);
};

} // namespace fes

#endif // FES_UTILS_INNOVUS_VERIFIER_H