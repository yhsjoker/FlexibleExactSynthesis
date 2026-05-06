#include "fes/utils/SynthesisLibraryLoader.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fes {
namespace {

std::string trimCopy(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }

    const size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

uint64_t truthTableMaskForInputs(int numInputs) {
    if (numInputs < 0 || numInputs > 6) {
        throw std::runtime_error(
            "SynthesisLibraryLoader: numInputs out of supported range.");
    }
    const int rows = 1 << numInputs;
    if (rows >= 64) {
        return ~uint64_t{0};
    }
    return (uint64_t{1} << rows) - 1;
}

struct Token {
    enum class Kind {
        kEnd,
        kIdentifier,
        kConst0,
        kConst1,
        kNot,
        kAnd,
        kOr,
        kXor,
        kLParen,
        kRParen,
    };

    Kind kind = Kind::kEnd;
    std::string text;
};

struct ExprNode {
    enum class Kind {
        kConst,
        kVar,
        kNot,
        kAnd,
        kOr,
        kXor,
    };

    Kind kind = Kind::kConst;
    bool constValue = false;
    std::string varName;
    std::unique_ptr<ExprNode> lhs;
    std::unique_ptr<ExprNode> rhs;
};

class ExpressionParser {
public:
    explicit ExpressionParser(std::string expression)
        : tokens_(tokenize(std::move(expression))) {}

    std::unique_ptr<ExprNode> parse() {
        auto node = parseOr();
        if (peek().kind != Token::Kind::kEnd) {
            throw std::runtime_error(
                "Unexpected token in gate expression: " + peek().text);
        }
        return node;
    }

private:
    static std::vector<Token> tokenize(std::string expression) {
        std::vector<Token> tokens;
        for (std::size_t i = 0; i < expression.size();) {
            const char c = expression[i];
            if (std::isspace(static_cast<unsigned char>(c))) {
                ++i;
                continue;
            }

            if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                std::size_t start = i++;
                while (i < expression.size() &&
                       (std::isalnum(static_cast<unsigned char>(expression[i])) ||
                        expression[i] == '_')) {
                    ++i;
                }
                tokens.push_back(
                    Token{Token::Kind::kIdentifier, expression.substr(start, i - start)});
                continue;
            }

            switch (c) {
                case '0':
                    tokens.push_back(Token{Token::Kind::kConst0, "0"});
                    ++i;
                    break;
                case '1':
                    tokens.push_back(Token{Token::Kind::kConst1, "1"});
                    ++i;
                    break;
                case '!':
                    tokens.push_back(Token{Token::Kind::kNot, "!"});
                    ++i;
                    break;
                case '&':
                    tokens.push_back(Token{Token::Kind::kAnd, "&"});
                    ++i;
                    break;
                case '|':
                    tokens.push_back(Token{Token::Kind::kOr, "|"});
                    ++i;
                    break;
                case '^':
                    tokens.push_back(Token{Token::Kind::kXor, "^"});
                    ++i;
                    break;
                case '(':
                    tokens.push_back(Token{Token::Kind::kLParen, "("});
                    ++i;
                    break;
                case ')':
                    tokens.push_back(Token{Token::Kind::kRParen, ")"});
                    ++i;
                    break;
                default:
                    throw std::runtime_error(
                        std::string("Unsupported character in gate expression: ") + c);
            }
        }

        tokens.push_back(Token{Token::Kind::kEnd, ""});
        return tokens;
    }

    const Token& peek() const { return tokens_[index_]; }

    const Token& consume(Token::Kind kind, const char* message) {
        if (peek().kind != kind) {
            throw std::runtime_error(message);
        }
        return tokens_[index_++];
    }

    std::unique_ptr<ExprNode> parseOr() {
        auto node = parseXor();
        while (peek().kind == Token::Kind::kOr) {
            ++index_;
            auto rhs = parseXor();
            auto parent = std::make_unique<ExprNode>();
            parent->kind = ExprNode::Kind::kOr;
            parent->lhs = std::move(node);
            parent->rhs = std::move(rhs);
            node = std::move(parent);
        }
        return node;
    }

    std::unique_ptr<ExprNode> parseXor() {
        auto node = parseAnd();
        while (peek().kind == Token::Kind::kXor) {
            ++index_;
            auto rhs = parseAnd();
            auto parent = std::make_unique<ExprNode>();
            parent->kind = ExprNode::Kind::kXor;
            parent->lhs = std::move(node);
            parent->rhs = std::move(rhs);
            node = std::move(parent);
        }
        return node;
    }

    std::unique_ptr<ExprNode> parseAnd() {
        auto node = parseUnary();
        while (peek().kind == Token::Kind::kAnd) {
            ++index_;
            auto rhs = parseUnary();
            auto parent = std::make_unique<ExprNode>();
            parent->kind = ExprNode::Kind::kAnd;
            parent->lhs = std::move(node);
            parent->rhs = std::move(rhs);
            node = std::move(parent);
        }
        return node;
    }

    std::unique_ptr<ExprNode> parseUnary() {
        if (peek().kind == Token::Kind::kNot) {
            ++index_;
            auto child = parseUnary();
            auto node = std::make_unique<ExprNode>();
            node->kind = ExprNode::Kind::kNot;
            node->lhs = std::move(child);
            return node;
        }
        return parsePrimary();
    }

    std::unique_ptr<ExprNode> parsePrimary() {
        if (peek().kind == Token::Kind::kIdentifier) {
            const std::string name = consume(
                Token::Kind::kIdentifier,
                "Expected identifier in gate expression.").text;
            auto node = std::make_unique<ExprNode>();
            node->kind = ExprNode::Kind::kVar;
            node->varName = name;
            return node;
        }
        if (peek().kind == Token::Kind::kConst0 ||
            peek().kind == Token::Kind::kConst1) {
            const bool value = peek().kind == Token::Kind::kConst1;
            ++index_;
            auto node = std::make_unique<ExprNode>();
            node->kind = ExprNode::Kind::kConst;
            node->constValue = value;
            return node;
        }
        if (peek().kind == Token::Kind::kLParen) {
            ++index_;
            auto node = parseOr();
            consume(Token::Kind::kRParen,
                    "Expected ')' in gate expression.");
            return node;
        }
        throw std::runtime_error("Malformed gate expression.");
    }

    std::vector<Token> tokens_;
    std::size_t index_ = 0;
};

void collectVariables(const ExprNode& node, std::set<std::string>* names) {
    switch (node.kind) {
        case ExprNode::Kind::kConst:
            return;
        case ExprNode::Kind::kVar:
            names->insert(node.varName);
            return;
        case ExprNode::Kind::kNot:
            collectVariables(*node.lhs, names);
            return;
        case ExprNode::Kind::kAnd:
        case ExprNode::Kind::kOr:
        case ExprNode::Kind::kXor:
            collectVariables(*node.lhs, names);
            collectVariables(*node.rhs, names);
            return;
    }
}

std::tuple<std::string, bool, int> splitSignalName(const std::string& name) {
    std::size_t pos = name.size();
    while (pos > 0 &&
           std::isdigit(static_cast<unsigned char>(name[pos - 1]))) {
        --pos;
    }
    const std::string prefix = name.substr(0, pos);
    if (pos == name.size()) {
        return {prefix, false, 0};
    }
    return {prefix, true, std::stoi(name.substr(pos))};
}

bool signalNameLess(const std::string& lhs, const std::string& rhs) {
    const auto [lhsPrefix, lhsHasNumber, lhsNumber] = splitSignalName(lhs);
    const auto [rhsPrefix, rhsHasNumber, rhsNumber] = splitSignalName(rhs);
    if (lhsPrefix != rhsPrefix) {
        return lhsPrefix < rhsPrefix;
    }
    if (lhsHasNumber != rhsHasNumber) {
        return !lhsHasNumber;
    }
    if (lhsHasNumber && lhsNumber != rhsNumber) {
        return lhsNumber < rhsNumber;
    }
    return lhs < rhs;
}

bool evaluateExpr(const ExprNode& node,
                  const std::unordered_map<std::string, bool>& values) {
    switch (node.kind) {
        case ExprNode::Kind::kConst:
            return node.constValue;
        case ExprNode::Kind::kVar: {
            const auto it = values.find(node.varName);
            if (it == values.end()) {
                throw std::runtime_error(
                    "Missing variable assignment for " + node.varName);
            }
            return it->second;
        }
        case ExprNode::Kind::kNot:
            return !evaluateExpr(*node.lhs, values);
        case ExprNode::Kind::kAnd:
            return evaluateExpr(*node.lhs, values) &&
                   evaluateExpr(*node.rhs, values);
        case ExprNode::Kind::kOr:
            return evaluateExpr(*node.lhs, values) ||
                   evaluateExpr(*node.rhs, values);
        case ExprNode::Kind::kXor:
            return evaluateExpr(*node.lhs, values) !=
                   evaluateExpr(*node.rhs, values);
    }
    throw std::runtime_error("Unknown expression node kind.");
}

GateType deriveGateType(const StandardCell& cell) {
    ExpressionParser parser(cell.expression);
    std::unique_ptr<ExprNode> expr = parser.parse();

    std::set<std::string> uniqueNames;
    collectVariables(*expr, &uniqueNames);
    std::vector<std::string> inputs(uniqueNames.begin(), uniqueNames.end());
    std::sort(inputs.begin(), inputs.end(), signalNameLess);

    if (inputs.size() > 6) {
        throw std::runtime_error(
            "Cell expression exceeds supported input count for synthesis: " +
            cell.name);
    }

    const int numInputs = static_cast<int>(inputs.size());
    const int rows = 1 << numInputs;
    uint64_t truthTable = 0;
    std::unordered_map<std::string, bool> assignment;
    for (int row = 0; row < rows; ++row) {
        assignment.clear();
        for (int i = 0; i < numInputs; ++i) {
            assignment.emplace(inputs[i], ((row >> i) & 1) != 0);
        }
        if (evaluateExpr(*expr, assignment)) {
            truthTable |= uint64_t{1} << row;
        }
    }

    return GateType(
        cell.name,
        numInputs,
        truthTable & truthTableMaskForInputs(numInputs),
        cell.leakage,
        cell.area,
        0.0);
}

}  // namespace

std::vector<GateType> SynthesisLibraryLoader::deriveFromStandardCells(
    const std::vector<StandardCell>& cells) {
    std::vector<GateType> library;
    library.reserve(cells.size());
    for (const auto& cell : cells) {
        if (trimCopy(cell.name).empty() || trimCopy(cell.expression).empty()) {
            continue;
        }
        library.push_back(deriveGateType(cell));
    }

    if (library.empty()) {
        throw std::runtime_error(
            "standard_cells.csv contains no usable synthesis gates.");
    }

    return library;
}

}  // namespace fes
