#include "fes/app/AppConfig.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace fes::app {
namespace {

enum class JsonKind {
    kNull,
    kBool,
    kNumber,
    kString,
    kArray,
    kObject,
};

struct JsonValue {
    JsonKind kind = JsonKind::kNull;
    bool boolValue = false;
    double numberValue = 0.0;
    std::string stringValue;
    std::vector<JsonValue> arrayValue;
    std::map<std::string, JsonValue> objectValue;

    bool isObject() const { return kind == JsonKind::kObject; }
    bool isArray() const { return kind == JsonKind::kArray; }
    bool isString() const { return kind == JsonKind::kString; }
    bool isNumber() const { return kind == JsonKind::kNumber; }
    bool isBool() const { return kind == JsonKind::kBool; }
};

class JsonParser {
public:
    explicit JsonParser(std::string text) : text_(std::move(text)) {}

    JsonValue parse() {
        JsonValue value = parseValue();
        skipWhitespace();
        if (pos_ != text_.size()) {
            throw std::runtime_error("Unexpected trailing JSON content.");
        }
        return value;
    }

private:
    JsonValue parseValue() {
        skipWhitespace();
        if (pos_ >= text_.size()) {
            throw std::runtime_error("Unexpected end of JSON.");
        }

        const char c = text_[pos_];
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') return parseString();
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
            return parseNumber();
        }
        if (match("true")) {
            JsonValue v;
            v.kind = JsonKind::kBool;
            v.boolValue = true;
            return v;
        }
        if (match("false")) {
            JsonValue v;
            v.kind = JsonKind::kBool;
            v.boolValue = false;
            return v;
        }
        if (match("null")) {
            return JsonValue{};
        }
        throw std::runtime_error("Invalid JSON value.");
    }

    JsonValue parseObject() {
        expect('{');
        JsonValue value;
        value.kind = JsonKind::kObject;
        skipWhitespace();
        if (peek('}')) {
            ++pos_;
            return value;
        }

        while (true) {
            JsonValue key = parseString();
            skipWhitespace();
            expect(':');
            value.objectValue[key.stringValue] = parseValue();
            skipWhitespace();
            if (peek('}')) {
                ++pos_;
                break;
            }
            expect(',');
        }
        return value;
    }

    JsonValue parseArray() {
        expect('[');
        JsonValue value;
        value.kind = JsonKind::kArray;
        skipWhitespace();
        if (peek(']')) {
            ++pos_;
            return value;
        }

        while (true) {
            value.arrayValue.push_back(parseValue());
            skipWhitespace();
            if (peek(']')) {
                ++pos_;
                break;
            }
            expect(',');
        }
        return value;
    }

    JsonValue parseString() {
        expect('"');
        JsonValue value;
        value.kind = JsonKind::kString;
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') {
                return value;
            }
            if (c != '\\') {
                value.stringValue += c;
                continue;
            }
            if (pos_ >= text_.size()) {
                throw std::runtime_error("Invalid JSON string escape.");
            }
            const char escaped = text_[pos_++];
            if (escaped == '"' || escaped == '\\' || escaped == '/') {
                value.stringValue += escaped;
            } else if (escaped == 'n') {
                value.stringValue += '\n';
            } else if (escaped == 'r') {
                value.stringValue += '\r';
            } else if (escaped == 't') {
                value.stringValue += '\t';
            } else {
                throw std::runtime_error("Unsupported JSON string escape.");
            }
        }
        throw std::runtime_error("Unterminated JSON string.");
    }

    JsonValue parseNumber() {
        const size_t start = pos_;
        if (peek('-')) ++pos_;
        while (pos_ < text_.size() &&
               std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
        }
        if (peek('.')) {
            ++pos_;
            while (pos_ < text_.size() &&
                   std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
                ++pos_;
            }
        }
        if (peek('e') || peek('E')) {
            ++pos_;
            if (peek('+') || peek('-')) ++pos_;
            while (pos_ < text_.size() &&
                   std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
                ++pos_;
            }
        }

        JsonValue value;
        value.kind = JsonKind::kNumber;
        value.numberValue = std::stod(text_.substr(start, pos_ - start));
        return value;
    }

    bool match(const std::string& token) {
        if (text_.compare(pos_, token.size(), token) == 0) {
            pos_ += token.size();
            return true;
        }
        return false;
    }

    bool peek(char c) const {
        return pos_ < text_.size() && text_[pos_] == c;
    }

    void expect(char c) {
        skipWhitespace();
        if (!peek(c)) {
            throw std::runtime_error(std::string("Expected JSON token: ") + c);
        }
        ++pos_;
    }

    void skipWhitespace() {
        while (pos_ < text_.size() &&
               std::isspace(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
        }
    }

    std::string text_;
    size_t pos_ = 0;
};

std::string toLowerCopy(std::string text) {
    std::transform(
        text.begin(),
        text.end(),
        text.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
    return text;
}

const JsonValue* findMember(const JsonValue& object, const std::string& key) {
    if (!object.isObject()) {
        return nullptr;
    }
    const auto it = object.objectValue.find(key);
    return it == object.objectValue.end() ? nullptr : &it->second;
}

const JsonValue& requireMember(const JsonValue& object,
                               const std::string& key) {
    const JsonValue* value = findMember(object, key);
    if (value == nullptr) {
        throw std::runtime_error("Missing required config key: " + key);
    }
    return *value;
}

std::string getString(const JsonValue& object,
                      const std::string& key,
                      const std::string& defaultValue = "") {
    const JsonValue* value = findMember(object, key);
    if (value == nullptr) {
        return defaultValue;
    }
    if (!value->isString()) {
        throw std::runtime_error("Config key must be a string: " + key);
    }
    return value->stringValue;
}

fs::path getPath(const JsonValue& object, const std::string& key) {
    return fs::path(getString(object, key));
}

int getInt(const JsonValue& object, const std::string& key, int defaultValue) {
    const JsonValue* value = findMember(object, key);
    if (value == nullptr) {
        return defaultValue;
    }
    if (!value->isNumber()) {
        throw std::runtime_error("Config key must be a number: " + key);
    }
    return static_cast<int>(value->numberValue);
}

unsigned getUnsigned(const JsonValue& object,
                     const std::string& key,
                     unsigned defaultValue) {
    const int value = getInt(object, key, static_cast<int>(defaultValue));
    if (value < 0) {
        throw std::runtime_error("Config key must be non-negative: " + key);
    }
    return static_cast<unsigned>(value);
}

bool getBool(const JsonValue& object, const std::string& key, bool defaultValue) {
    const JsonValue* value = findMember(object, key);
    if (value == nullptr) {
        return defaultValue;
    }
    if (!value->isBool()) {
        throw std::runtime_error("Config key must be a bool: " + key);
    }
    return value->boolValue;
}

std::vector<double> parseNumberArray(const JsonValue& value,
                                     const std::string& key) {
    if (!value.isArray()) {
        throw std::runtime_error("Config key must be a number array: " + key);
    }
    std::vector<double> values;
    values.reserve(value.arrayValue.size());
    for (const JsonValue& item : value.arrayValue) {
        if (!item.isNumber()) {
            throw std::runtime_error(
                "Config key must contain only numbers: " + key);
        }
        values.push_back(
            ActivityPatternGenerator::stableActivityValue(item.numberValue));
    }
    return values;
}

std::vector<std::vector<double>> parseExplicitArray(const JsonValue& value,
                                                    const std::string& key) {
    if (!value.isArray()) {
        throw std::runtime_error("Config key must be an array: " + key);
    }
    std::vector<std::vector<double>> patterns;
    patterns.reserve(value.arrayValue.size());
    for (const JsonValue& item : value.arrayValue) {
        patterns.push_back(parseNumberArray(item, key));
    }
    return patterns;
}

void applyRunConfig(const JsonValue& root, GenerateOptions* opts) {
    const JsonValue* run = findMember(root, "run");
    if (run == nullptr) {
        return;
    }
    opts->resumePolicy =
        resumePolicyFromString(getString(*run, "resume_policy", "run_all"));
    opts->caseTimeoutMs =
        getInt(*run, "case_timeout_ms", opts->caseTimeoutMs);
    opts->workerCount = getUnsigned(*run, "threads", opts->workerCount);
}

void applyRunConfig(const JsonValue& root, EvaluationOptions* opts) {
    const JsonValue* run = findMember(root, "run");
    if (run == nullptr) {
        return;
    }
    opts->resumePolicy =
        resumePolicyFromString(getString(*run, "resume_policy", "run_all"));
    opts->caseTimeoutMs =
        getInt(*run, "case_timeout_ms", opts->caseTimeoutMs);
    opts->workerCount = getUnsigned(*run, "threads", opts->workerCount);
}

void applyToolConfig(const JsonValue& root, GenerateOptions* opts) {
    const JsonValue* tools = findMember(root, "tools");
    if (tools == nullptr) {
        return;
    }
    opts->abcPath = getString(*tools, "abc_path", opts->abcPath);
    opts->genlibPath = getPath(*tools, "genlib_path");
    opts->libertyPath = getPath(*tools, "liberty_path");
    opts->pythonScriptPath = getPath(*tools, "python_script");
    opts->standardCellCsvPath = getPath(*tools, "standard_cell_csv");
}

void applyToolConfig(const JsonValue& root, EvaluationOptions* opts) {
    const JsonValue* tools = findMember(root, "tools");
    if (tools == nullptr) {
        return;
    }
    opts->abcPath = getString(*tools, "abc_path", opts->abcPath);
    opts->pythonScriptPath = getPath(*tools, "python_script");
}

void applyActivityConfig(const JsonValue& generate, GenerateOptions* opts) {
    const JsonValue* activity = findMember(generate, "activity");
    if (activity == nullptr) {
        return;
    }

    const std::string mode =
        toLowerCopy(getString(*activity, "mode", "uniform"));
    opts->activityModeName = mode;
    if (mode == "uniform") {
        const JsonValue* levels = findMember(*activity, "levels");
        if (levels != nullptr) {
            opts->activityPatternSpec =
                ActivityPatternSpec::UniformSweep(
                    parseNumberArray(*levels, "generate.activity.levels"));
        }
    } else if (mode == "cartesian") {
        opts->activityPatternSpec =
            ActivityPatternSpec::CartesianGrid(parseNumberArray(
                requireMember(*activity, "levels"),
                "generate.activity.levels"));
    } else if (mode == "explicit") {
        opts->activityPatternSpec =
            ActivityPatternSpec::ExplicitList(parseExplicitArray(
                requireMember(*activity, "explicit"),
                "generate.activity.explicit"));
    } else {
        throw std::runtime_error(
            "generate.activity.mode must be uniform, cartesian, or explicit.");
    }
}

void applyGenerateConfig(const JsonValue& root,
                         int maxLutInputs,
                         GenerateOptions* opts) {
    const JsonValue* generate = findMember(root, "generate");
    if (generate == nullptr) {
        return;
    }

    opts->k = getInt(*generate, "k", opts->k);
    if (opts->k <= 0 || opts->k > maxLutInputs) {
        throw std::runtime_error(
            "generate.k must be in the compiled LUT range.");
    }
    opts->numFunctions =
        getInt(*generate, "num_functions", opts->numFunctions);
    opts->mode = toLowerCopy(
        getString(*generate, "function_source", opts->mode));
    opts->benchmarkDir = getPath(*generate, "benchmark_dir");
    opts->outputDir = getPath(*generate, "output_dir");
    opts->verify = getBool(*generate, "verify", opts->verify);
    opts->satTimeoutMs =
        getInt(*generate, "sat_timeout_ms", opts->satTimeoutMs);
    opts->optTimeoutMs =
        getInt(*generate, "opt_timeout_ms", opts->optTimeoutMs);
    opts->caseTimeoutMs =
        getInt(*generate, "case_timeout_ms", opts->caseTimeoutMs);
    if (opts->numFunctions < 0 || opts->satTimeoutMs < 0 ||
        opts->optTimeoutMs < 0 || opts->caseTimeoutMs < 0) {
        throw std::runtime_error(
            "generate timeout and count settings must be non-negative.");
    }
    applyActivityConfig(*generate, opts);
}

void applyEvaluationConfig(const JsonValue& root, EvaluationOptions* opts) {
    const JsonValue* evaluation = findMember(root, "evaluate");
    if (evaluation == nullptr) {
        evaluation = findMember(root, "benchmark");
    }
    if (evaluation == nullptr) {
        return;
    }

    opts->benchmarkDir = getPath(*evaluation, "benchmark_dir");
    opts->libraryDir = getPath(*evaluation, "library_dir");
    opts->abcLocalLibraryDir = getPath(*evaluation, "abc_local_library_dir");
    opts->verify = getBool(*evaluation, "verify", opts->verify);
    opts->mappedFourWay =
        getString(*evaluation, "mode", "standard") == "mapped_four_way";
    opts->caseTimeoutMs =
        getInt(*evaluation, "case_timeout_ms", opts->caseTimeoutMs);
    if (opts->caseTimeoutMs < 0) {
        throw std::runtime_error(
            "evaluate.case_timeout_ms must be non-negative.");
    }
}

fs::path findConfigPath(const std::vector<std::string>& args) {
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string prefix = "--config=";
        if (args[i] == "--config") {
            if (i + 1 >= args.size()) {
                throw std::runtime_error("Missing value for option --config");
            }
            return args[i + 1];
        }
        if (args[i].rfind(prefix, 0) == 0) {
            return args[i].substr(prefix.size());
        }
    }
    return {};
}

bool consumeOption(const std::vector<std::string>& args,
                   size_t& index,
                   const std::string& name,
                   std::string* valueOut) {
    const std::string prefix = name + "=";
    if (args[index] == name) {
        if (index + 1 >= args.size()) {
            throw std::runtime_error("Missing value for option " + name);
        }
        *valueOut = args[++index];
        return true;
    }
    if (args[index].rfind(prefix, 0) == 0) {
        *valueOut = args[index].substr(prefix.size());
        return true;
    }
    return false;
}

void applyEvaluationOverrides(const std::vector<std::string>& args,
                              EvaluationOptions* opts) {
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        std::string value;

        if (arg == "-h" || arg == "--help") {
            opts->help = true;
            continue;
        }
        if (arg == "--verify") {
            opts->verify = true;
            continue;
        }
        if (arg == "--mapped-four-way") {
            opts->mappedFourWay = true;
            continue;
        }
        if (arg == "--resume") {
            opts->resumePolicy = ResumePolicy::kResume;
            continue;
        }
        if (arg == "--skip-completed") {
            opts->resumePolicy = ResumePolicy::kSkipCompleted;
            continue;
        }
        if (consumeOption(args, i, "--config", &value)) {
            opts->configPath = value;
            continue;
        }
        if (consumeOption(args, i, "--path", &value)) {
            opts->benchmarkDir = value;
            continue;
        }
        if (consumeOption(args, i, "--lib", &value)) {
            opts->libraryDir = value;
            continue;
        }
        if (consumeOption(args, i, "--abc-local-lib", &value)) {
            opts->abcLocalLibraryDir = value;
            continue;
        }
        if (consumeOption(args, i, "--rerun", &value)) {
            opts->resumePolicy = resumePolicyFromString("rerun_" + value);
            continue;
        }
        if (consumeOption(args, i, "--case-timeout-ms", &value)) {
            opts->caseTimeoutMs = std::stoi(value);
            continue;
        }

        throw std::runtime_error("Unknown evaluate option: " + arg);
    }
}

}  // namespace

AppConfig loadAppConfig(const fs::path& configPath, int maxLutInputs) {
    std::ifstream input(configPath);
    if (!input.is_open()) {
        throw std::runtime_error(
            "Could not open config file: " + configPath.string());
    }
    std::string text{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};

    JsonValue root = JsonParser(std::move(text)).parse();
    if (!root.isObject()) {
        throw std::runtime_error("Top-level config must be a JSON object.");
    }

    AppConfig config;
    config.command = toLowerCopy(getString(root, "command", ""));
    config.generate.configPath = configPath;
    config.evaluation.configPath = configPath;

    applyRunConfig(root, &config.generate);
    applyRunConfig(root, &config.evaluation);
    applyToolConfig(root, &config.generate);
    applyToolConfig(root, &config.evaluation);
    applyGenerateConfig(root, maxLutInputs, &config.generate);
    applyEvaluationConfig(root, &config.evaluation);
    return config;
}

EvaluationOptions parseEvaluationOptions(
    const std::vector<std::string>& args,
    int maxLutInputs) {
    const fs::path configPath = findConfigPath(args);
    EvaluationOptions opts;
    if (!configPath.empty()) {
        AppConfig config = loadAppConfig(configPath, maxLutInputs);
        if (!config.command.empty() &&
            config.command != "evaluate" &&
            config.command != "benchmark" &&
            config.command != "optimize") {
            throw std::runtime_error(
                "Config command is not compatible with evaluation: " +
                config.command);
        }
        opts = config.evaluation;
        opts.configPath = configPath;
    }
    applyEvaluationOverrides(args, &opts);
    if (opts.caseTimeoutMs < 0) {
        throw std::runtime_error("Timeout values must be non-negative.");
    }
    return opts;
}

}  // namespace fes::app
