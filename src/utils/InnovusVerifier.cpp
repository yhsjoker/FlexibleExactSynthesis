#include "fes/utils/InnovusVerifier.h"
#include <array>
#include <memory>
#include <iostream>
#include <sstream>
#include <fstream>
#include <stdexcept>
#include <cstdio>
#include <cstdlib>
#include <random>

namespace fes {

InnovusVerifier::InnovusVerifier(const std::string& pythonScriptPath) 
    : scriptPath_(pythonScriptPath) {}

PPAResult InnovusVerifier::getPPAResult(const std::string& blifFilePath, 
                                        const std::vector<double>& probs,
                                        const std::vector<double>& acts) {
    PPAResult result;
    
    // 1. 基础校验
    if (probs.size() != acts.size()) {
        std::cerr << "[C++ Error] Probs size (" << probs.size() 
                  << ") != Acts size (" << acts.size() << ")" << std::endl;
        return result; // 返回无效结果
    }

    // 2. 生成临时的 .act 文件
    std::string actFilePath = blifFilePath + ".temp.act";
    
    if (!writeTempActFile(actFilePath, probs, acts)) {
        std::cerr << "[C++ Error] Failed to write temp act file." << std::endl;
        return result;
    }

    // 3. 构造调用命令
    std::string command = "python3 " + scriptPath_ + " " + blifFilePath + " " + actFilePath + " 2>/dev/null";

    try {
        std::string output = execCommand(command);

        // 增加一行：只解析最后一行（防止 Python 某些库如 paramiko 偷偷喷出一行警告）
        std::stringstream ss;
        std::string lastLine;
        std::stringstream rawStream(output);
        while (std::getline(rawStream, lastLine)) {
            if (!lastLine.empty()) {
                ss.clear();
                ss.str(lastLine); // 总是把最后一行送入解析器
            }
        }

        ss >> result.power_total 
           >> result.power_internal 
           >> result.power_switching 
           >> result.power_leakage 
           >> result.area 
           >> result.delay;

        // 6. 有效性判定 (Total Power > 0 即认为成功)
        if (result.power_total > 0.0) {
            result.valid = true;
        } else {
            std::cerr << "[InnovusVerifier Error] Script returned invalid data. Raw output:\n" 
                      << output << std::endl;
        }

    } catch (const std::exception& e) {
        std::cerr << "[C++ Error] Execution failed: " << e.what() << std::endl;
        result.valid = false;
    }

    // 7. 清理临时文件
    std::remove(actFilePath.c_str());

    return result;
}

bool InnovusVerifier::writeTempActFile(const std::string& filepath, 
                                       const std::vector<double>& probs, 
                                       const std::vector<double>& acts) {
    std::ofstream ofs(filepath);
    if (!ofs.is_open()) {
        return false;
    }

    // 写入格式: 静态概率(Duty)  翻转率(Activity)
    for (size_t i = 0; i < probs.size(); ++i) {
        ofs << probs[i] << " " << acts[i] << "\n";
    }
    
    ofs.close();
    return true;
}

std::string InnovusVerifier::execCommand(const std::string& cmd) {
    std::array<char, 256> buffer;
    std::string result;

    #ifdef _WIN32
        #define POPEN _popen
        #define PCLOSE _pclose
    #else
        #define POPEN popen
        #define PCLOSE pclose
    #endif

    std::unique_ptr<FILE, decltype(&PCLOSE)> pipe(POPEN(cmd.c_str(), "r"), PCLOSE);
    
    if (!pipe) {
        throw std::runtime_error("popen() failed!");
    }

    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }

    return result;
}

// =========================================================
// [修改点] 将随机数生成与评估完全剥离
// =========================================================
void InnovusVerifier::generateRandomWorkloads(int numInputs, int numCycles, 
                                              std::vector<double>& probs, 
                                              std::vector<double>& acts) {
    probs.assign(numInputs, 0.0);
    acts.assign(numInputs, 0.0);
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 1);

    for (int i = 0; i < numInputs; ++i) {
        int ones_count = 0;
        int toggle_count = 0;
        
        int last_val = dis(gen);
        ones_count += last_val;

        for (int c = 1; c < numCycles; ++c) {
            int current_val = dis(gen);
            ones_count += current_val;
            
            if (current_val != last_val) {
                toggle_count++;
            }
            last_val = current_val;
        }

        probs[i] = static_cast<double>(ones_count) / numCycles;
        acts[i]  = static_cast<double>(toggle_count) / (numCycles - 1);
    }
}

// [新增] 一次性生成多组固定的测试向量集
std::vector<WorkloadSet> InnovusVerifier::generateWorkloadSets(int numInputs, int numSets, int numCycles) {
    std::vector<WorkloadSet> workloads;
    for (int i = 0; i < numSets; ++i) {
        WorkloadSet wl;
        generateRandomWorkloads(numInputs, numCycles, wl.probs, wl.acts);
        workloads.push_back(wl);
    }
    return workloads;
}

// [修改] 接收预先生成的统一向量集进行平均评估
PPAResult InnovusVerifier::getAveragePPAResult(const std::string& blifFilePath, 
                                               const std::vector<WorkloadSet>& workloads) {
    PPAResult avgResult = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, false};
    int validRuns = 0;

    for (size_t i = 0; i < workloads.size(); ++i) {
        // 使用外部传入的同一批概率和翻转率
        PPAResult res = getPPAResult(blifFilePath, workloads[i].probs, workloads[i].acts);
        
        if (res.valid) {
            avgResult.power_total     += res.power_total;
            avgResult.power_internal  += res.power_internal;
            avgResult.power_switching += res.power_switching;
            avgResult.power_leakage   += res.power_leakage;
            avgResult.area            += res.area;
            avgResult.delay           += res.delay;
            validRuns++;
        } else {
            std::cerr << "[Warning] Run " << (i + 1) << " failed, skipping in average." << std::endl;
        }
    }

    if (validRuns > 0) {
        avgResult.power_total     /= validRuns;
        avgResult.power_internal  /= validRuns;
        avgResult.power_switching /= validRuns;
        avgResult.power_leakage   /= validRuns;
        avgResult.area            /= validRuns;
        avgResult.delay           /= validRuns;
        avgResult.valid           = true;
    }

    return avgResult;
}

} // namespace fes
