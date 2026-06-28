/**
 * @file compiler.cpp
 * @brief Shell脚本编译器实现 - 完整版支持黑水脚本
 * @license MIT License
 */

#include "compiler.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <iomanip>
#include <unordered_set>
#include <cstring>
#include <cstdlib>
#include <cctype>

#ifdef _WIN32
#include <process.h>
#define SHELLVM_GETPID _getpid
#else
#include <unistd.h>
#define SHELLVM_GETPID getpid
#endif

namespace shellvm {

// ============================================================================
// 编译辅助状态（文件作用域）
// ============================================================================

// 函数地址表：函数名 -> 函数体起始地址
static std::unordered_map<std::string, int32_t> g_functionAddresses;
// 已知函数名集合
static std::unordered_set<std::string> g_knownFunctions;

// 循环跳转表（用于break/continue）
static std::vector<std::vector<size_t>> g_breakPatchList;
static std::vector<std::vector<size_t>> g_continuePatchList;
static std::vector<size_t> g_loopStarts;

// ============================================================================
// 辅助函数
// ============================================================================

// 将AST节点转换为字符串表示（用于嵌入CMD参数）
static std::string nodeToRawString(ASTNodePtr node) {
    if (!node) return "";
    switch (node->type) {
        case NodeType::Literal:
            return node->literalValue.asString();
        case NodeType::Variable:
            return "$" + node->value;
        case NodeType::CommandSubstitution:
            return node->value; // 包含 $(...)
        case NodeType::ArithmeticExpansion:
            return node->value; // 包含 $((...))
        case NodeType::ParameterExpansion:
            return node->value; // 包含 ${...}
        default:
            return node->value;
    }
}

// 判断字符是否为单词字符（可用于标识符）
static bool isWordChar(char c) {
    return isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.';
}

static size_t byteOffsetForInstructionIndex(const InstructionList& instructions, size_t index) {
    size_t offset = 0;
    size_t limit = std::min(index, instructions.size());
    for (size_t i = 0; i < limit; ++i) {
        offset += instructions[i].toBytes().size();
    }
    return offset;
}

static size_t currentByteOffset(const InstructionList& instructions) {
    return byteOffsetForInstructionIndex(instructions, instructions.size());
}

static bool containsPipelineOrRedirection(ASTNodePtr node) {
    if (!node) {
        return false;
    }

    if (node->type == NodeType::Pipeline) {
        return true;
    }

    if (node->type == NodeType::Command && !node->redirections.empty()) {
        return true;
    }

    if (containsPipelineOrRedirection(node->left) ||
        containsPipelineOrRedirection(node->right) ||
        containsPipelineOrRedirection(node->body) ||
        containsPipelineOrRedirection(node->elseBody) ||
        containsPipelineOrRedirection(node->condition)) {
        return true;
    }

    for (const auto& child : node->children) {
        if (containsPipelineOrRedirection(child)) {
            return true;
        }
    }

    for (const auto& arg : node->args) {
        if (containsPipelineOrRedirection(arg)) {
            return true;
        }
    }

    for (const auto& branch : node->elifBranches) {
        if (containsPipelineOrRedirection(branch.first) ||
            containsPipelineOrRedirection(branch.second)) {
            return true;
        }
    }

    return false;
}

static bool isCompilerTraceEnabled() {
    const char* value = std::getenv("SHELLVM_TRACE");
    return value && std::string(value) == "1";
}

static const char* tokenTypeName(TokenType type) {
    switch (type) {
        case TokenType::EndOfFile: return "EndOfFile";
        case TokenType::Newline: return "Newline";
        case TokenType::Semicolon: return "Semicolon";
        case TokenType::Integer: return "Integer";
        case TokenType::Float: return "Float";
        case TokenType::String: return "String";
        case TokenType::Identifier: return "Identifier";
        case TokenType::KW_If: return "KW_If";
        case TokenType::KW_Then: return "KW_Then";
        case TokenType::KW_Else: return "KW_Else";
        case TokenType::KW_Elif: return "KW_Elif";
        case TokenType::KW_Fi: return "KW_Fi";
        case TokenType::KW_For: return "KW_For";
        case TokenType::KW_While: return "KW_While";
        case TokenType::KW_Until: return "KW_Until";
        case TokenType::KW_Do: return "KW_Do";
        case TokenType::KW_Done: return "KW_Done";
        case TokenType::KW_In: return "KW_In";
        case TokenType::KW_Function: return "KW_Function";
        case TokenType::KW_Return: return "KW_Return";
        case TokenType::KW_Exit: return "KW_Exit";
        case TokenType::KW_Export: return "KW_Export";
        case TokenType::KW_Local: return "KW_Local";
        case TokenType::KW_True: return "KW_True";
        case TokenType::KW_False: return "KW_False";
        case TokenType::KW_Case: return "KW_Case";
        case TokenType::KW_Esac: return "KW_Esac";
        case TokenType::KW_Break: return "KW_Break";
        case TokenType::KW_Continue: return "KW_Continue";
        case TokenType::KW_Undef: return "KW_Undef";
        case TokenType::KW_Declare: return "KW_Declare";
        case TokenType::KW_Readonly: return "KW_Readonly";
        case TokenType::OP_Assign: return "OP_Assign";
        case TokenType::OP_Equals: return "OP_Equals";
        case TokenType::OP_NotEquals: return "OP_NotEquals";
        case TokenType::OP_Less: return "OP_Less";
        case TokenType::OP_LessEqual: return "OP_LessEqual";
        case TokenType::OP_Greater: return "OP_Greater";
        case TokenType::OP_GreaterEqual: return "OP_GreaterEqual";
        case TokenType::OP_Plus: return "OP_Plus";
        case TokenType::OP_Minus: return "OP_Minus";
        case TokenType::OP_Multiply: return "OP_Multiply";
        case TokenType::OP_Divide: return "OP_Divide";
        case TokenType::OP_Modulo: return "OP_Modulo";
        case TokenType::OP_And: return "OP_And";
        case TokenType::OP_Or: return "OP_Or";
        case TokenType::OP_Not: return "OP_Not";
        case TokenType::OP_BitAnd: return "OP_BitAnd";
        case TokenType::OP_BitOr: return "OP_BitOr";
        case TokenType::OP_BitNot: return "OP_BitNot";
        case TokenType::OP_BitXor: return "OP_BitXor";
        case TokenType::OP_ShiftLeft: return "OP_ShiftLeft";
        case TokenType::OP_ShiftRight: return "OP_ShiftRight";
        case TokenType::OP_Pipe: return "OP_Pipe";
        case TokenType::OP_PipeAnd: return "OP_PipeAnd";
        case TokenType::OP_RedirectOut: return "OP_RedirectOut";
        case TokenType::OP_RedirectAppend: return "OP_RedirectAppend";
        case TokenType::OP_RedirectIn: return "OP_RedirectIn";
        case TokenType::OP_RedirectInAnd: return "OP_RedirectInAnd";
        case TokenType::OP_RedirectErr: return "OP_RedirectErr";
        case TokenType::OP_RedirectErrOut: return "OP_RedirectErrOut";
        case TokenType::OP_RedirectOutErr: return "OP_RedirectOutErr";
        case TokenType::OP_Background: return "OP_Background";
        case TokenType::LParen: return "LParen";
        case TokenType::RParen: return "RParen";
        case TokenType::LBrace: return "LBrace";
        case TokenType::RBrace: return "RBrace";
        case TokenType::LBracket: return "LBracket";
        case TokenType::RBracket: return "RBracket";
        case TokenType::LDollarParen: return "LDollarParen";
        case TokenType::RDollarParen: return "RDollarParen";
        case TokenType::Comment: return "Comment";
        case TokenType::Dollar: return "Dollar";
        case TokenType::Backtick: return "Backtick";
        case TokenType::DollarLBrace: return "DollarLBrace";
        case TokenType::RBraceClose: return "RBraceClose";
        case TokenType::Unknown: return "Unknown";
        default: return "TokenType?";
    }
}

static const char* nodeTypeName(NodeType type) {
    switch (type) {
        case NodeType::Program: return "Program";
        case NodeType::Command: return "Command";
        case NodeType::Pipeline: return "Pipeline";
        case NodeType::Assignment: return "Assignment";
        case NodeType::BinaryOp: return "BinaryOp";
        case NodeType::UnaryOp: return "UnaryOp";
        case NodeType::Literal: return "Literal";
        case NodeType::Variable: return "Variable";
        case NodeType::IfStatement: return "IfStatement";
        case NodeType::WhileStatement: return "WhileStatement";
        case NodeType::ForStatement: return "ForStatement";
        case NodeType::ForInStatement: return "ForInStatement";
        case NodeType::FunctionDef: return "FunctionDef";
        case NodeType::FunctionCall: return "FunctionCall";
        case NodeType::Block: return "Block";
        case NodeType::Subshell: return "Subshell";
        case NodeType::Redirection: return "Redirection";
        case NodeType::ReturnStatement: return "ReturnStatement";
        case NodeType::ExitStatement: return "ExitStatement";
        case NodeType::BreakStatement: return "BreakStatement";
        case NodeType::ContinueStatement: return "ContinueStatement";
        case NodeType::CaseStatement: return "CaseStatement";
        case NodeType::TestExpression: return "TestExpression";
        case NodeType::ArrayVariable: return "ArrayVariable";
        case NodeType::CommandSubstitution: return "CommandSubstitution";
        case NodeType::ArithmeticExpansion: return "ArithmeticExpansion";
        case NodeType::ParameterExpansion: return "ParameterExpansion";
        case NodeType::UntilStatement: return "UntilStatement";
        case NodeType::Background: return "Background";
        default: return "NodeType?";
    }
}

// ============================================================================
// 词法分析器实现
// ============================================================================

Lexer::Lexer(const std::string& source) : source_(source) {}

char Lexer::current() const {
    return pos_ < source_.length() ? source_[pos_] : '\0';
}

char Lexer::peek(size_t offset) const {
    size_t nextPos = pos_ + offset;
    return nextPos < source_.length() ? source_[nextPos] : '\0';
}

char Lexer::advance() {
    if (pos_ >= source_.length()) return '\0';
    char c = source_[pos_++];
    if (c == '\n') {
        line_++;
        column_ = 1;
    } else {
        column_++;
    }
    return c;
}

void Lexer::skipWhitespace() {
    while (pos_ < source_.length()) {
        char c = current();
        if (c == ' ' || c == '\t') {
            advance();
        } else if (c == '\\' && peek() == '\n') {
            // 反斜杠续行
            advance(); // '\'
            advance(); // '\n'
        } else {
            break;
        }
    }
}

void Lexer::skipComment() {
    while (pos_ < source_.length() && current() != '\n') {
        advance();
    }
}

TokenType Lexer::lookupKeyword(const std::string& id) const {
    static const std::unordered_map<std::string, TokenType> keywords = {
        {"if", TokenType::KW_If},
        {"then", TokenType::KW_Then},
        {"else", TokenType::KW_Else},
        {"elif", TokenType::KW_Elif},
        {"fi", TokenType::KW_Fi},
        {"for", TokenType::KW_For},
        {"while", TokenType::KW_While},
        {"until", TokenType::KW_Until},
        {"do", TokenType::KW_Do},
        {"done", TokenType::KW_Done},
        {"in", TokenType::KW_In},
        {"function", TokenType::KW_Function},
        {"return", TokenType::KW_Return},
        {"exit", TokenType::KW_Exit},
        {"export", TokenType::KW_Export},
        {"local", TokenType::KW_Local},
        {"true", TokenType::KW_True},
        {"false", TokenType::KW_False},
        {"case", TokenType::KW_Case},
        {"esac", TokenType::KW_Esac},
        {"break", TokenType::KW_Break},
        {"continue", TokenType::KW_Continue},
        {"unset", TokenType::KW_Undef},
        {"declare", TokenType::KW_Declare},
        {"readonly", TokenType::KW_Readonly}
    };

    auto it = keywords.find(id);
    return it != keywords.end() ? it->second : TokenType::Identifier;
}

Token Lexer::nextToken() {
    if (hasPeek_) {
        hasPeek_ = false;
        return peeked_;
    }

    skipWhitespace();

    if (atEnd()) {
        return Token(TokenType::EndOfFile, "", line_, column_);
    }

    char c = current();

    // 注释
    if (c == '#') {
        // 只有在命令开始位置才视为注释
        // 简化处理：# 后跟空格或行尾视为注释
        skipComment();
        return nextToken();
    }

    // 换行
    if (c == '\n') {
        advance();
        return Token(TokenType::Newline, "\\n", line_ - 1, column_);
    }

    // 分号
    if (c == ';') {
        advance();
        return Token(TokenType::Semicolon, ";", line_, column_);
    }

    // 字符串
    if (c == '"' || c == '\'') {
        return readString(c);
    }

    // 反引号命令替换
    if (c == '`') {
        return readString('`');
    }

    // 数字
    if (isdigit(static_cast<unsigned char>(c))) {
        // 检查是否是重定向 fd> 的情况
        // 例如 2> 或 2>&1
        if (peek() == '>') {
            return readOperator();
        }
        return readNumber();
    }

    // $ 表达式
    if (c == '$') {
        return readDollarExpr();
    }

    // 标识符或关键字
    if (isalpha(static_cast<unsigned char>(c)) || c == '_') {
        return readIdentifier();
    }

    // 路径（以 / 开头，如 /dev/null, /proc/$pid/comm, /data/user/0/...）
    if (c == '/' && pos_ + 1 < source_.length()) {
        char next = peek();
        if (isalnum(static_cast<unsigned char>(next)) || next == '_' || next == '.' || next == '$') {
            return readPath();
        }
    }

    // 以 - 开头的选项（如 -0, --help, -e, -ne）
    if (c == '-' && pos_ + 1 < source_.length()) {
        char next = peek();
        if (isalnum(static_cast<unsigned char>(next)) || next == '_' || next == '-') {
            return readOption();
        }
    }

    // 操作符
    return readOperator();
}

Token Lexer::peekToken() {
    if (!hasPeek_) {
        peeked_ = nextToken();
        hasPeek_ = true;
    }
    return peeked_;
}

Token Lexer::readNumber() {
    std::string value;
    int startLine = line_;
    int startCol = column_;

    // 读取整数部分
    while (pos_ < source_.length() && isdigit(static_cast<unsigned char>(current()))) {
        value += advance();
    }

    // 检查是否有小数部分
    if (current() == '.' && peek() != '\0' && isdigit(static_cast<unsigned char>(peek()))) {
        value += advance(); // 读取 '.'
        while (pos_ < source_.length() && isdigit(static_cast<unsigned char>(current()))) {
            value += advance();
        }
        return Token(TokenType::Float, value, startLine, startCol);
    }

    auto isBarewordSuffix = [](char ch) {
        return isalnum(static_cast<unsigned char>(ch)) ||
               ch == '_' || ch == '.' || ch == '-' || ch == '/' || ch == ':';
    };

    if (isBarewordSuffix(current())) {
        while (pos_ < source_.length() && isBarewordSuffix(current())) {
            value += advance();
        }
        return Token(TokenType::Identifier, value, startLine, startCol);
    }

    return Token(TokenType::Integer, value, startLine, startCol);
}

Token Lexer::readString(char quote) {
    std::string value;
    int startLine = line_;
    int startCol = column_;

    advance(); // 读取开头的引号

    if (quote == '`') {
        // 反引号命令替换：读取到下一个反引号
        while (pos_ < source_.length() && current() != '`') {
            if (current() == '\\' && pos_ + 1 < source_.length()) {
                advance();
                char escaped = advance();
                switch (escaped) {
                    case 'n': value += '\n'; break;
                    case 't': value += '\t'; break;
                    case 'r': value += '\r'; break;
                    case '\\': value += '\\'; break;
                    case '`': value += '`'; break;
                    case '$': value += '$'; break;
                    default: value += '\\'; value += escaped; break;
                }
            } else {
                value += advance();
            }
        }
        if (current() == '`') {
            advance();
        }
        // 反引号命令替换包装为 $(...)
        value = "$(" + value + ")";
        return Token(TokenType::Dollar, value, startLine, startCol);
    }

    auto appendBalancedExpansion = [&](char openCh, char closeCh) {
        // 读取 $() / ${}，并允许内部再次出现引号和嵌套展开。
        value += advance(); // '$'
        value += advance(); // '(' or '{'
        int depth = 1;

        while (pos_ < source_.length() && depth > 0) {
            if (current() == '\\' && pos_ + 1 < source_.length()) {
                value += advance();
                value += advance();
                continue;
            }

            if (current() == '\'' || current() == '"') {
                char nestedQuote = advance();
                value += nestedQuote;
                while (pos_ < source_.length()) {
                    if (current() == '\\' && nestedQuote == '"' && pos_ + 1 < source_.length()) {
                        value += advance();
                        value += advance();
                        continue;
                    }
                    char ch = advance();
                    value += ch;
                    if (ch == nestedQuote) {
                        break;
                    }
                }
                continue;
            }

            if (current() == '$' && peek() == openCh) {
                value += advance();
                value += advance();
                ++depth;
                continue;
            }

            char ch = advance();
            value += ch;
            if (ch == openCh) {
                ++depth;
            } else if (ch == closeCh) {
                --depth;
            }
        }
    };

    while (pos_ < source_.length() && current() != quote) {
        if (quote == '"' && current() == '$' && peek() == '(') {
            appendBalancedExpansion('(', ')');
            continue;
        }
        if (quote == '"' && current() == '$' && peek() == '{') {
            appendBalancedExpansion('{', '}');
            continue;
        }

        if (current() == '\\' && pos_ + 1 < source_.length()) {
            advance(); // 读取反斜杠
            char escaped = advance();
            if (quote == '"') {
                // 双引号内只处理特定转义
                switch (escaped) {
                    case '\\': value += '\\'; break;
                    case '"': value += '"'; break;
                    case '$': value += '$'; break;
                    case '`': value += '`'; break;
                    case 'n': value += "\\n"; break; // 保留给echo -e处理
                    case 't': value += "\\t"; break;
                    case 'r': value += "\\r"; break;
                    case '0': 
                        // 八进制转义 \0NN
                        value += "\\0";
                        if (pos_ < source_.length() && isdigit(static_cast<unsigned char>(current()))) {
                            value += advance();
                            if (pos_ < source_.length() && isdigit(static_cast<unsigned char>(current()))) {
                                value += advance();
                            }
                        }
                        break;
                    default: 
                        value += '\\'; 
                        value += escaped; 
                        break;
                }
            } else {
                // 单引号内不处理转义
                value += '\\';
                value += escaped;
            }
        } else {
            value += advance();
        }
    }

    if (pos_ < source_.length() && current() == quote) {
        advance(); // 读取结尾的引号
    }

    return Token(TokenType::String, value, startLine, startCol);
}

Token Lexer::readIdentifier() {
    std::string value;
    int startLine = line_;
    int startCol = column_;

    // 第一个字符
    value += advance();

    // 后续字符：字母数字下划线
    while (pos_ < source_.length() && 
           (isalnum(static_cast<unsigned char>(current())) || current() == '_')) {
        value += advance();
    }

    TokenType type = lookupKeyword(value);

    // 只对非关键字的标识符进行扩展word读取
    if (type == TokenType::Identifier) {
        // 支持包含 . / - : + 等字符的路径和属性名
        // 例如: ro.product.cpu.abi, /dev/null, /proc/$pid/comm, armv8.2-a, kworker/u8:0
        bool continueReading = true;
        while (continueReading && pos_ < source_.length()) {
            char c = current();
            char next = peek();

            // 点号后跟字母数字（如 ro.product.cpu.abi）
            if (c == '.' && (isalnum(static_cast<unsigned char>(next)) || next == '_')) {
                value += advance(); // '.'
                while (pos_ < source_.length() && 
                       (isalnum(static_cast<unsigned char>(current())) || current() == '_')) {
                    value += advance();
                }
            }
            // 斜杠后跟字母数字（如 /dev/null, /proc/...）
            else if (c == '/' && (isalnum(static_cast<unsigned char>(next)) || next == '_' || next == '$' || next == '.')) {
                value += advance(); // '/'
                while (pos_ < source_.length() && 
                       (isalnum(static_cast<unsigned char>(current())) || current() == '_' || current() == '.')) {
                    value += advance();
                }
            }
            // 减号后跟字母数字（如 armv8.2-a, -0, --help）
            else if (c == '-' && (isalnum(static_cast<unsigned char>(next)) || next == '_' || next == '-')) {
                value += advance(); // '-'
                while (pos_ < source_.length() && 
                       (isalnum(static_cast<unsigned char>(current())) || current() == '_' || current() == '.' || current() == '-')) {
                    value += advance();
                }
            }
            // 冒号后跟字母数字（如 kworker/u8:0）
            else if (c == ':' && (isalnum(static_cast<unsigned char>(next)) || next == '_')) {
                value += advance(); // ':'
                while (pos_ < source_.length() && 
                       (isalnum(static_cast<unsigned char>(current())) || current() == '_')) {
                    value += advance();
                }
            }
            // 加号后跟字母数字
            else if (c == '+' && (isalnum(static_cast<unsigned char>(next)) || next == '_')) {
                value += advance(); // '+'
                while (pos_ < source_.length() && 
                       (isalnum(static_cast<unsigned char>(current())) || current() == '_')) {
                    value += advance();
                }
            }
            else {
                continueReading = false;
            }
        }
    }

    return Token(type, value, startLine, startCol);
}

Token Lexer::readPath() {
    // 读取路径，如 /dev/null, /proc/$pid/comm, /data/user/0/com.tencent.tmgp.dfm
    std::string value;
    int startLine = line_;
    int startCol = column_;

    while (pos_ < source_.length()) {
        char c = current();
        // 路径中的合法字符：字母数字 _ . / - : + ~
        if (isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' ||
            c == '/' || c == '-' || c == ':' || c == '+' || c == '~') {
            value += advance();
        }
        // $ 开头的变量引用，将 $ 附加到值中（运行时展开）
        else if (c == '$') {
            value += advance();
            // 读取变量名
            if (pos_ < source_.length() && 
                (isalnum(static_cast<unsigned char>(current())) || current() == '_')) {
                while (pos_ < source_.length() && 
                       (isalnum(static_cast<unsigned char>(current())) || current() == '_')) {
                    value += advance();
                }
            } else if (current() == '{') {
                // ${...}
                value += advance();
                while (pos_ < source_.length() && current() != '}') {
                    value += advance();
                }
                if (current() == '}') value += advance();
            }
        }
        else {
            break;
        }
    }

    return Token(TokenType::Identifier, value, startLine, startCol);
}

Token Lexer::readOption() {
    // 读取选项，如 -0, --help, -e, -ne, -lt, -eq
    std::string value;
    int startLine = line_;
    int startCol = column_;

    // 读取减号
    value += advance();

    // 读取后续字符
    while (pos_ < source_.length()) {
        char c = current();
        if (isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') {
            value += advance();
        } else {
            break;
        }
    }

    return Token(TokenType::Identifier, value, startLine, startCol);
}

Token Lexer::readDollarExpr() {
    int startLine = line_;
    int startCol = column_;
    std::string value;
    value += advance(); // 读取 '$'

    if (atEnd()) {
        return Token(TokenType::Dollar, value, startLine, startCol);
    }

    char c = current();

    if (c == '(') {
        // 检查是否是 $(( 算术展开
        if (peek() == '(') {
            // $((expr)) 算术展开
            value += advance(); // (
            value += advance(); // (
            int depth = 1;
            while (!atEnd() && depth > 0) {
                if (current() == '(') {
                    depth++;
                    value += advance();
                } else if (current() == ')') {
                    depth--;
                    value += advance(); // 读取 )
                    if (depth == 0) {
                        // 检查是否还有第二个 )
                        if (current() == ')') {
                            value += advance(); // 读取第二个 )
                        }
                        break;
                    }
                } else {
                    value += advance();
                }
            }
        } else {
            // $(cmd) 命令替换
            value += advance(); // (
            int depth = 1;
            while (!atEnd() && depth > 0) {
                char cc = current();
                if (cc == '(') {
                    depth++;
                    value += advance();
                } else if (cc == ')') {
                    depth--;
                    value += advance(); // 读取 )
                    if (depth == 0) break;
                } else if (cc == '"') {
                    // 跳过字符串内容
                    value += advance();
                    while (!atEnd() && current() != '"') {
                        if (current() == '\\' && pos_ + 1 < source_.length()) {
                            value += advance();
                        }
                        if (!atEnd()) value += advance();
                    }
                    if (!atEnd() && current() == '"') value += advance();
                } else if (cc == '\'') {
                    value += advance();
                    while (!atEnd() && current() != '\'') {
                        value += advance();
                    }
                    if (!atEnd()) value += advance();
                } else {
                    value += advance();
                }
            }
        }
        return Token(TokenType::Dollar, value, startLine, startCol);
    }

    if (c == '{') {
        // ${...} 参数展开
        value += advance(); // {
        int depth = 1;
        while (!atEnd() && depth > 0) {
            if (current() == '{') {
                depth++;
                value += advance();
            } else if (current() == '}') {
                depth--;
                value += advance(); // 读取 }
                if (depth == 0) break;
            } else {
                value += advance();
            }
        }
        return Token(TokenType::DollarLBrace, value, startLine, startCol);
    }

    // 特殊变量：$@ $* $# $$ $! $? $0-$9
    if (c == '@' || c == '*' || c == '#' || c == '$' || c == '!' || c == '?') {
        value += advance();
        return Token(TokenType::Identifier, value, startLine, startCol);
    }

    if (isdigit(static_cast<unsigned char>(c))) {
        // 位置参数 $0-$9（也支持 ${10} 形式）
        value += advance();
        return Token(TokenType::Identifier, value, startLine, startCol);
    }

    if (isalpha(static_cast<unsigned char>(c)) || c == '_') {
        // 变量名 $var
        while (!atEnd() && 
               (isalnum(static_cast<unsigned char>(current())) || current() == '_')) {
            value += advance();
        }
        // 支持属性路径 $ro.product.cpu.abi
        while (!atEnd() && current() == '.' && 
               peek() != '\0' && (isalnum(static_cast<unsigned char>(peek())) || peek() == '_')) {
            value += advance();
            while (!atEnd() && 
                   (isalnum(static_cast<unsigned char>(current())) || current() == '_')) {
                value += advance();
            }
        }
        return Token(TokenType::Identifier, value, startLine, startCol);
    }

    // 单独的 $
    return Token(TokenType::Dollar, value, startLine, startCol);
}

Token Lexer::readOperator() {
    int startLine = line_;
    int startCol = column_;
    char c = current();

    // 检查数字+重定向的情况 (2>, 2>&1 等)
    if (isdigit(static_cast<unsigned char>(c)) && peek() == '>') {
        // 读取 fd 数字
        std::string fdStr;
        fdStr += advance(); // 数字
        // 现在指向 >
        c = current();
        
        if (c == '>' && peek() == '>') {
            // 2>> 追加重定向
            advance(); advance();
            // 检查 &1
            if (current() == '&' && peek() == '1') {
                advance(); advance();
                return Token(TokenType::OP_RedirectErrOut, "2>>&1", startLine, startCol);
            }
            return Token(TokenType::OP_RedirectAppend, "2>>", startLine, startCol);
        }
        if (c == '>' && peek() == '&') {
            // 2>&1
            advance(); advance(); // > &
            if (current() == '1') {
                advance();
                return Token(TokenType::OP_RedirectErrOut, "2>&1", startLine, startCol);
            }
            return Token(TokenType::OP_RedirectErr, "2>&", startLine, startCol);
        }
        if (c == '>') {
            advance();
            return Token(TokenType::OP_RedirectErr, "2>", startLine, startCol);
        }
    }

    char next = peek();

    // 三字符操作符
    if (next != '\0') {
        // 检查 &> 
        if (c == '&' && next == '>') {
            advance(); advance();
            if (current() == '>') {
                advance();
                return Token(TokenType::OP_RedirectOutErr, "&>>", startLine, startCol);
            }
            return Token(TokenType::OP_RedirectOutErr, "&>", startLine, startCol);
        }
    }

    // 双字符操作符
    if (next != '\0') {
        std::string twoCharOp = std::string(1, c) + std::string(1, next);

        if (twoCharOp == "==") {
            advance(); advance();
            return Token(TokenType::OP_Equals, "==", startLine, startCol);
        }
        if (twoCharOp == "!=") {
            advance(); advance();
            return Token(TokenType::OP_NotEquals, "!=", startLine, startCol);
        }
        if (twoCharOp == "<=") {
            advance(); advance();
            return Token(TokenType::OP_LessEqual, "<=", startLine, startCol);
        }
        if (twoCharOp == ">=") {
            advance(); advance();
            return Token(TokenType::OP_GreaterEqual, ">=", startLine, startCol);
        }
        if (twoCharOp == "&&") {
            advance(); advance();
            return Token(TokenType::OP_And, "&&", startLine, startCol);
        }
        if (twoCharOp == "||") {
            advance(); advance();
            return Token(TokenType::OP_Or, "||", startLine, startCol);
        }
        if (twoCharOp == ">>") {
            advance(); advance();
            return Token(TokenType::OP_RedirectAppend, ">>", startLine, startCol);
        }
        if (twoCharOp == "<<") {
            advance(); advance();
            return Token(TokenType::OP_RedirectInAnd, "<<", startLine, startCol);
        }
        if (twoCharOp == "|&") {
            advance(); advance();
            return Token(TokenType::OP_PipeAnd, "|&", startLine, startCol);
        }
        if (twoCharOp == ">&") {
            advance(); advance();
            return Token(TokenType::OP_RedirectErrOut, ">&", startLine, startCol);
        }
    }

    // 单字符操作符
    advance();

    switch (c) {
        case '=': return Token(TokenType::OP_Assign, "=", startLine, startCol);
        case '<': return Token(TokenType::OP_RedirectIn, "<", startLine, startCol);
        case '>': return Token(TokenType::OP_RedirectOut, ">", startLine, startCol);
        case '+': return Token(TokenType::OP_Plus, "+", startLine, startCol);
        case '-': return Token(TokenType::OP_Minus, "-", startLine, startCol);
        case '*': return Token(TokenType::OP_Multiply, "*", startLine, startCol);
        case '/': return Token(TokenType::OP_Divide, "/", startLine, startCol);
        case '%': return Token(TokenType::OP_Modulo, "%", startLine, startCol);
        case '!': return Token(TokenType::OP_Not, "!", startLine, startCol);
        case '|': return Token(TokenType::OP_Pipe, "|", startLine, startCol);
        case '&': return Token(TokenType::OP_Background, "&", startLine, startCol);
        case '(': return Token(TokenType::LParen, "(", startLine, startCol);
        case ')': return Token(TokenType::RParen, ")", startLine, startCol);
        case '{': return Token(TokenType::LBrace, "{", startLine, startCol);
        case '}': return Token(TokenType::RBrace, "}", startLine, startCol);
        case '[': return Token(TokenType::LBracket, "[", startLine, startCol);
        case ']': return Token(TokenType::RBracket, "]", startLine, startCol);
        case '$': return Token(TokenType::Dollar, "$", startLine, startCol);
        case '`': return Token(TokenType::Backtick, "`", startLine, startCol);
        default: return Token(TokenType::Unknown, std::string(1, c), startLine, startCol);
    }
}

// ============================================================================
// 解析器实现
// ============================================================================

Parser::Parser(const std::string& source) : lexer_(source) {
    current_ = lexer_.nextToken();
}

Token Parser::advanceToken() {
    Token token = current_;
    current_ = lexer_.nextToken();
    return token;
}

bool Parser::match(TokenType type) {
    if (check(type)) {
        advanceToken();
        return true;
    }
    return false;
}

bool Parser::check(TokenType type) const {
    return current_.type == type;
}

void Parser::expect(TokenType type, const std::string& message) {
    if (!match(type)) {
        error(message + " at line " + std::to_string(current_.line) + 
              " (got token type " + std::string(tokenTypeName(current_.type)) + ")");
    }
}

void Parser::error(const std::string& message) {
    if (!hasError_) {
        hasError_ = true;
        errorMessage_ = message;
    }
}

bool Parser::isCommandTerminator() const {
    switch (current_.type) {
        case TokenType::EndOfFile:
        case TokenType::Newline:
        case TokenType::Semicolon:
        case TokenType::OP_Pipe:
        case TokenType::OP_PipeAnd:
        case TokenType::OP_Background:
        case TokenType::OP_And:
        case TokenType::OP_Or:
        case TokenType::OP_RedirectOut:
        case TokenType::OP_RedirectAppend:
        case TokenType::OP_RedirectIn:
        case TokenType::OP_RedirectInAnd:
        case TokenType::OP_RedirectErr:
        case TokenType::OP_RedirectErrOut:
        case TokenType::OP_RedirectOutErr:
        case TokenType::KW_Do:
        case TokenType::KW_Done:
        case TokenType::KW_Fi:
        case TokenType::KW_Else:
        case TokenType::KW_Elif:
        case TokenType::KW_Then:
        case TokenType::KW_Esac:
        case TokenType::RParen:
        case TokenType::RBrace:
            return true;
        default:
            return false;
    }
}

ASTNodePtr Parser::parse() {
    return parseProgram();
}

ASTNodePtr Parser::parseProgram() {
    auto program = std::make_shared<ASTNode>(NodeType::Program);

    while (!check(TokenType::EndOfFile)) {
        // 跳过空行和分号
        while (match(TokenType::Newline) || match(TokenType::Semicolon)) {}

        if (check(TokenType::EndOfFile)) break;

        // 保存错误状态以便恢复
        bool hadError = hasError_;
        std::string oldError = errorMessage_;

        auto stmt = parseStatement();
        if (stmt) {
            program->children.push_back(stmt);
        }

        // 错误恢复：如果解析出错，跳过当前行继续
        if (hasError_ && !hadError) {
            if (isCompilerTraceEnabled()) {
                Token lookahead = lexer_.peekToken();
                std::cerr << "[shellvm-trace] parse-recover: " << errorMessage_
                          << " current=" << tokenTypeName(current_.type)
                          << " next=" << tokenTypeName(lookahead.type) << "\n";
            }
            // 重置错误状态，跳过到下一行
            hasError_ = false;
            errorMessage_.clear();
            // 跳过当前行的剩余token
            while (!check(TokenType::Newline) &&
                   !check(TokenType::EndOfFile)) {
                advanceToken();
            }
            // 消费换行符
            match(TokenType::Newline);
        }

        while (match(TokenType::Newline) || match(TokenType::Semicolon)) {}
    }

    return program;
}

ASTNodePtr Parser::parseStatement() {
    // 容错：跳过不应单独作为语句起始的孤立右括号。
    if (match(TokenType::RParen)) {
        return nullptr;
    }

    // if语句
    if (match(TokenType::KW_If)) {
        return parseIfStatement();
    }

    // while循环
    if (match(TokenType::KW_While)) {
        return parseWhileStatement();
    }

    // until循环
    if (match(TokenType::KW_Until)) {
        return parseUntilStatement();
    }

    // for循环
    if (match(TokenType::KW_For)) {
        return parseForStatement();
    }

    // case语句
    if (match(TokenType::KW_Case)) {
        return parseCaseStatement();
    }

    // 函数定义 (function关键字)
    if (match(TokenType::KW_Function)) {
        return parseFunctionDefKeyword();
    }

    // break
    if (match(TokenType::KW_Break)) {
        auto node = std::make_shared<ASTNode>(NodeType::BreakStatement, current_.line);
        return node;
    }

    // continue
    if (match(TokenType::KW_Continue)) {
        auto node = std::make_shared<ASTNode>(NodeType::ContinueStatement, current_.line);
        return node;
    }

    // return语句
    if (match(TokenType::KW_Return)) {
        auto node = std::make_shared<ASTNode>(NodeType::ReturnStatement, current_.line);
        if (!isCommandTerminator() && !check(TokenType::Newline) && 
            !check(TokenType::Semicolon) && !check(TokenType::EndOfFile)) {
            node->right = parseExpression();
        }
        return node;
    }

    // exit语句
    if (match(TokenType::KW_Exit)) {
        auto node = std::make_shared<ASTNode>(NodeType::ExitStatement, current_.line);
        if (check(TokenType::Integer)) {
            node->literalValue = Value(static_cast<int64_t>(std::stoll(advanceToken().value)));
        } else if (check(TokenType::Identifier)) {
            auto varNode = std::make_shared<ASTNode>(NodeType::Variable, current_.line);
            varNode->value = advanceToken().value.substr(1);
            node->right = varNode;
        }
        return node;
    }

    // local 声明
    if (match(TokenType::KW_Local)) {
        if (check(TokenType::Identifier)) {
            auto assign = std::make_shared<ASTNode>(NodeType::Assignment, current_.line);
            assign->value = advanceToken().value;
            if (match(TokenType::OP_Assign)) {
                assign->right = parseExpression();
            }
            return assign;
        }
        return nullptr;
    }

    // export 声明
    if (match(TokenType::KW_Export)) {
        if (check(TokenType::Identifier)) {
            auto assign = std::make_shared<ASTNode>(NodeType::Assignment, current_.line);
            assign->value = advanceToken().value;
            assign->literalValue = Value(true); // 标记为export
            if (match(TokenType::OP_Assign)) {
                assign->right = parseExpression();
            }
            return assign;
        }
        return nullptr;
    }

    // readonly/declare
    if (match(TokenType::KW_Readonly) || match(TokenType::KW_Declare)) {
        if (check(TokenType::Identifier)) {
            auto assign = std::make_shared<ASTNode>(NodeType::Assignment, current_.line);
            assign->value = advanceToken().value;
            if (match(TokenType::OP_Assign)) {
                assign->right = parseExpression();
            }
            return assign;
        }
        return nullptr;
    }

    // unset
    if (match(TokenType::KW_Undef)) {
        auto cmd = std::make_shared<ASTNode>(NodeType::Command, current_.line);
        cmd->value = "unset";
        while (!isCommandTerminator() && !check(TokenType::EndOfFile)) {
            if (check(TokenType::Identifier)) {
                auto arg = std::make_shared<ASTNode>(NodeType::Literal, current_.line);
                arg->literalValue = Value(advanceToken().value);
                cmd->children.push_back(arg);
            } else {
                break;
            }
        }
        return cmd;
    }

    // 块语句 { ... }
    if (match(TokenType::LBrace)) {
        return parseBlock();
    }

    // 子shell ( ... )
    if (match(TokenType::LParen)) {
        return parseSubshell();
    }

    // 函数定义 name() { ... }
    if (check(TokenType::Identifier) && lexer_.peekToken().type == TokenType::LParen) {
        return parseFunctionDef();
    }

    // 赋值语句 VAR=value
    if (check(TokenType::Identifier) && lexer_.peekToken().type == TokenType::OP_Assign) {
        return parseAssignment();
    }

    // 命令或管道
    return parsePipeline();
}

ASTNodePtr Parser::parsePipeline() {
    auto left = parseCommand();

    while (check(TokenType::OP_Pipe) || check(TokenType::OP_PipeAnd)) {
        bool pipeAnd = check(TokenType::OP_PipeAnd);
        (void)pipeAnd; // |& 管道语义已通过 OP_PipeAnd 标记，编译阶段统一处理
        advanceToken();
        auto right = parseCommand();
        auto pipeline = std::make_shared<ASTNode>(NodeType::Pipeline, current_.line);
        pipeline->left = left;
        pipeline->right = right;
        left = pipeline;

        // 处理 && 和 ||
        while (check(TokenType::OP_And) || check(TokenType::OP_Or)) {
            TokenType opType = current_.type;
            advanceToken();
            auto right2 = parseCommand();
            auto binOp = std::make_shared<ASTNode>(NodeType::BinaryOp, current_.line);
            binOp->op = opType;
            binOp->left = left;
            binOp->right = right2;
            left = binOp;
        }
    }

    // 处理 && 和 || (命令列表)
    while (check(TokenType::OP_And) || check(TokenType::OP_Or)) {
        TokenType opType = current_.type;
        advanceToken();
        // 跳过换行
        while (match(TokenType::Newline)) {}
        auto right = parsePipeline();
        auto binOp = std::make_shared<ASTNode>(NodeType::BinaryOp, current_.line);
        binOp->op = opType;
        binOp->left = left;
        binOp->right = right;
        left = binOp;
    }

    // 后台执行 &
    if (check(TokenType::OP_Background)) {
        advanceToken();
        auto bg = std::make_shared<ASTNode>(NodeType::Background, current_.line);
        bg->left = left;
        left = bg;
    }

    return left;
}

ASTNodePtr Parser::parseCommand() {
    return parseSimpleCommand();
}

ASTNodePtr Parser::parseSimpleCommand() {
    auto cmd = std::make_shared<ASTNode>(NodeType::Command, current_.line);

    // [ 命令 (test)
    if (check(TokenType::LBracket)) {
        advanceToken(); // 消费 [
        cmd->value = "[";
        bool doubleBracket = match(TokenType::LBracket);

        // 读取参数直到 ]
        while (!check(TokenType::RBracket) && !check(TokenType::EndOfFile) && 
               !check(TokenType::Newline)) {
            auto arg = parseWord();
            if (arg) {
                cmd->children.push_back(arg);
            }
        }
        if (check(TokenType::RBracket)) {
            advanceToken(); // 消费 ]
        }
        if (doubleBracket && check(TokenType::RBracket)) {
            advanceToken(); // 消费 ]]
        }
        return parseRedirection(cmd);
    }

    // 读取命令名
    if (check(TokenType::Identifier) || check(TokenType::String) || 
        check(TokenType::Dollar) || check(TokenType::DollarLBrace)) {
        Token token = advanceToken();
        cmd->value = token.value;
    } else {
        error("Expected command name at line " + std::to_string(current_.line));
        return nullptr;
    }

    // 读取参数
    while (!isCommandTerminator() && !check(TokenType::EndOfFile) && !hasError_) {
        auto arg = parseWord();
        if (arg) {
            cmd->children.push_back(arg);
        } else {
            break;
        }
    }

    return parseRedirection(cmd);
}

ASTNodePtr Parser::parseWord() {
    // 处理各种参数类型
    if (check(TokenType::String)) {
        auto node = std::make_shared<ASTNode>(NodeType::Literal, current_.line);
        node->literalValue = Value(advanceToken().value);
        return node;
    }

    if (check(TokenType::Integer)) {
        auto node = std::make_shared<ASTNode>(NodeType::Literal, current_.line);
        node->literalValue = Value(static_cast<int64_t>(std::stoll(advanceToken().value)));
        return node;
    }

    if (check(TokenType::Float)) {
        auto node = std::make_shared<ASTNode>(NodeType::Literal, current_.line);
        node->literalValue = Value(std::stod(advanceToken().value));
        return node;
    }

    if (check(TokenType::Dollar)) {
        // $() 命令替换或 $(( )) 算术展开
        return parseDollarExpression();
    }

    if (check(TokenType::DollarLBrace)) {
        // ${} 参数展开
        return parseParameterExpansion(current_.value);
    }

    if (check(TokenType::Identifier)) {
        Token token = advanceToken();
        if (!token.value.empty() && token.value[0] == '$') {
            // 变量引用 $var, $@, $1 等
            auto node = std::make_shared<ASTNode>(NodeType::Variable, token.line);
            node->value = token.value.substr(1);
            return node;
        } else {
            // 普通标识符
            auto node = std::make_shared<ASTNode>(NodeType::Literal, token.line);
            node->literalValue = Value(token.value);
            return node;
        }
    }

    // 无法识别的token
    if (!isCommandTerminator() && !check(TokenType::EndOfFile)) {
        Token token = advanceToken();
        auto node = std::make_shared<ASTNode>(NodeType::Literal, token.line);
        node->literalValue = Value(token.value);
        return node;
    }

    return nullptr;
}

ASTNodePtr Parser::parseAssignment() {
    auto assign = std::make_shared<ASTNode>(NodeType::Assignment, current_.line);

    // 变量名
    assign->value = advanceToken().value;

    // 等号
    expect(TokenType::OP_Assign, "Expected '=' after variable name");

    // 值：支持引号字符串、$()命令替换、$变量、或bare word（可能包含. - / : 等）
    if (check(TokenType::String)) {
        // 引号字符串
        auto node = std::make_shared<ASTNode>(NodeType::Literal, current_.line);
        node->literalValue = Value(advanceToken().value);
        assign->right = node;
    } else if (check(TokenType::Dollar) || check(TokenType::DollarLBrace)) {
        // $() 命令替换或 ${} 参数展开或 $变量
        assign->right = parseDollarExpression();
    } else if (check(TokenType::Integer)) {
        auto node = std::make_shared<ASTNode>(NodeType::Literal, current_.line);
        node->literalValue = Value(static_cast<int64_t>(std::stoll(advanceToken().value)));
        assign->right = node;
    } else if (check(TokenType::Float)) {
        auto node = std::make_shared<ASTNode>(NodeType::Literal, current_.line);
        node->literalValue = Value(std::stod(advanceToken().value));
        assign->right = node;
    } else {
        // bare word: 拼接连续的token直到遇到终止符
        // 例如 armv8.2-a, ro.board.platform, /data/user/0/com.tencent.tmgp.dfm
        std::string wordValue;
        int wordLine = current_.line;

        while (!check(TokenType::Newline) &&
               !check(TokenType::Semicolon) &&
               !check(TokenType::EndOfFile) &&
               !check(TokenType::OP_Pipe) &&
               !check(TokenType::OP_PipeAnd) &&
               !check(TokenType::OP_Background) &&
               !check(TokenType::OP_RedirectOut) &&
               !check(TokenType::OP_RedirectAppend) &&
               !check(TokenType::OP_RedirectIn) &&
               !check(TokenType::OP_RedirectInAnd) &&
               !check(TokenType::OP_And) &&
               !check(TokenType::OP_Or)) {

            // 遇到空白停止（词法分析器已跳过空白，但如果遇到其他终止符）
            Token token = advanceToken();

            // 检查是否是变量引用 $VAR
            if (token.type == TokenType::Dollar || token.type == TokenType::DollarLBrace) {
                // 如果已经有bare word内容，先保存
                if (!wordValue.empty()) {
                    // 需要处理混合内容，这里简化：将bare word部分作为字符串
                    // 然后处理变量
                    // 实际上这种混合情况较少，先处理纯bare word
                }
                // 回退并处理变量
                // 由于无法回退，这里简化处理
                wordValue += "$" + token.value;
            } else if (token.type == TokenType::Identifier && !token.value.empty() && token.value[0] == '$') {
                // $开头的标识符（词法分析器可能已合并$）
                wordValue += token.value;
            } else {
                // 普通token，拼接其值
                wordValue += token.value;
            }
        }

        auto node = std::make_shared<ASTNode>(NodeType::Literal, wordLine);
        node->literalValue = Value(wordValue);
        assign->right = node;
    }

    return assign;
}

ASTNodePtr Parser::parseExpression() {
    // 字符串
    if (check(TokenType::String)) {
        auto node = std::make_shared<ASTNode>(NodeType::Literal, current_.line);
        node->literalValue = Value(advanceToken().value);
        return node;
    }

    // 整数
    if (check(TokenType::Integer)) {
        auto node = std::make_shared<ASTNode>(NodeType::Literal, current_.line);
        node->literalValue = Value(static_cast<int64_t>(std::stoll(advanceToken().value)));
        return node;
    }

    // 浮点数
    if (check(TokenType::Float)) {
        auto node = std::make_shared<ASTNode>(NodeType::Literal, current_.line);
        node->literalValue = Value(std::stod(advanceToken().value));
        return node;
    }

    // true/false
    if (match(TokenType::KW_True)) {
        auto node = std::make_shared<ASTNode>(NodeType::Literal, current_.line);
        node->literalValue = Value(true);
        return node;
    }
    if (match(TokenType::KW_False)) {
        auto node = std::make_shared<ASTNode>(NodeType::Literal, current_.line);
        node->literalValue = Value(false);
        return node;
    }

    // $() 命令替换
    if (check(TokenType::Dollar)) {
        return parseDollarExpression();
    }

    // ${} 参数展开
    if (check(TokenType::DollarLBrace)) {
        return parseParameterExpansion(current_.value);
    }

    // 标识符或变量
    if (check(TokenType::Identifier)) {
        Token token = advanceToken();
        if (!token.value.empty() && token.value[0] == '$') {
            auto node = std::make_shared<ASTNode>(NodeType::Variable, token.line);
            node->value = token.value.substr(1);
            return node;
        } else {
            auto node = std::make_shared<ASTNode>(NodeType::Literal, token.line);
            node->literalValue = Value(token.value);
            return node;
        }
    }

    // 命令替换 $()
    if (check(TokenType::Dollar)) {
        return parseDollarExpression();
    }

    // 管道或命令（在 $() 内部）
    if (!isCommandTerminator() && !check(TokenType::EndOfFile)) {
        return parsePipeline();
    }

    return nullptr;
}

ASTNodePtr Parser::parseIfStatement() {
    auto ifStmt = std::make_shared<ASTNode>(NodeType::IfStatement, current_.line);
    int ifLine = ifStmt->line;

    if (isCompilerTraceEnabled()) {
        std::cerr << "[shellvm-trace] if-enter line=" << ifLine << "\n";
    }

    // 条件命令列表（到 then 为止）
    ifStmt->left = parsePipeline();

    // 跳过分号和换行
    while (match(TokenType::Semicolon) || match(TokenType::Newline)) {}

    // then
    expect(TokenType::KW_Then, "Expected 'then' after if condition");

    // 跳过换行
    while (match(TokenType::Newline)) {}

    // then块
    auto thenBlock = std::make_shared<ASTNode>(NodeType::Block);
    while (!check(TokenType::KW_Else) && !check(TokenType::KW_Elif) &&
           !check(TokenType::KW_Fi) && !check(TokenType::EndOfFile) && !hasError_) {
        while (match(TokenType::Semicolon) || match(TokenType::Newline)) {}
        if (check(TokenType::KW_Else) || check(TokenType::KW_Elif) || 
            check(TokenType::KW_Fi) || check(TokenType::EndOfFile)) break;
        if (isCompilerTraceEnabled() && ifLine == 87) {
            Token lookahead = lexer_.peekToken();
            std::cerr << "[shellvm-trace] if-body owner=87 token="
                      << tokenTypeName(current_.type)
                      << " next=" << tokenTypeName(lookahead.type)
                      << " line=" << current_.line << "\n";
        }
        auto stmt = parseStatement();
        if (stmt) {
            thenBlock->children.push_back(stmt);
        }
        while (match(TokenType::Semicolon) || match(TokenType::Newline)) {}
    }
    ifStmt->body = thenBlock;

    // elif分支
    while (check(TokenType::KW_Elif)) {
        if (isCompilerTraceEnabled()) {
            std::cerr << "[shellvm-trace] if-elif line=" << current_.line
                      << " owner=" << ifLine << "\n";
        }
        advanceToken(); // elif
        auto elifCond = parsePipeline();
        while (match(TokenType::Semicolon) || match(TokenType::Newline)) {}
        expect(TokenType::KW_Then, "Expected 'then' after elif condition");
        while (match(TokenType::Newline)) {}

        auto elifBody = std::make_shared<ASTNode>(NodeType::Block);
        while (!check(TokenType::KW_Else) && !check(TokenType::KW_Elif) &&
               !check(TokenType::KW_Fi) && !check(TokenType::EndOfFile) && !hasError_) {
            while (match(TokenType::Semicolon) || match(TokenType::Newline)) {}
            if (check(TokenType::KW_Else) || check(TokenType::KW_Elif) || 
                check(TokenType::KW_Fi) || check(TokenType::EndOfFile)) break;
            auto stmt = parseStatement();
            if (stmt) {
                elifBody->children.push_back(stmt);
            }
            while (match(TokenType::Semicolon) || match(TokenType::Newline)) {}
        }
        ifStmt->elifBranches.push_back({elifCond, elifBody});
    }

    // else块
    if (match(TokenType::KW_Else)) {
        if (isCompilerTraceEnabled()) {
            std::cerr << "[shellvm-trace] if-else line=" << current_.line
                      << " owner=" << ifLine << "\n";
        }
        while (match(TokenType::Newline)) {}
        auto elseBlock = std::make_shared<ASTNode>(NodeType::Block);
        while (!check(TokenType::KW_Fi) && !check(TokenType::EndOfFile) && !hasError_) {
            while (match(TokenType::Semicolon) || match(TokenType::Newline)) {}
            if (check(TokenType::KW_Fi) || check(TokenType::EndOfFile)) break;
            auto stmt = parseStatement();
            if (stmt) {
                elseBlock->children.push_back(stmt);
            }
            while (match(TokenType::Semicolon) || match(TokenType::Newline)) {}
        }
        ifStmt->elseBody = elseBlock;
    }

    // fi
    expect(TokenType::KW_Fi, "Expected 'fi' to close if statement");

    if (isCompilerTraceEnabled() && !hasError_) {
        std::cerr << "[shellvm-trace] if-close line=" << ifLine << "\n";
    }

    return ifStmt;
}

ASTNodePtr Parser::parseWhileStatement() {
    auto whileStmt = std::make_shared<ASTNode>(NodeType::WhileStatement, current_.line);

    // 条件
    whileStmt->left = parsePipeline();

    // 跳过分号和换行
    while (match(TokenType::Semicolon) || match(TokenType::Newline)) {}

    // do
    expect(TokenType::KW_Do, "Expected 'do' after while condition");
    while (match(TokenType::Newline)) {}

    // 循环体
    auto body = std::make_shared<ASTNode>(NodeType::Block);
    while (!check(TokenType::KW_Done) && !check(TokenType::EndOfFile) && !hasError_) {
        while (match(TokenType::Semicolon) || match(TokenType::Newline)) {}
        if (check(TokenType::KW_Done) || check(TokenType::EndOfFile)) break;
        auto stmt = parseStatement();
        if (stmt) {
            body->children.push_back(stmt);
        }
        while (match(TokenType::Semicolon) || match(TokenType::Newline)) {}
    }
    whileStmt->body = body;

    // done
    expect(TokenType::KW_Done, "Expected 'done' to close while loop");

    return whileStmt;
}

ASTNodePtr Parser::parseUntilStatement() {
    auto untilStmt = std::make_shared<ASTNode>(NodeType::UntilStatement, current_.line);

    // 条件
    untilStmt->left = parsePipeline();

    while (match(TokenType::Semicolon) || match(TokenType::Newline)) {}

    // do
    expect(TokenType::KW_Do, "Expected 'do' after until condition");
    while (match(TokenType::Newline)) {}

    // 循环体
    auto body = std::make_shared<ASTNode>(NodeType::Block);
    while (!check(TokenType::KW_Done) && !check(TokenType::EndOfFile) && !hasError_) {
        while (match(TokenType::Semicolon) || match(TokenType::Newline)) {}
        if (check(TokenType::KW_Done) || check(TokenType::EndOfFile)) break;
        auto stmt = parseStatement();
        if (stmt) {
            body->children.push_back(stmt);
        }
        while (match(TokenType::Semicolon) || match(TokenType::Newline)) {}
    }
    untilStmt->body = body;

    // done
    expect(TokenType::KW_Done, "Expected 'done' to close until loop");

    return untilStmt;
}

ASTNodePtr Parser::parseForStatement() {
    auto forStmt = std::make_shared<ASTNode>(NodeType::ForInStatement, current_.line);

    // 变量名
    if (check(TokenType::Identifier)) {
        // 如果以$开头，去掉$
        std::string varName = advanceToken().value;
        if (!varName.empty() && varName[0] == '$') {
            varName = varName.substr(1);
        }
        forStmt->value = varName;
    }

    // 跳过分号和换行
    while (match(TokenType::Semicolon) || match(TokenType::Newline)) {}

    // in
    if (match(TokenType::KW_In)) {
        // 列表元素
        auto list = std::make_shared<ASTNode>(NodeType::Block);
        while (!check(TokenType::KW_Do) && !check(TokenType::Newline) &&
               !check(TokenType::Semicolon) && !check(TokenType::EndOfFile) && !hasError_) {
            auto elem = parseWord();
            if (elem) {
                list->children.push_back(elem);
            }
        }
        forStmt->children.push_back(list);
    }

    // 跳过分号和换行
    while (match(TokenType::Semicolon) || match(TokenType::Newline)) {}

    // do
    expect(TokenType::KW_Do, "Expected 'do' in for loop");
    while (match(TokenType::Newline)) {}

    // 循环体
    auto body = std::make_shared<ASTNode>(NodeType::Block);
    while (!check(TokenType::KW_Done) && !check(TokenType::EndOfFile) && !hasError_) {
        while (match(TokenType::Semicolon) || match(TokenType::Newline)) {}
        if (check(TokenType::KW_Done) || check(TokenType::EndOfFile)) break;
        auto stmt = parseStatement();
        if (stmt) {
            body->children.push_back(stmt);
        }
        while (match(TokenType::Semicolon) || match(TokenType::Newline)) {}
    }
    forStmt->body = body;

    // done
    expect(TokenType::KW_Done, "Expected 'done' to close for loop");

    return forStmt;
}

ASTNodePtr Parser::parseCaseStatement() {
    auto caseStmt = std::make_shared<ASTNode>(NodeType::CaseStatement, current_.line);

    // 条件表达式
    caseStmt->left = parseExpression();

    // 跳过分号和换行
    while (match(TokenType::Semicolon) || match(TokenType::Newline)) {}

    // in
    expect(TokenType::KW_In, "Expected 'in' in case statement");
    while (match(TokenType::Newline)) {}

    // 解析各个分支
    while (!check(TokenType::KW_Esac) && !check(TokenType::EndOfFile) && !hasError_) {
        // 解析模式
        auto pattern = parseWord();
        if (!pattern) {
            break;
        }

        // )
        expect(TokenType::RParen, "Expected ')' after case pattern");

        while (match(TokenType::Newline)) {}

        // 分支体
        auto body = std::make_shared<ASTNode>(NodeType::Block);
        while (!check(TokenType::Semicolon) || true) {
            // 检查 ;; 
            if (check(TokenType::Semicolon) && lexer_.peekToken().type == TokenType::Semicolon) {
                break;
            }
            while (match(TokenType::Newline)) {}
            if (check(TokenType::KW_Esac) || check(TokenType::EndOfFile)) break;
            if (check(TokenType::Semicolon) && lexer_.peekToken().type == TokenType::Semicolon) break;
            
            auto stmt = parseStatement();
            if (stmt) {
                body->children.push_back(stmt);
            }
            while (match(TokenType::Newline)) {}
            if (check(TokenType::Semicolon) && lexer_.peekToken().type == TokenType::Semicolon) break;
        }

        caseStmt->elifBranches.push_back({pattern, body});

        // ;;
        if (check(TokenType::Semicolon) && lexer_.peekToken().type == TokenType::Semicolon) {
            advanceToken(); // first ;
            advanceToken(); // second ;
        }
        while (match(TokenType::Newline)) {}
    }

    // esac
    expect(TokenType::KW_Esac, "Expected 'esac' to close case statement");

    return caseStmt;
}

ASTNodePtr Parser::parseFunctionDef() {
    auto funcDef = std::make_shared<ASTNode>(NodeType::FunctionDef, current_.line);

    // 函数名
    if (check(TokenType::Identifier)) {
        funcDef->value = advanceToken().value;
    }

    // 参数列表 ()
    expect(TokenType::LParen, "Expected '(' after function name");
    expect(TokenType::RParen, "Expected ')' after '('");

    // 跳过换行
    while (match(TokenType::Newline)) {}

    // 函数体 { ... }
    if (match(TokenType::LBrace)) {
        auto body = std::make_shared<ASTNode>(NodeType::Block);
        while (!check(TokenType::RBrace) && !check(TokenType::EndOfFile) && !hasError_) {
            while (match(TokenType::Semicolon) || match(TokenType::Newline)) {}
            if (check(TokenType::RBrace) || check(TokenType::EndOfFile)) break;
            auto stmt = parseStatement();
            if (stmt) {
                body->children.push_back(stmt);
            }
            while (match(TokenType::Semicolon) || match(TokenType::Newline)) {}
        }
        expect(TokenType::RBrace, "Expected '}' to close function body");
        funcDef->body = body;
    } else {
        // 单行函数体
        auto body = std::make_shared<ASTNode>(NodeType::Block);
        auto stmt = parseStatement();
        if (stmt) {
            body->children.push_back(stmt);
        }
        funcDef->body = body;
    }

    return funcDef;
}

ASTNodePtr Parser::parseFunctionDefKeyword() {
    auto funcDef = std::make_shared<ASTNode>(NodeType::FunctionDef, current_.line);

    // 函数名
    if (check(TokenType::Identifier)) {
        funcDef->value = advanceToken().value;
    }

    // 可选的 ()
    match(TokenType::LParen);
    match(TokenType::RParen);

    // 跳过换行
    while (match(TokenType::Newline)) {}

    // 函数体 { ... }
    if (match(TokenType::LBrace)) {
        auto body = std::make_shared<ASTNode>(NodeType::Block);
        while (!check(TokenType::RBrace) && !check(TokenType::EndOfFile) && !hasError_) {
            while (match(TokenType::Semicolon) || match(TokenType::Newline)) {}
            if (check(TokenType::RBrace) || check(TokenType::EndOfFile)) break;
            auto stmt = parseStatement();
            if (stmt) {
                body->children.push_back(stmt);
            }
            while (match(TokenType::Semicolon) || match(TokenType::Newline)) {}
        }
        expect(TokenType::RBrace, "Expected '}' to close function body");
        funcDef->body = body;
    } else {
        auto body = std::make_shared<ASTNode>(NodeType::Block);
        auto stmt = parseStatement();
        if (stmt) {
            body->children.push_back(stmt);
        }
        funcDef->body = body;
    }

    return funcDef;
}

ASTNodePtr Parser::parseBlock() {
    auto block = std::make_shared<ASTNode>(NodeType::Block, current_.line);

    while (!check(TokenType::RBrace) && !check(TokenType::EndOfFile) && !hasError_) {
        while (match(TokenType::Semicolon) || match(TokenType::Newline)) {}
        if (check(TokenType::RBrace) || check(TokenType::EndOfFile)) break;
        auto stmt = parseStatement();
        if (stmt) {
            block->children.push_back(stmt);
        }
        while (match(TokenType::Semicolon) || match(TokenType::Newline)) {}
    }

    if (check(TokenType::RBrace)) {
        advanceToken(); // 消费 }
    }

    return block;
}

ASTNodePtr Parser::parseSubshell() {
    auto subshell = std::make_shared<ASTNode>(NodeType::Subshell, current_.line);

    // 子shell内容
    auto body = std::make_shared<ASTNode>(NodeType::Block);
    while (!check(TokenType::RParen) && !check(TokenType::EndOfFile) && !hasError_) {
        while (match(TokenType::Semicolon) || match(TokenType::Newline)) {}
        if (check(TokenType::RParen) || check(TokenType::EndOfFile)) break;
        auto stmt = parseStatement();
        if (stmt) {
            body->children.push_back(stmt);
        }
        while (match(TokenType::Semicolon) || match(TokenType::Newline)) {}
    }

    expect(TokenType::RParen, "Expected ')' to close subshell");
    subshell->body = body;

    // 检查后台执行
    if (check(TokenType::OP_Background)) {
        advanceToken();
        auto bg = std::make_shared<ASTNode>(NodeType::Background, current_.line);
        bg->left = subshell;
        return bg;
    }

    return subshell;
}

ASTNodePtr Parser::parseTestExpression() {
    // [ ... ] 表达式
    auto testNode = std::make_shared<ASTNode>(NodeType::TestExpression, current_.line);
    
    if (match(TokenType::LBracket)) {
        auto cmd = std::make_shared<ASTNode>(NodeType::Command, current_.line);
        cmd->value = "[";
        
        while (!check(TokenType::RBracket) && !check(TokenType::EndOfFile) && 
               !check(TokenType::Newline) && !hasError_) {
            auto arg = parseWord();
            if (arg) {
                cmd->children.push_back(arg);
            }
        }
        
        if (match(TokenType::RBracket)) {
            // 成功
        }
        testNode->left = cmd;
    }
    
    return testNode;
}

ASTNodePtr Parser::parseRedirection(ASTNodePtr cmd) {
    while (true) {
        if (check(TokenType::OP_RedirectOut)) {
            advanceToken();
            if (check(TokenType::Identifier) || check(TokenType::String) ||
                check(TokenType::Integer)) {
                cmd->redirections.push_back({1, advanceToken().value});
            }
        } else if (check(TokenType::OP_RedirectAppend)) {
            advanceToken();
            if (check(TokenType::Identifier) || check(TokenType::String) ||
                check(TokenType::Integer)) {
                cmd->redirections.push_back({2, advanceToken().value});
            }
        } else if (check(TokenType::OP_RedirectIn)) {
            advanceToken();
            if (check(TokenType::Identifier) || check(TokenType::String) ||
                check(TokenType::Integer)) {
                cmd->redirections.push_back({3, advanceToken().value});
            }
        } else if (check(TokenType::OP_RedirectErr)) {
            // 2> file
            advanceToken();
            if (check(TokenType::Identifier) || check(TokenType::String) ||
                check(TokenType::Integer)) {
                cmd->redirections.push_back({4, advanceToken().value});
            }
        } else if (check(TokenType::OP_RedirectErrOut)) {
            // 2>&1
            advanceToken();
            cmd->redirections.push_back({5, "&1"});
        } else if (check(TokenType::OP_RedirectOutErr)) {
            // &> file
            advanceToken();
            if (check(TokenType::Identifier) || check(TokenType::String) ||
                check(TokenType::Integer)) {
                cmd->redirections.push_back({6, advanceToken().value});
            }
        } else if (check(TokenType::OP_RedirectInAnd)) {
            // << (here-doc, 简化处理)
            advanceToken();
            if (check(TokenType::Identifier) || check(TokenType::String)) {
                advanceToken(); // 跳过分隔符
            }
        } else {
            break;
        }
    }
    return cmd;
}

ASTNodePtr Parser::parseDollarExpression() {
    Token token = advanceToken(); // Dollar token
    const std::string& value = token.value;

    if (value.size() >= 3 && value.substr(0, 3) == "$((") {
        // $((...)) 算术展开
        auto node = std::make_shared<ASTNode>(NodeType::ArithmeticExpansion, token.line);
        node->value = value;
        return node;
    } else if (value.size() >= 2 && value.substr(0, 2) == "$(") {
        // $(...) 命令替换
        auto node = std::make_shared<ASTNode>(NodeType::CommandSubstitution, token.line);
        node->value = value;
        return node;
    }

    // 默认作为变量
    auto node = std::make_shared<ASTNode>(NodeType::Variable, token.line);
    node->value = value;
    return node;
}

ASTNodePtr Parser::parseArithmeticExpansion() {
    Token token = advanceToken();
    auto node = std::make_shared<ASTNode>(NodeType::ArithmeticExpansion, token.line);
    node->value = token.value;
    return node;
}

ASTNodePtr Parser::parseParameterExpansion(const std::string& varName) {
    (void)varName; // ${...} 内部内容已在词法阶段捕获，无需前置变量名
    Token token = advanceToken(); // DollarLBrace token
    auto node = std::make_shared<ASTNode>(NodeType::ParameterExpansion, token.line);
    node->value = token.value; // 包含 ${...}
    return node;
}

// ============================================================================
// 编译器实现
// ============================================================================

Bytecode Compiler::compile(const std::string& source) {
    // 清空编译状态
    g_functionAddresses.clear();
    g_knownFunctions.clear();
    g_breakPatchList.clear();
    g_continuePatchList.clear();
    g_loopStarts.clear();

    Parser parser(source);
    auto ast = parser.parse();

    // 解析错误时不再抛出异常，继续编译已成功解析的部分
    // 这样可以支持包含复杂语法的Shell脚本（跳过无法解析的行）
    if (parser.hasError() && isCompilerTraceEnabled()) {
        std::cerr << "[shellvm-trace] parse-warning: " << parser.errorMessage() << "\n";
    }

    return compileAST(ast);
}

Bytecode Compiler::compileAST(ASTNodePtr ast) {
    InstructionList instructions;
    compileNode(ast, instructions);

    Bytecode bytecode;
    for (const auto& inst : instructions) {
        auto bytes = inst.toBytes();
        bytecode.insert(bytecode.end(), bytes.begin(), bytes.end());
    }

    return bytecode;
}

std::string Compiler::bytecodeToCppArray(const Bytecode& bytecode, const std::string& symbolName) {
    std::ostringstream oss;
    oss << "#include <cstddef>\n";
    oss << "#include <cstdint>\n\n";
    oss << "extern const std::uint8_t " << symbolName << "[] = {\n";

    for (size_t i = 0; i < bytecode.size(); ++i) {
        if (i % 12 == 0) {
            oss << "    ";
        }
        oss << "0x"
            << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(bytecode[i]);
        if (i + 1 != bytecode.size()) {
            oss << ", ";
        }
        if (i % 12 == 11 || i + 1 == bytecode.size()) {
            oss << "\n";
        }
    }

    oss << "};\n";
    oss << "extern const std::size_t " << symbolName << "Size = sizeof(" << symbolName << ");\n";
    return oss.str();
}

void Compiler::compileNode(ASTNodePtr node, InstructionList& instructions) {
    if (!node) return;

    switch (node->type) {
        case NodeType::Program:
            compileProgram(node, instructions);
            break;

        case NodeType::Command:
            compileCommand(node, instructions);
            break;

        case NodeType::CommandSubstitution:
            compileCommandSubstitution(node, instructions);
            break;

        case NodeType::Pipeline:
            compilePipeline(node, instructions);
            break;

        case NodeType::Assignment:
            compileAssignment(node, instructions);
            break;

        case NodeType::BinaryOp:
            compileBinaryOp(node, instructions);
            break;

        case NodeType::IfStatement:
            compileIfStatement(node, instructions);
            break;

        case NodeType::WhileStatement:
            compileWhileStatement(node, instructions);
            break;

        case NodeType::UntilStatement:
            compileUntilStatement(node, instructions);
            break;

        case NodeType::ForStatement:
        case NodeType::ForInStatement:
            compileForStatement(node, instructions);
            break;

        case NodeType::CaseStatement:
            compileCaseStatement(node, instructions);
            break;

        case NodeType::FunctionDef:
            compileFunctionDef(node, instructions);
            break;

        case NodeType::FunctionCall:
            compileFunctionCall(node, instructions);
            break;

        case NodeType::Block:
            compileBlock(node, instructions);
            break;

        case NodeType::Subshell:
            compileSubshell(node, instructions);
            break;

        case NodeType::Background:
            compileBackground(node, instructions);
            break;

        case NodeType::ReturnStatement:
            compileReturn(node, instructions);
            break;

        case NodeType::ExitStatement:
            compileExit(node, instructions);
            break;

        case NodeType::BreakStatement:
            compileBreak(node, instructions);
            break;

        case NodeType::ContinueStatement:
            compileContinue(node, instructions);
            break;

        case NodeType::ArithmeticExpansion:
            compileArithmeticExpansion(node, instructions);
            break;

        case NodeType::ParameterExpansion:
            compileParameterExpansion(node, instructions);
            break;

        case NodeType::TestExpression:
            compileTestExpression(node, instructions);
            break;

        case NodeType::Variable:
            compileVariable(node, instructions);
            break;

        case NodeType::Literal:
            compileLiteral(node, instructions);
            break;

        default:
            break;
    }
}

void Compiler::compileProgram(ASTNodePtr node, InstructionList& instructions) {
    if (isCompilerTraceEnabled()) {
        std::cerr << "[shellvm-trace] program-children=" << node->children.size() << "\n";
        for (size_t i = 0; i < node->children.size(); ++i) {
            auto& child = node->children[i];
            std::cerr << "[shellvm-trace] child[" << i << "] "
                      << nodeTypeName(child->type)
                      << " line=" << child->line << "\n";
        }
    }
    for (auto& child : node->children) {
        compileNode(child, instructions);
    }
    instructions.push_back(Instruction(OpCode::HALT));
}

void Compiler::compileCommand(ASTNodePtr node, InstructionList& instructions) {
    const std::string& cmdName = node->value;

    // 检查是否为已定义函数
    if (g_knownFunctions.count(cmdName) > 0) {
        // 编译为函数调用
        auto funcCall = std::make_shared<ASTNode>(NodeType::FunctionCall, node->line);
        funcCall->value = cmdName;
        funcCall->children = node->children;
        compileFunctionCall(funcCall, instructions);
        return;
    }

    // 构建CMD指令：命令名 + 参数数量 + 各参数字符串
    // 参数作为内联字符串嵌入
    Instruction cmdInst(OpCode::CMD);
    cmdInst.addString(cmdName);
    cmdInst.addByte(static_cast<uint8_t>(node->children.size()));

    for (auto& arg : node->children) {
        std::string argStr = nodeToRawString(arg);
        cmdInst.addString(argStr);
    }

    instructions.push_back(cmdInst);

    // 处理重定向
    for (const auto& redir : node->redirections) {
        int redirType = redir.first;
        const std::string& target = redir.second;

        switch (redirType) {
            case 1: // > file
                instructions.push_back(Instruction(OpCode::PUSH_STR).addString(target));
                instructions.push_back(Instruction(OpCode::REDIRECT_OUT));
                break;
            case 2: // >> file
                instructions.push_back(Instruction(OpCode::PUSH_STR).addString(target));
                instructions.push_back(Instruction(OpCode::REDIRECT_APPEND));
                break;
            case 3: // < file
                instructions.push_back(Instruction(OpCode::PUSH_STR).addString(target));
                instructions.push_back(Instruction(OpCode::REDIRECT_IN));
                break;
            case 4: // 2> file
                instructions.push_back(Instruction(OpCode::PUSH_STR).addString(target));
                instructions.push_back(Instruction(OpCode::REDIRECT_OUT));
                break;
            case 6: // &> file
                instructions.push_back(Instruction(OpCode::PUSH_STR).addString(target));
                instructions.push_back(Instruction(OpCode::REDIRECT_OUT));
                break;
            default:
                break;
        }
    }
}

void Compiler::compileCommandSubstitution(ASTNodePtr node, InstructionList& instructions) {
    // node->value 包含 $(...)
    const std::string& fullExpr = node->value;

    if (fullExpr.size() < 3) return;

    // 提取内部命令文本
    std::string innerCmd;
    if (fullExpr.substr(0, 2) == "$(") {
        innerCmd = fullExpr.substr(2, fullExpr.length() - 3);
    } else {
        innerCmd = fullExpr;
    }

    // 重新编译内部命令
    try {
        Parser innerParser(innerCmd);
        auto innerAst = innerParser.parse();
        if (!innerParser.hasError() && innerAst) {
            if (containsPipelineOrRedirection(innerAst)) {
                Instruction cmdInst(OpCode::CMD);
                cmdInst.addString("sh");
                cmdInst.addByte(2);
                cmdInst.addString("-c");
                cmdInst.addString(innerCmd);
                instructions.push_back(cmdInst);
            } else {
            // 编译内部命令（不加HALT）
                for (auto& child : innerAst->children) {
                    compileNode(child, instructions);
                }
            }
        }
    } catch (...) {
        // 解析失败，生成空命令
    }

    // 获取命令输出并压入栈
    // CMD已将退出码压栈，先弹出，再获取输出
    instructions.push_back(Instruction(OpCode::POP)); // 弹出退出码
    instructions.push_back(Instruction(OpCode::GET_OUTPUT)); // 压入stdout输出
}

void Compiler::compilePipeline(ASTNodePtr node, InstructionList& instructions) {
    // 编译左侧命令
    compileNode(node->left, instructions);
    // 编译右侧命令
    compileNode(node->right, instructions);
    // 执行管道操作
    instructions.push_back(Instruction(OpCode::PIPE));
}

void Compiler::compileAssignment(ASTNodePtr node, InstructionList& instructions) {
    // 编译右值
    if (node->right) {
        compileNode(node->right, instructions);
    } else {
        instructions.push_back(Instruction(OpCode::PUSH_NULL));
    }

    // 检查是否为export变量
    if (node->literalValue.isBoolean() && node->literalValue.asBoolean()) {
        // export VAR=value: 设置环境变量
        instructions.push_back(Instruction(OpCode::ENV_SET).addString(node->value));
    }

    // 设置变量
    instructions.push_back(Instruction(OpCode::VAR_SET).addString(node->value));
}

void Compiler::compileBinaryOp(ASTNodePtr node, InstructionList& instructions) {
    if (node->op == TokenType::OP_And) {
        // && : 短路逻辑与
        compileNode(node->left, instructions);
        // 将退出码转换为布尔值 (0=true)
        instructions.push_back(Instruction(OpCode::PUSH_INT).addInt64(0));
        instructions.push_back(Instruction(OpCode::EQ));
        
        size_t skipJump = instructions.size();
        instructions.push_back(Instruction(OpCode::JUMP_IF_NOT).addInt32(0));
        
        // 如果左侧为真，编译右侧
        compileNode(node->right, instructions);
        instructions.push_back(Instruction(OpCode::PUSH_INT).addInt64(0));
        instructions.push_back(Instruction(OpCode::EQ));
        
        patchJump(instructions, skipJump, instructions.size());
        return;
    }

    if (node->op == TokenType::OP_Or) {
        // || : 短路逻辑或
        compileNode(node->left, instructions);
        instructions.push_back(Instruction(OpCode::PUSH_INT).addInt64(0));
        instructions.push_back(Instruction(OpCode::EQ));
        
        size_t skipJump = instructions.size();
        instructions.push_back(Instruction(OpCode::JUMP_IF).addInt32(0));
        
        // 如果左侧为假，编译右侧
        compileNode(node->right, instructions);
        instructions.push_back(Instruction(OpCode::PUSH_INT).addInt64(0));
        instructions.push_back(Instruction(OpCode::EQ));
        
        patchJump(instructions, skipJump, instructions.size());
        return;
    }

    // 普通二元运算
    compileNode(node->left, instructions);
    compileNode(node->right, instructions);

    switch (node->op) {
        case TokenType::OP_Equals:
            instructions.push_back(Instruction(OpCode::EQ));
            break;
        case TokenType::OP_NotEquals:
            instructions.push_back(Instruction(OpCode::NE));
            break;
        case TokenType::OP_Less:
            instructions.push_back(Instruction(OpCode::LT));
            break;
        case TokenType::OP_LessEqual:
            instructions.push_back(Instruction(OpCode::LE));
            break;
        case TokenType::OP_Greater:
            instructions.push_back(Instruction(OpCode::GT));
            break;
        case TokenType::OP_GreaterEqual:
            instructions.push_back(Instruction(OpCode::GE));
            break;
        case TokenType::OP_Plus:
            instructions.push_back(Instruction(OpCode::ADD));
            break;
        case TokenType::OP_Minus:
            instructions.push_back(Instruction(OpCode::SUB));
            break;
        case TokenType::OP_Multiply:
            instructions.push_back(Instruction(OpCode::MUL));
            break;
        case TokenType::OP_Divide:
            instructions.push_back(Instruction(OpCode::DIV));
            break;
        case TokenType::OP_Modulo:
            instructions.push_back(Instruction(OpCode::MOD));
            break;
        default:
            break;
    }
}

void Compiler::compileIfStatement(ASTNodePtr node, InstructionList& instructions) {
    // 编译条件
    compileNode(node->left, instructions);

    // 将命令退出码转换为布尔值 (exit code 0 = true)
    instructions.push_back(Instruction(OpCode::PUSH_INT).addInt64(0));
    instructions.push_back(Instruction(OpCode::EQ));

    // 条件跳转（如果为false跳到elif/else）
    size_t elseJumpOffset = instructions.size();
    instructions.push_back(Instruction(OpCode::JUMP_IF_NOT).addInt32(0));

    // then块
    if (node->body) {
        compileNode(node->body, instructions);
    }

    // 跳过else块
    std::vector<size_t> endJumps;
    size_t endJumpOffset = instructions.size();
    instructions.push_back(Instruction(OpCode::JUMP).addInt32(0));
    endJumps.push_back(endJumpOffset);

    // 补丁else跳转
    patchJump(instructions, elseJumpOffset, instructions.size());

    // elif分支
    for (const auto& branch : node->elifBranches) {
        // 编译elif条件
        compileNode(branch.first, instructions);
        instructions.push_back(Instruction(OpCode::PUSH_INT).addInt64(0));
        instructions.push_back(Instruction(OpCode::EQ));

        size_t nextJumpOffset = instructions.size();
        instructions.push_back(Instruction(OpCode::JUMP_IF_NOT).addInt32(0));

        // elif body
        if (branch.second) {
            compileNode(branch.second, instructions);
        }

        size_t endJump = instructions.size();
        instructions.push_back(Instruction(OpCode::JUMP).addInt32(0));
        endJumps.push_back(endJump);

        patchJump(instructions, nextJumpOffset, instructions.size());
    }

    // else块（如果有）
    if (node->elseBody) {
        compileNode(node->elseBody, instructions);
    }

    // 补丁所有end跳转
    for (size_t jump : endJumps) {
        patchJump(instructions, jump, instructions.size());
    }
}

void Compiler::compileWhileStatement(ASTNodePtr node, InstructionList& instructions) {
    size_t loopStart = instructions.size();

    // 压入循环上下文
    g_breakPatchList.push_back(std::vector<size_t>());
    g_continuePatchList.push_back(std::vector<size_t>());
    g_loopStarts.push_back(loopStart);

    // 编译条件
    compileNode(node->left, instructions);
    instructions.push_back(Instruction(OpCode::PUSH_INT).addInt64(0));
    instructions.push_back(Instruction(OpCode::EQ));

    // 如果为false跳出循环
    size_t exitJumpOffset = instructions.size();
    instructions.push_back(Instruction(OpCode::JUMP_IF_NOT).addInt32(0));

    // 循环体
    if (node->body) {
        compileNode(node->body, instructions);
    }

    // 跳回开始
    instructions.push_back(Instruction(OpCode::JUMP).addInt32(
        static_cast<int32_t>(byteOffsetForInstructionIndex(instructions, loopStart))));

    // 补丁exit跳转
    size_t exitOffset = instructions.size();
    patchJump(instructions, exitJumpOffset, exitOffset);

    // 补丁break跳转
    for (size_t jump : g_breakPatchList.back()) {
        patchJump(instructions, jump, exitOffset);
    }

    // 补丁continue跳转
    for (size_t jump : g_continuePatchList.back()) {
        patchJump(instructions, jump, loopStart);
    }

    g_breakPatchList.pop_back();
    g_continuePatchList.pop_back();
    g_loopStarts.pop_back();
}

void Compiler::compileUntilStatement(ASTNodePtr node, InstructionList& instructions) {
    size_t loopStart = instructions.size();

    g_breakPatchList.push_back(std::vector<size_t>());
    g_continuePatchList.push_back(std::vector<size_t>());
    g_loopStarts.push_back(loopStart);

    // 编译条件
    compileNode(node->left, instructions);
    // until: 当条件为真(退出码0)时退出
    instructions.push_back(Instruction(OpCode::PUSH_INT).addInt64(0));
    instructions.push_back(Instruction(OpCode::EQ));

    // 如果为true跳出循环
    size_t exitJumpOffset = instructions.size();
    instructions.push_back(Instruction(OpCode::JUMP_IF).addInt32(0));

    // 循环体
    if (node->body) {
        compileNode(node->body, instructions);
    }

    // 跳回开始
    instructions.push_back(Instruction(OpCode::JUMP).addInt32(
        static_cast<int32_t>(byteOffsetForInstructionIndex(instructions, loopStart))));

    // 补丁exit跳转
    size_t exitOffset = instructions.size();
    patchJump(instructions, exitJumpOffset, exitOffset);

    // 补丁break跳转
    for (size_t jump : g_breakPatchList.back()) {
        patchJump(instructions, jump, exitOffset);
    }

    // 补丁continue跳转
    for (size_t jump : g_continuePatchList.back()) {
        patchJump(instructions, jump, loopStart);
    }

    g_breakPatchList.pop_back();
    g_continuePatchList.pop_back();
    g_loopStarts.pop_back();
}

void Compiler::compileForStatement(ASTNodePtr node, InstructionList& instructions) {
    // for var in list; do body; done
    // 使用数组存储列表元素，然后循环遍历

    // 获取列表
    ASTNodePtr listNode = nullptr;
    if (!node->children.empty()) {
        listNode = node->children[0];
    }

    // 创建数组并填充元素
    instructions.push_back(Instruction(OpCode::ARR_NEW));

    if (listNode) {
        for (auto& elem : listNode->children) {
            instructions.push_back(Instruction(OpCode::DUP)); // 复制数组引用
            compileNode(elem, instructions);
            instructions.push_back(Instruction(OpCode::ARR_PUSH));
        }
    }

    // 存储数组到临时变量
    instructions.push_back(Instruction(OpCode::VAR_SET).addString("__for_list"));

    // 初始化索引
    instructions.push_back(Instruction(OpCode::PUSH_INT).addInt64(0));
    instructions.push_back(Instruction(OpCode::VAR_SET).addString("__for_index"));

    size_t loopStart = instructions.size();

    g_breakPatchList.push_back(std::vector<size_t>());
    g_continuePatchList.push_back(std::vector<size_t>());
    g_loopStarts.push_back(loopStart);

    // 检查索引 < 数组长度
    instructions.push_back(Instruction(OpCode::VAR_GET).addString("__for_list"));
    instructions.push_back(Instruction(OpCode::ARR_LEN));
    instructions.push_back(Instruction(OpCode::VAR_GET).addString("__for_index"));
    instructions.push_back(Instruction(OpCode::GT)); // list_len > index

    size_t exitJumpOffset = instructions.size();
    instructions.push_back(Instruction(OpCode::JUMP_IF_NOT).addInt32(0));

    // 获取当前元素并赋给循环变量
    instructions.push_back(Instruction(OpCode::VAR_GET).addString("__for_list"));
    instructions.push_back(Instruction(OpCode::VAR_GET).addString("__for_index"));
    instructions.push_back(Instruction(OpCode::ARR_GET));
    instructions.push_back(Instruction(OpCode::VAR_SET).addString(node->value));

    // 循环体
    if (node->body) {
        compileNode(node->body, instructions);
    }

    // continue跳转点
    size_t continueTarget = instructions.size();

    // 增加索引
    instructions.push_back(Instruction(OpCode::VAR_GET).addString("__for_index"));
    instructions.push_back(Instruction(OpCode::PUSH_INT).addInt64(1));
    instructions.push_back(Instruction(OpCode::ADD));
    instructions.push_back(Instruction(OpCode::VAR_SET).addString("__for_index"));

    // 跳回开始
    instructions.push_back(Instruction(OpCode::JUMP).addInt32(
        static_cast<int32_t>(byteOffsetForInstructionIndex(instructions, loopStart))));

    // 补丁exit跳转
    size_t exitOffset = instructions.size();
    patchJump(instructions, exitJumpOffset, exitOffset);

    // 补丁break跳转
    for (size_t jump : g_breakPatchList.back()) {
        patchJump(instructions, jump, exitOffset);
    }

    // 补丁continue跳转
    for (size_t jump : g_continuePatchList.back()) {
        patchJump(instructions, jump, continueTarget);
    }

    g_breakPatchList.pop_back();
    g_continuePatchList.pop_back();
    g_loopStarts.pop_back();

    // 清理临时变量
    instructions.push_back(Instruction(OpCode::VAR_DEL).addString("__for_list"));
    instructions.push_back(Instruction(OpCode::VAR_DEL).addString("__for_index"));
}

void Compiler::compileCaseStatement(ASTNodePtr node, InstructionList& instructions) {
    // 编译case表达式
    compileNode(node->left, instructions);

    // 存储case值到临时变量
    instructions.push_back(Instruction(OpCode::VAR_SET).addString("__case_value"));

    std::vector<size_t> endJumps;

    for (const auto& branch : node->elifBranches) {
        // 获取case值
        instructions.push_back(Instruction(OpCode::VAR_GET).addString("__case_value"));
        // 编译模式
        compileNode(branch.first, instructions);

        // 比较相等
        instructions.push_back(Instruction(OpCode::EQ));

        // 检查是否为通配符 *
        // 如果模式是 "*"，总是匹配
        if (branch.first && branch.first->type == NodeType::Literal &&
            branch.first->literalValue.isString() &&
            branch.first->literalValue.asString() == "*") {
            // 通配符总是匹配，不跳过
        } else {
            size_t skipJump = instructions.size();
            instructions.push_back(Instruction(OpCode::JUMP_IF_NOT).addInt32(0));
            patchJump(instructions, skipJump, instructions.size() + 1); // 暂时不跳
        }

        // 编译分支体
        if (branch.second) {
            compileNode(branch.second, instructions);
        }

        // 跳转到case结束
        size_t endJump = instructions.size();
        instructions.push_back(Instruction(OpCode::JUMP).addInt32(0));
        endJumps.push_back(endJump);
    }

    // 补丁所有end跳转
    for (size_t jump : endJumps) {
        patchJump(instructions, jump, instructions.size());
    }

    // 清理临时变量
    instructions.push_back(Instruction(OpCode::VAR_DEL).addString("__case_value"));
}

void Compiler::compileFunctionDef(ASTNodePtr node, InstructionList& instructions) {
    // 记录函数名
    g_knownFunctions.insert(node->value);

    // 跳过函数体
    size_t skipJumpOffset = instructions.size();
    instructions.push_back(Instruction(OpCode::JUMP).addInt32(0));

    // 函数体起始地址
    size_t funcStart = instructions.size();
    g_functionAddresses[node->value] = static_cast<int32_t>(currentByteOffset(instructions));

    // 编译函数体
    if (node->body) {
        compileNode(node->body, instructions);
    }

    // 函数返回
    instructions.push_back(Instruction(OpCode::RET));

    // 补丁skip跳转
    size_t afterFunc = instructions.size();
    patchJump(instructions, skipJumpOffset, afterFunc);

    // 存储函数地址到变量（以便运行时查找）
    instructions.push_back(Instruction(OpCode::PUSH_INT).addInt64(static_cast<int64_t>(funcStart)));
    instructions.push_back(Instruction(OpCode::VAR_SET).addString("__func_" + node->value));
}

void Compiler::compileFunctionCall(ASTNodePtr node, InstructionList& instructions) {
    // 查找函数地址
    auto it = g_functionAddresses.find(node->value);
    int32_t target = 0;
    if (it != g_functionAddresses.end()) {
        target = it->second;
    }

    // 编译参数到栈上
    for (auto& arg : node->children) {
        compileNode(arg, instructions);
    }

    // 调用函数
    instructions.push_back(Instruction(OpCode::CALL)
        .addInt32(target)
        .addByte(static_cast<uint8_t>(node->children.size())));
}

void Compiler::compileBlock(ASTNodePtr node, InstructionList& instructions) {
    for (auto& child : node->children) {
        compileNode(child, instructions);
    }
}

void Compiler::compileSubshell(ASTNodePtr node, InstructionList& instructions) {
    // 子shell：编译内部语句
    if (node->body) {
        compileNode(node->body, instructions);
    }
}

void Compiler::compileBackground(ASTNodePtr node, InstructionList& instructions) {
    // 后台执行：编译内部命令，使用CMD_ASYNC
    if (node->left) {
        if (node->left->type == NodeType::Command) {
            // 构建CMD_ASYNC指令
            Instruction cmdInst(OpCode::CMD_ASYNC);
            cmdInst.addString(node->left->value);
            cmdInst.addByte(static_cast<uint8_t>(node->left->children.size()));

            for (auto& arg : node->left->children) {
                std::string argStr = nodeToRawString(arg);
                cmdInst.addString(argStr);
            }

            instructions.push_back(cmdInst);
        } else if (node->left->type == NodeType::Subshell) {
            // ( ... ) & 子shell后台执行
            if (node->left->body) {
                compileNode(node->left->body, instructions);
            }
        } else {
            compileNode(node->left, instructions);
        }
    }
}

void Compiler::compileReturn(ASTNodePtr node, InstructionList& instructions) {
    if (node->right) {
        compileNode(node->right, instructions);
    }
    instructions.push_back(Instruction(OpCode::RET));
}

void Compiler::compileExit(ASTNodePtr node, InstructionList& instructions) {
    if (node->literalValue.isInteger()) {
        instructions.push_back(Instruction(OpCode::PUSH_INT)
            .addInt64(node->literalValue.asInteger()));
    } else if (node->right) {
        compileNode(node->right, instructions);
    } else {
        instructions.push_back(Instruction(OpCode::PUSH_INT).addInt64(0));
    }
    instructions.push_back(Instruction(OpCode::HALT));
}

void Compiler::compileBreak(ASTNodePtr node, InstructionList& instructions) {
    (void)node; // break 不需要AST节点内容
    if (!g_breakPatchList.empty()) {
        size_t jumpOffset = instructions.size();
        instructions.push_back(Instruction(OpCode::JUMP).addInt32(0));
        g_breakPatchList.back().push_back(jumpOffset);
    }
}

void Compiler::compileContinue(ASTNodePtr node, InstructionList& instructions) {
    (void)node; // continue 不需要AST节点内容
    if (!g_continuePatchList.empty()) {
        size_t jumpOffset = instructions.size();
        instructions.push_back(Instruction(OpCode::JUMP).addInt32(0));
        g_continuePatchList.back().push_back(jumpOffset);
    }
}

void Compiler::compileArithmeticExpansion(ASTNodePtr node, InstructionList& instructions) {
    // node->value 包含 $((expr))
    std::string expr;
    if (node->value.size() >= 4 && node->value.substr(0, 3) == "$((") {
        expr = node->value.substr(3, node->value.length() - 5); // 去掉 $(( 和 ))
    } else if (node->value.size() >= 2 && node->value.substr(0, 2) == "((") {
        expr = node->value.substr(2, node->value.length() - 4);
    } else {
        expr = node->value;
    }

    // 解析并编译算术表达式
    size_t pos = 0;

    // 简化版算术表达式编译器
    auto parseFactor = [&instructions, &expr, &pos](auto& self) -> void {
        // 跳过空格
        while (pos < expr.size() && expr[pos] == ' ') pos++;
        
        if (pos >= expr.size()) {
            instructions.push_back(Instruction(OpCode::PUSH_INT).addInt64(0));
            return;
        }
        
        if (expr[pos] == '(') {
            pos++; // (
            // parseExpr (递归)
            // 由于lambda无法直接递归，我们使用简单方法
            int depth = 1;
            std::string subExpr;
            while (pos < expr.size() && depth > 0) {
                if (expr[pos] == '(') depth++;
                else if (expr[pos] == ')') {
                    depth--;
                    if (depth == 0) { pos++; break; }
                }
                if (depth > 0) subExpr += expr[pos++];
            }
            // 递归编译子表达式
            // 简化处理：直接编译为整数
            // 实际应该递归调用
            instructions.push_back(Instruction(OpCode::PUSH_INT).addInt64(0));
            return;
        }
        
        if (expr[pos] == '-') {
            pos++;
            self(self); // parseFactor
            instructions.push_back(Instruction(OpCode::NEG));
            return;
        }
        
        if (expr[pos] == '!') {
            pos++;
            self(self);
            instructions.push_back(Instruction(OpCode::NOT));
            return;
        }
        
        // 数字
        if (isdigit(static_cast<unsigned char>(expr[pos]))) {
            std::string num;
            while (pos < expr.size() && isdigit(static_cast<unsigned char>(expr[pos]))) {
                num += expr[pos++];
            }
            // 跳过后缀
            if (pos < expr.size() && expr[pos] == 'x') {
                // 十六进制 0x...
                pos++;
                std::string hex;
                while (pos < expr.size() && isxdigit(static_cast<unsigned char>(expr[pos]))) {
                    hex += expr[pos++];
                }
                instructions.push_back(Instruction(OpCode::PUSH_INT)
                    .addInt64(static_cast<int64_t>(std::stoll(hex, nullptr, 16))));
            } else {
                instructions.push_back(Instruction(OpCode::PUSH_INT)
                    .addInt64(std::stoll(num)));
            }
            return;
        }
        
        // 变量
        if (isalpha(static_cast<unsigned char>(expr[pos])) || expr[pos] == '_') {
            std::string varName;
            while (pos < expr.size() && 
                   (isalnum(static_cast<unsigned char>(expr[pos])) || expr[pos] == '_')) {
                varName += expr[pos++];
            }
            // 跳过点号路径
            while (pos < expr.size() && expr[pos] == '.' && 
                   pos + 1 < expr.size() && 
                   (isalnum(static_cast<unsigned char>(expr[pos + 1])) || expr[pos + 1] == '_')) {
                varName += expr[pos++];
                while (pos < expr.size() && 
                       (isalnum(static_cast<unsigned char>(expr[pos])) || expr[pos] == '_')) {
                    varName += expr[pos++];
                }
            }
            instructions.push_back(Instruction(OpCode::VAR_GET).addString(varName));
            instructions.push_back(Instruction(OpCode::CAST_INT));
            return;
        }
        
        // 默认
        instructions.push_back(Instruction(OpCode::PUSH_INT).addInt64(0));
    };
    
    auto parseTerm = [&instructions, &expr, &pos, &parseFactor](auto& self) -> void {
        (void)self; // parseTerm 不需要递归自身，通过捕获的 parseFactor 调用
        parseFactor(parseFactor);
        
        while (pos < expr.size()) {
            while (pos < expr.size() && expr[pos] == ' ') pos++;
            if (pos >= expr.size()) break;
            
            char op = expr[pos];
            if (op == '*' || op == '/' || op == '%') {
                pos++;
                parseFactor(parseFactor);
                if (op == '*') {
                    instructions.push_back(Instruction(OpCode::MUL));
                } else if (op == '/') {
                    instructions.push_back(Instruction(OpCode::DIV));
                } else {
                    instructions.push_back(Instruction(OpCode::MOD));
                }
            } else {
                break;
            }
        }
    };
    
    // 解析加法和减法
    auto parseAddExpr = [&instructions, &expr, &pos, &parseTerm](auto& self) -> void {
        (void)self; // parseAddExpr 不需要递归自身，通过捕获的 parseTerm 调用
        parseTerm(parseTerm);
        
        while (pos < expr.size()) {
            while (pos < expr.size() && expr[pos] == ' ') pos++;
            if (pos >= expr.size()) break;
            
            char op = expr[pos];
            if (op == '+' || op == '-') {
                pos++;
                parseTerm(parseTerm);
                if (op == '+') {
                    instructions.push_back(Instruction(OpCode::ADD));
                } else {
                    instructions.push_back(Instruction(OpCode::SUB));
                }
            } else {
                break;
            }
        }
    };
    
    // 解析比较
    auto parseCmpExpr = [&instructions, &expr, &pos, &parseAddExpr](auto& self) -> void {
        (void)self; // parseCmpExpr 不需要递归自身，通过捕获的 parseAddExpr 调用
        parseAddExpr(parseAddExpr);
        
        while (pos < expr.size()) {
            while (pos < expr.size() && expr[pos] == ' ') pos++;
            if (pos >= expr.size()) break;
            
            // 检查比较操作符
            if (pos + 1 < expr.size()) {
                std::string two = expr.substr(pos, 2);
                if (two == "==") { pos += 2; parseAddExpr(parseAddExpr); instructions.push_back(Instruction(OpCode::EQ)); continue; }
                if (two == "!=") { pos += 2; parseAddExpr(parseAddExpr); instructions.push_back(Instruction(OpCode::NE)); continue; }
                if (two == "<=") { pos += 2; parseAddExpr(parseAddExpr); instructions.push_back(Instruction(OpCode::LE)); continue; }
                if (two == ">=") { pos += 2; parseAddExpr(parseAddExpr); instructions.push_back(Instruction(OpCode::GE)); continue; }
                if (two == "&&") { pos += 2; parseAddExpr(parseAddExpr); instructions.push_back(Instruction(OpCode::AND)); continue; }
                if (two == "||") { pos += 2; parseAddExpr(parseAddExpr); instructions.push_back(Instruction(OpCode::OR)); continue; }
            }
            char op = expr[pos];
            if (op == '<') { pos++; parseAddExpr(parseAddExpr); instructions.push_back(Instruction(OpCode::LT)); continue; }
            if (op == '>') { pos++; parseAddExpr(parseAddExpr); instructions.push_back(Instruction(OpCode::GT)); continue; }
            break;
        }
    };
    
    parseCmpExpr(parseCmpExpr);
}

void Compiler::compileParameterExpansion(ASTNodePtr node, InstructionList& instructions) {
    // node->value 包含 ${...}
    std::string content = node->value;
    
    // 去掉 ${ 和 }
    if (content.size() >= 3 && content[0] == '$' && content[1] == '{') {
        content = content.substr(2, content.length() - 3);
    } else if (content.size() >= 2 && content[0] == '{') {
        content = content.substr(1, content.length() - 2);
    }

    // ${#var} - 字符串长度
    if (!content.empty() && content[0] == '#') {
        std::string varName = content.substr(1);
        instructions.push_back(Instruction(OpCode::VAR_GET).addString(varName));
        instructions.push_back(Instruction(OpCode::STR_LEN));
        return;
    }

    // 查找冒号 (用于 ${var:offset:length})
    size_t colonPos = content.find(':');
    if (colonPos != std::string::npos) {
        std::string varName = content.substr(0, colonPos);
        std::string rest = content.substr(colonPos + 1);
        
        // 查找第二个冒号
        size_t secondColon = rest.find(':');
        if (secondColon != std::string::npos) {
            // ${var:offset:length}
            std::string offsetStr = rest.substr(0, secondColon);
            std::string lengthStr = rest.substr(secondColon + 1);
            
            instructions.push_back(Instruction(OpCode::VAR_GET).addString(varName));
            
            // 检查offset和length是否为字面量
            bool offsetIsLiteral = true;
            for (char c : offsetStr) {
                if (!isdigit(static_cast<unsigned char>(c)) && c != '-') {
                    offsetIsLiteral = false;
                    break;
                }
            }
            bool lengthIsLiteral = true;
            for (char c : lengthStr) {
                if (!isdigit(static_cast<unsigned char>(c)) && c != '-') {
                    lengthIsLiteral = false;
                    break;
                }
            }
            
            if (offsetIsLiteral && lengthIsLiteral) {
                int32_t offset = offsetStr.empty() ? 0 : std::stoi(offsetStr);
                int32_t length = lengthStr.empty() ? -1 : std::stoi(lengthStr);
                if (length < 0) {
                    // 取到末尾
                    instructions.push_back(Instruction(OpCode::STR_SUB)
                        .addInt32(static_cast<int32_t>(std::string::npos))
                        .addInt32(offset));
                } else {
                    instructions.push_back(Instruction(OpCode::STR_SUB)
                        .addInt32(length)
                        .addInt32(offset));
                }
            } else {
                // 动态偏移/长度：获取变量值，存储，使用GET_OUTPUT
                // 简化处理：嵌入原始文本
                instructions.push_back(Instruction(OpCode::POP)); // 弹出VAR_GET的结果
                instructions.push_back(Instruction(OpCode::PUSH_STR).addString(node->value));
            }
            return;
        } else {
            // ${var:offset} - 从offset到末尾
            instructions.push_back(Instruction(OpCode::VAR_GET).addString(varName));
            
            bool offsetIsLiteral = true;
            for (char c : rest) {
                if (!isdigit(static_cast<unsigned char>(c)) && c != '-') {
                    offsetIsLiteral = false;
                    break;
                }
            }
            
            if (offsetIsLiteral) {
                int32_t offset = rest.empty() ? 0 : std::stoi(rest);
                instructions.push_back(Instruction(OpCode::STR_SUB)
                    .addInt32(static_cast<int32_t>(std::string::npos))
                    .addInt32(offset));
            } else {
                instructions.push_back(Instruction(OpCode::POP));
                instructions.push_back(Instruction(OpCode::PUSH_STR).addString(node->value));
            }
            return;
        }
    }

    // ${var:-default} - 默认值
    size_t dashPos = content.find(":-");
    if (dashPos != std::string::npos) {
        std::string varName = content.substr(0, dashPos);
        std::string defaultVal = content.substr(dashPos + 2);
        
        // 获取变量值
        instructions.push_back(Instruction(OpCode::VAR_GET).addString(varName));
        // 检查是否为空
        instructions.push_back(Instruction(OpCode::DUP));
        instructions.push_back(Instruction(OpCode::STR_LEN));
        
        size_t skipJump = instructions.size();
        instructions.push_back(Instruction(OpCode::JUMP_IF_NOT).addInt32(0));
        
        // 非空，使用变量值
        instructions.push_back(Instruction(OpCode::POP)); // 弹出长度
        size_t endJump = instructions.size();
        instructions.push_back(Instruction(OpCode::JUMP).addInt32(0));
        
        // 空，使用默认值
        patchJump(instructions, skipJump, instructions.size());
        instructions.push_back(Instruction(OpCode::POP)); // 弹出空值
        instructions.push_back(Instruction(OpCode::PUSH_STR).addString(defaultVal));
        
        patchJump(instructions, endJump, instructions.size());
        return;
    }

    // ${var=default} - 赋默认值
    size_t eqPos = content.find('=');
    if (eqPos != std::string::npos && (eqPos == 0 || (content[eqPos-1] != '!' && content[eqPos-1] != '<' && content[eqPos-1] != '>'))) {
        std::string varName = content.substr(0, eqPos);
        std::string defaultVal = content.substr(eqPos + 1);
        
        instructions.push_back(Instruction(OpCode::VAR_GET).addString(varName));
        instructions.push_back(Instruction(OpCode::DUP));
        instructions.push_back(Instruction(OpCode::STR_LEN));
        
        size_t skipJump = instructions.size();
        instructions.push_back(Instruction(OpCode::JUMP_IF_NOT).addInt32(0));
        
        instructions.push_back(Instruction(OpCode::POP));
        size_t endJump = instructions.size();
        instructions.push_back(Instruction(OpCode::JUMP).addInt32(0));
        
        patchJump(instructions, skipJump, instructions.size());
        instructions.push_back(Instruction(OpCode::POP));
        instructions.push_back(Instruction(OpCode::PUSH_STR).addString(defaultVal));
        instructions.push_back(Instruction(OpCode::DUP));
        instructions.push_back(Instruction(OpCode::VAR_SET).addString(varName));
        
        patchJump(instructions, endJump, instructions.size());
        return;
    }

    // 普通变量引用 ${var}
    instructions.push_back(Instruction(OpCode::VAR_GET).addString(content));
}

void Compiler::compileTestExpression(ASTNodePtr node, InstructionList& instructions) {
    if (node->left) {
        compileNode(node->left, instructions);
    } else {
        instructions.push_back(Instruction(OpCode::PUSH_BOOL).addByte(0));
    }
}

void Compiler::compileVariable(ASTNodePtr node, InstructionList& instructions) {
    // 特殊变量处理
    const std::string& varName = node->value;
    
    if (varName == "?") {
        // $? - 退出码
        instructions.push_back(Instruction(OpCode::GET_EXIT_CODE));
        return;
    }
    
    if (varName == "@") {
        // $@ - 所有参数（作为数组）
        instructions.push_back(Instruction(OpCode::VAR_GET).addString("__args"));
        return;
    }
    
    if (varName == "*") {
        // $* - 所有参数（作为字符串）
        instructions.push_back(Instruction(OpCode::VAR_GET).addString("__args"));
        instructions.push_back(Instruction(OpCode::ARR_JOIN).addString(" "));
        return;
    }
    
    if (varName == "#") {
        // $# - 参数数量
        instructions.push_back(Instruction(OpCode::VAR_GET).addString("__args"));
        instructions.push_back(Instruction(OpCode::ARR_LEN));
        return;
    }
    
    if (varName == "$") {
        // $$ - 当前进程ID
        instructions.push_back(Instruction(OpCode::PUSH_INT).addInt64(static_cast<int64_t>(SHELLVM_GETPID())));
        return;
    }
    
    if (varName == "!") {
        // $! - 最后一个后台进程ID
        instructions.push_back(Instruction(OpCode::VAR_GET).addString("__last_pid"));
        return;
    }

    // 普通变量
    instructions.push_back(Instruction(OpCode::VAR_GET).addString(varName));
}

void Compiler::compileLiteral(ASTNodePtr node, InstructionList& instructions) {
    if (node->literalValue.isString()) {
        instructions.push_back(Instruction(OpCode::PUSH_STR)
            .addString(node->literalValue.asString()));
    } else if (node->literalValue.isInteger()) {
        instructions.push_back(Instruction(OpCode::PUSH_INT)
            .addInt64(node->literalValue.asInteger()));
    } else if (node->literalValue.isFloat()) {
        instructions.push_back(Instruction(OpCode::PUSH_FLOAT)
            .addFloat(node->literalValue.asFloat()));
    } else if (node->literalValue.isBoolean()) {
        instructions.push_back(Instruction(OpCode::PUSH_BOOL)
            .addByte(node->literalValue.asBoolean() ? 1 : 0));
    } else if (node->literalValue.isNull()) {
        instructions.push_back(Instruction(OpCode::PUSH_NULL));
    } else {
        instructions.push_back(Instruction(OpCode::PUSH_NULL));
    }
}

void Compiler::patchJump(InstructionList& instructions, size_t offset, size_t target) {
    if (offset < instructions.size()) {
        int32_t byteTarget = static_cast<int32_t>(byteOffsetForInstructionIndex(instructions, target));
        auto& inst = instructions[offset];
        inst.operands.clear();
        inst.addInt32(byteTarget);
    }
}

// ============================================================================
// 反汇编实现
// ============================================================================

std::string Compiler::disassemble(const Bytecode& bytecode) {
    std::ostringstream oss;
    size_t ip = 0;

    while (ip < bytecode.size()) {
        oss << std::hex << std::setw(4) << std::setfill('0') << ip << ": ";

        OpCode opcode = static_cast<OpCode>(bytecode[ip++]);

        // 读取int32
        auto readInt32 = [&bytecode, &ip]() -> int32_t {
            if (ip + 4 > bytecode.size()) return 0;
            int32_t value = static_cast<int32_t>(
                bytecode[ip] | (bytecode[ip + 1] << 8) |
                (bytecode[ip + 2] << 16) | (bytecode[ip + 3] << 24));
            ip += 4;
            return value;
        };

        // 读取int64
        auto readInt64 = [&bytecode, &ip]() -> int64_t {
            if (ip + 8 > bytecode.size()) return 0;
            int64_t value = 0;
            for (int i = 0; i < 8; ++i) {
                value |= static_cast<int64_t>(bytecode[ip + i]) << (i * 8);
            }
            ip += 8;
            return value;
        };

        // 读取string
        auto readString = [&bytecode, &ip]() -> std::string {
            int32_t len = static_cast<int32_t>(
                bytecode[ip] | (bytecode[ip + 1] << 8) |
                (bytecode[ip + 2] << 16) | (bytecode[ip + 3] << 24));
            ip += 4;
            if (len < 0 || ip + len > bytecode.size()) return "";
            std::string s(reinterpret_cast<const char*>(&bytecode[ip]), len);
            ip += len;
            return s;
        };

        switch (opcode) {
            case OpCode::NOP:
                oss << "NOP";
                break;
            case OpCode::PUSH_NULL:
                oss << "PUSH_NULL";
                break;
            case OpCode::PUSH_INT:
                oss << "PUSH_INT " << std::dec << readInt64();
                break;
            case OpCode::PUSH_FLOAT: {
                int64_t bits = readInt64();
                double val;
                std::memcpy(&val, &bits, sizeof(double));
                oss << "PUSH_FLOAT " << val;
                break;
            }
            case OpCode::PUSH_STR:
                oss << "PUSH_STR \"" << readString() << "\"";
                break;
            case OpCode::PUSH_BOOL: {
                uint8_t v = bytecode[ip++];
                oss << "PUSH_BOOL " << (v ? "true" : "false");
                break;
            }
            case OpCode::POP:
                oss << "POP";
                break;
            case OpCode::DUP:
                oss << "DUP";
                break;
            case OpCode::SWAP:
                oss << "SWAP";
                break;
            case OpCode::ADD:
                oss << "ADD";
                break;
            case OpCode::SUB:
                oss << "SUB";
                break;
            case OpCode::MUL:
                oss << "MUL";
                break;
            case OpCode::DIV:
                oss << "DIV";
                break;
            case OpCode::MOD:
                oss << "MOD";
                break;
            case OpCode::NEG:
                oss << "NEG";
                break;
            case OpCode::EQ:
                oss << "EQ";
                break;
            case OpCode::NE:
                oss << "NE";
                break;
            case OpCode::LT:
                oss << "LT";
                break;
            case OpCode::LE:
                oss << "LE";
                break;
            case OpCode::GT:
                oss << "GT";
                break;
            case OpCode::GE:
                oss << "GE";
                break;
            case OpCode::AND:
                oss << "AND";
                break;
            case OpCode::OR:
                oss << "OR";
                break;
            case OpCode::NOT:
                oss << "NOT";
                break;
            case OpCode::VAR_SET: {
                std::string name = readString();
                oss << "VAR_SET " << name;
                break;
            }
            case OpCode::VAR_GET: {
                std::string name = readString();
                oss << "VAR_GET " << name;
                break;
            }
            case OpCode::VAR_DEL: {
                std::string name = readString();
                oss << "VAR_DEL " << name;
                break;
            }
            case OpCode::CMD: {
                std::string cmd = readString();
                uint8_t argCount = bytecode[ip++];
                oss << "CMD " << cmd << " (" << std::dec << (int)argCount << " args)";
                for (int i = 0; i < argCount; ++i) {
                    std::string arg = readString();
                    oss << " arg" << i << "=\"" << arg << "\"";
                }
                break;
            }
            case OpCode::CMD_ASYNC: {
                std::string cmd = readString();
                uint8_t argCount = bytecode[ip++];
                oss << "CMD_ASYNC " << cmd << " (" << std::dec << (int)argCount << " args)";
                for (int i = 0; i < argCount; ++i) {
                    std::string arg = readString();
                    oss << " arg" << i << "=\"" << arg << "\"";
                }
                break;
            }
            case OpCode::PIPE:
                oss << "PIPE";
                break;
            case OpCode::REDIRECT_OUT:
                oss << "REDIRECT_OUT";
                break;
            case OpCode::REDIRECT_IN:
                oss << "REDIRECT_IN";
                break;
            case OpCode::REDIRECT_APPEND:
                oss << "REDIRECT_APPEND";
                break;
            case OpCode::GET_EXIT_CODE:
                oss << "GET_EXIT_CODE";
                break;
            case OpCode::GET_OUTPUT:
                oss << "GET_OUTPUT";
                break;
            case OpCode::JUMP: {
                int32_t target = readInt32();
                oss << "JUMP " << std::dec << target;
                break;
            }
            case OpCode::JUMP_IF: {
                int32_t target = readInt32();
                oss << "JUMP_IF " << std::dec << target;
                break;
            }
            case OpCode::JUMP_IF_NOT: {
                int32_t target = readInt32();
                oss << "JUMP_IF_NOT " << std::dec << target;
                break;
            }
            case OpCode::CALL: {
                int32_t target = readInt32();
                uint8_t argCount = bytecode[ip++];
                oss << "CALL " << std::dec << target << " (" << (int)argCount << " args)";
                break;
            }
            case OpCode::RET:
                oss << "RET";
                break;
            case OpCode::HALT:
                oss << "HALT";
                break;
            case OpCode::STR_CAT:
                oss << "STR_CAT";
                break;
            case OpCode::STR_LEN:
                oss << "STR_LEN";
                break;
            case OpCode::STR_SUB: {
                int32_t length = readInt32();
                int32_t start = readInt32();
                oss << "STR_SUB len=" << std::dec << length << " start=" << start;
                break;
            }
            case OpCode::STR_SPLIT: {
                std::string delim = readString();
                oss << "STR_SPLIT \"" << delim << "\"";
                break;
            }
            case OpCode::ARR_NEW:
                oss << "ARR_NEW";
                break;
            case OpCode::ARR_PUSH:
                oss << "ARR_PUSH";
                break;
            case OpCode::ARR_GET:
                oss << "ARR_GET";
                break;
            case OpCode::ARR_LEN:
                oss << "ARR_LEN";
                break;
            case OpCode::ARR_JOIN: {
                std::string delim = readString();
                oss << "ARR_JOIN \"" << delim << "\"";
                break;
            }
            case OpCode::TYPEOF:
                oss << "TYPEOF";
                break;
            case OpCode::CAST_INT:
                oss << "CAST_INT";
                break;
            case OpCode::CAST_FLOAT:
                oss << "CAST_FLOAT";
                break;
            case OpCode::CAST_STR:
                oss << "CAST_STR";
                break;
            case OpCode::CAST_BOOL:
                oss << "CAST_BOOL";
                break;
            case OpCode::ENV_GET: {
                std::string name = readString();
                oss << "ENV_GET " << name;
                break;
            }
            case OpCode::ENV_SET: {
                std::string name = readString();
                oss << "ENV_SET " << name;
                break;
            }
            case OpCode::CWD_GET:
                oss << "CWD_GET";
                break;
            case OpCode::CWD_SET:
                oss << "CWD_SET";
                break;
            case OpCode::FILE_READ:
                oss << "FILE_READ";
                break;
            case OpCode::FILE_WRITE:
                oss << "FILE_WRITE";
                break;
            case OpCode::FILE_EXISTS:
                oss << "FILE_EXISTS";
                break;
            case OpCode::FILE_DELETE:
                oss << "FILE_DELETE";
                break;
            default:
                oss << "UNKNOWN_" << static_cast<int>(opcode);
                break;
        }

        oss << "\n";
    }

    return oss.str();
}

} // namespace shellvm
