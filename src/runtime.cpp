/**
 * @file runtime.cpp
 * @brief Shell运行时库实现
 * @license MIT License
 */

#include "runtime.h"
#include "vm_core.h"
#include "compiler.h"
#include <iostream>
#include <sstream>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <thread>
#include <mutex>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <signal.h>
#include <errno.h>
#endif

namespace shellvm {

// ============================================================================
// 全局运行时实例
// ============================================================================

static std::mutex runtimeMutex;

namespace {

static constexpr char kLiteralMarkerStart = '\x1D';
static constexpr char kLiteralMarkerEnd = '\x1E';

std::string trimCommandSubstitutionOutput(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

std::string stripLiteralMarkers(const std::string& text) {
    std::string stripped;
    stripped.reserve(text.size());
    for (char ch : text) {
        if (ch == kLiteralMarkerStart || ch == kLiteralMarkerEnd) {
            continue;
        }
        stripped += ch;
    }
    return stripped;
}

bool isTraceEnabled() {
    const char* value = std::getenv("SHELLVM_TRACE");
    return value && std::string(value) == "1";
}

std::string joinArgsForTrace(const std::vector<std::string>& args) {
    std::ostringstream oss;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) {
            oss << ", ";
        }
        oss << "\"" << args[i] << "\"";
    }
    return oss.str();
}

std::string shellQuoteArg(const std::string& arg) {
    std::string escaped;
    escaped.reserve(arg.size() + 8);
    for (char ch : arg) {
        if (ch == '\\' || ch == '"') {
            escaped += '\\';
        }
        escaped += ch;
    }
    return "\"" + escaped + "\"";
}

bool isAbsolutePath(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    if (path[0] == '/' || path[0] == '\\') {
        return true;
    }
    return path.size() > 1 && path[1] == ':';
}

std::string joinPath(const std::string& base, const std::string& name) {
    if (base.empty()) {
        return name;
    }
    if (name.empty()) {
        return base;
    }

    char last = base.back();
    if (last == '/' || last == '\\') {
        return base + name;
    }
    return base + "/" + name;
}

std::string resolvePathFromCwd(const std::string& path) {
    if (path.empty() || isAbsolutePath(path)) {
        return path;
    }

#ifdef _WIN32
    char buffer[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, buffer);
    return joinPath(std::string(buffer), path);
#else
    char buffer[PATH_MAX];
    if (getcwd(buffer, sizeof(buffer)) == nullptr) {
        return path;
    }
    return joinPath(std::string(buffer), path);
#endif
}

CommandResult executeExternalCommandArgv(
    const std::string& command,
    const std::vector<std::string>& args) {

#ifdef _WIN32
    (void)command;
    (void)args;
    return CommandResult(127, "", "Windows runtime path disabled; use Android target");
#else
    int stdoutPipe[2];
    if (pipe(stdoutPipe) != 0) {
        return CommandResult(127, "", "Failed to create stdout pipe");
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdoutPipe[0]);
        close(stdoutPipe[1]);
        return CommandResult(127, "", "Failed to fork");
    }

    if (pid == 0) {
        dup2(stdoutPipe[1], STDOUT_FILENO);
        close(stdoutPipe[0]);
        close(stdoutPipe[1]);

        std::vector<char*> argv;
        argv.reserve(args.size() + 2);
        argv.push_back(const_cast<char*>(command.c_str()));
        for (const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execvp(command.c_str(), argv.data());
        _exit(errno == ENOENT ? 127 : 126);
    }

    close(stdoutPipe[1]);

    std::string output;
    char buffer[256];
    ssize_t bytesRead = 0;
    while ((bytesRead = read(stdoutPipe[0], buffer, sizeof(buffer))) > 0) {
        output.append(buffer, buffer + bytesRead);
    }
    close(stdoutPipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    int exitCode = 127;
    if (WIFEXITED(status)) {
        exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        exitCode = 128 + WTERMSIG(status);
    }

    return CommandResult(exitCode, output, "");
#endif
}

int64_t readArithmeticVariable(const std::string& name, VM& vm) {
    if (name.empty()) {
        return 0;
    }
    return vm.getVariable(name).asInteger();
}

int64_t evaluateArithmeticExpr(const std::string& expr, VM& vm) {
    struct Parser {
        const std::string& text;
        size_t pos = 0;
        VM& vm;

        void skipSpaces() {
            while (pos < text.size() && isspace(static_cast<unsigned char>(text[pos]))) {
                ++pos;
            }
        }

        bool consume(char ch) {
            skipSpaces();
            if (pos < text.size() && text[pos] == ch) {
                ++pos;
                return true;
            }
            return false;
        }

        int64_t parseExpression() {
            int64_t value = parseTerm();
            while (true) {
                skipSpaces();
                if (consume('+')) {
                    value += parseTerm();
                } else if (consume('-')) {
                    value -= parseTerm();
                } else {
                    break;
                }
            }
            return value;
        }

        int64_t parseTerm() {
            int64_t value = parseFactor();
            while (true) {
                skipSpaces();
                if (consume('*')) {
                    value *= parseFactor();
                } else if (consume('/')) {
                    int64_t rhs = parseFactor();
                    value = rhs == 0 ? 0 : (value / rhs);
                } else if (consume('%')) {
                    int64_t rhs = parseFactor();
                    value = rhs == 0 ? 0 : (value % rhs);
                } else {
                    break;
                }
            }
            return value;
        }

        int64_t parseNumber() {
            skipSpaces();
            size_t start = pos;
            while (pos < text.size() && isdigit(static_cast<unsigned char>(text[pos]))) {
                ++pos;
            }
            if (start == pos) {
                return 0;
            }
            return std::stoll(text.substr(start, pos - start));
        }

        std::string parseIdentifier() {
            skipSpaces();
            size_t start = pos;
            while (pos < text.size() &&
                   (isalnum(static_cast<unsigned char>(text[pos])) || text[pos] == '_')) {
                ++pos;
            }
            return text.substr(start, pos - start);
        }

        int64_t parseFactor() {
            skipSpaces();
            if (consume('(')) {
                int64_t value = parseExpression();
                consume(')');
                return value;
            }
            if (consume('-')) {
                return -parseFactor();
            }
            if (consume('+')) {
                return parseFactor();
            }
            if (pos < text.size() && isdigit(static_cast<unsigned char>(text[pos]))) {
                return parseNumber();
            }
            if (pos < text.size() &&
                (isalpha(static_cast<unsigned char>(text[pos])) || text[pos] == '_')) {
                return readArithmeticVariable(parseIdentifier(), vm);
            }
            return 0;
        }
    };

    Parser parser{expr, 0, vm};
    return parser.parseExpression();
}

std::string rawNodeToString(ASTNodePtr node) {
    if (!node) {
        return "";
    }

    switch (node->type) {
        case NodeType::Literal:
            return node->literalValue.asString();
        case NodeType::Variable:
            return "$" + node->value;
        case NodeType::CommandSubstitution:
        case NodeType::ArithmeticExpansion:
        case NodeType::ParameterExpansion:
            return node->value;
        default:
            return node->value;
    }
}

std::string expandString(const std::string& str, VM& vm);

std::string shellCommandToken(const std::string& text) {
    if (text.empty()) {
        return "\"\"";
    }

    for (char ch : text) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) &&
            ch != '_' && ch != '-' && ch != '.' &&
            ch != '/' && ch != ':' && ch != '[' && ch != ']') {
            return shellQuoteArg(text);
        }
    }
    return text;
}

std::string buildShellCommandText(ASTNodePtr node, VM& vm) {
    if (!node) {
        return "";
    }

    if (node->type == NodeType::Pipeline) {
        std::string left = buildShellCommandText(node->left, vm);
        std::string right = buildShellCommandText(node->right, vm);
        if (left.empty()) {
            return right;
        }
        if (right.empty()) {
            return left;
        }
        return left + " | " + right;
    }

    if (node->type != NodeType::Command) {
        return "";
    }

    std::string fullCommand = shellCommandToken(expandString(node->value, vm));
    for (const auto& child : node->children) {
        fullCommand += " " + shellQuoteArg(expandString(rawNodeToString(child), vm));
    }

    for (const auto& redir : node->redirections) {
        std::string target = shellQuoteArg(expandString(redir.second, vm));
        switch (redir.first) {
            case 1:
                fullCommand += " > " + target;
                break;
            case 2:
                fullCommand += " >> " + target;
                break;
            case 3:
                fullCommand += " < " + target;
                break;
            case 4:
                fullCommand += " 2> " + target;
                break;
            case 5:
                fullCommand += " 2>&1";
                break;
            case 6:
                fullCommand += " > " + target + " 2>&1";
                break;
            default:
                break;
        }
    }

    return fullCommand;
}

CommandResult executeShellCommandText(const std::string& fullCommand) {
    if (isTraceEnabled()) {
        std::cerr << "[shellvm-trace] SHELL text=\"" << fullCommand << "\"\n";
    }

    FILE* pipe = popen(fullCommand.c_str(), "r");
    if (!pipe) {
        return CommandResult(127, "", "Failed to execute shell command");
    }

    std::string output;
    char buffer[128];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }

    int exitCode = pclose(pipe);
#ifdef _WIN32
    exitCode >>= 8;
#endif

    if (isTraceEnabled()) {
        std::cerr << "[shellvm-trace] SHELL result=" << exitCode
                  << " stdout_size=" << output.size() << "\n";
    }

    return CommandResult(exitCode, output, "");
}

CommandResult executeInlineAst(ASTNodePtr node, VM& vm) {
    auto& runtime = getGlobalRuntime();

    if (!node) {
        return CommandResult(0, "", "");
    }

    switch (node->type) {
        case NodeType::Program: {
            CommandResult result(0, "", "");
            for (const auto& child : node->children) {
                result = executeInlineAst(child, vm);
            }
            return result;
        }
        case NodeType::Command: {
            if (!node->redirections.empty()) {
                return executeShellCommandText(buildShellCommandText(node, vm));
            }
            std::vector<std::string> args;
            args.reserve(node->children.size());
            for (const auto& child : node->children) {
                args.push_back(rawNodeToString(child));
            }
            return runtime.executeCommand(node->value, args, vm);
        }
        case NodeType::Pipeline: {
            return executeShellCommandText(buildShellCommandText(node, vm));
        }
        default:
            return CommandResult(0, "", "");
    }
}

std::string expandParameterExpression(const std::string& expr, VM& vm) {
    if (expr.empty()) {
        return "";
    }

    auto decodeShellLiteral = [](const std::string& text) {
        std::string decoded;
        decoded.reserve(text.size());
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\\' && i + 1 < text.size()) {
                char next = text[i + 1];
                if (next == '"' || next == '\'' || next == '\\') {
                    decoded += next;
                    ++i;
                    continue;
                }
            }
            decoded += text[i];
        }
        return decoded;
    };

    if (expr[0] == '#') {
        return std::to_string(vm.getVariable(expr.substr(1)).asString().size());
    }

    size_t defaultPos = expr.find(":-");
    if (defaultPos != std::string::npos) {
        std::string varName = expr.substr(0, defaultPos);
        std::string current = vm.getVariable(varName).asString();
        return current.empty() ? expr.substr(defaultPos + 2) : current;
    }

    size_t suffixTrimPos = expr.find('%');
    if (suffixTrimPos != std::string::npos) {
        std::string varName = expr.substr(0, suffixTrimPos);
        std::string suffix = decodeShellLiteral(expr.substr(suffixTrimPos + 1));
        std::string current = vm.getVariable(varName).asString();
        if (!suffix.empty() && current.size() >= suffix.size() &&
            current.compare(current.size() - suffix.size(), suffix.size(), suffix) == 0) {
            current.erase(current.size() - suffix.size());
        }
        return current;
    }

    size_t prefixTrimPos = expr.find('#');
    if (prefixTrimPos != std::string::npos) {
        std::string varName = expr.substr(0, prefixTrimPos);
        std::string prefix = decodeShellLiteral(expr.substr(prefixTrimPos + 1));
        std::string current = vm.getVariable(varName).asString();
        if (!prefix.empty() && current.rfind(prefix, 0) == 0) {
            current.erase(0, prefix.size());
        }
        return current;
    }

    size_t assignPos = expr.find('=');
    if (assignPos != std::string::npos) {
        std::string varName = expr.substr(0, assignPos);
        std::string current = vm.getVariable(varName).asString();
        if (current.empty()) {
            std::string fallback = expr.substr(assignPos + 1);
            vm.setVariable(varName, Value(fallback));
            return fallback;
        }
        return current;
    }

    size_t colonPos = expr.find(':');
    if (colonPos != std::string::npos) {
        std::string varName = expr.substr(0, colonPos);
        std::string source = vm.getVariable(varName).asString();
        std::string rest = expr.substr(colonPos + 1);
        size_t secondColon = rest.find(':');
        int offset = 0;
        int length = -1;

        try {
            if (secondColon == std::string::npos) {
                offset = rest.empty() ? 0 : std::stoi(rest);
            } else {
                offset = rest.substr(0, secondColon).empty() ? 0 : std::stoi(rest.substr(0, secondColon));
                length = rest.substr(secondColon + 1).empty() ? -1 : std::stoi(rest.substr(secondColon + 1));
            }
        } catch (...) {
            return "";
        }

        if (offset < 0) {
            offset = static_cast<int>(source.size()) + offset;
        }
        if (offset < 0) {
            offset = 0;
        }
        if (offset >= static_cast<int>(source.size())) {
            return "";
        }
        if (length < 0) {
            return source.substr(static_cast<size_t>(offset));
        }
        return source.substr(static_cast<size_t>(offset), static_cast<size_t>(length));
    }

    return vm.getVariable(expr).asString();
}

std::string expandCommandSubstitution(const std::string& commandText, VM& vm) {
    try {
        Parser parser(commandText);
        auto ast = parser.parse();
        if (parser.hasError() || !ast) {
            return "";
        }
        return trimCommandSubstitutionOutput(executeInlineAst(ast, vm).stdout_output);
    } catch (...) {
        return "";
    }
}

std::string expandString(const std::string& str, VM& vm) {
    std::string result;
    size_t i = 0;
    bool inSingleQuote = false;
    bool inDoubleQuote = false;

    while (i < str.size()) {
        if (str[i] == kLiteralMarkerStart) {
            ++i;
            while (i < str.size() && str[i] != kLiteralMarkerEnd) {
                result += str[i++];
            }
            if (i < str.size() && str[i] == kLiteralMarkerEnd) {
                ++i;
            }
            continue;
        }

        if (!inDoubleQuote && str[i] == '\'') {
            inSingleQuote = !inSingleQuote;
            result += str[i++];
            continue;
        }

        if (!inSingleQuote && str[i] == '"') {
            inDoubleQuote = !inDoubleQuote;
            result += str[i++];
            continue;
        }

        if (inSingleQuote) {
            result += str[i++];
            continue;
        }

        if (str[i] != '$' || i + 1 >= str.size()) {
            result += str[i++];
            continue;
        }

        if (str.compare(i, 3, "$((") == 0) {
            size_t end = str.find("))", i + 3);
            if (end != std::string::npos) {
                std::string expr = str.substr(i + 3, end - (i + 3));
                result += std::to_string(evaluateArithmeticExpr(expr, vm));
                i = end + 2;
                continue;
            }
        }

        if (str.compare(i, 2, "$(") == 0) {
            size_t depth = 1;
            size_t j = i + 2;
            while (j < str.size() && depth > 0) {
                if (str[j] == '(') {
                    ++depth;
                } else if (str[j] == ')') {
                    --depth;
                }
                ++j;
            }
            if (depth == 0) {
                std::string inner = str.substr(i + 2, (j - 1) - (i + 2));
                result += expandCommandSubstitution(inner, vm);
                i = j;
                continue;
            }
        }

        if (str[i + 1] == '{') {
            size_t end = str.find('}', i + 2);
            if (end != std::string::npos) {
                result += expandParameterExpression(str.substr(i + 2, end - (i + 2)), vm);
                i = end + 1;
                continue;
            }
        }

        if (str[i + 1] == '@') {
            Value args = vm.getVariable("__args");
            if (args.isArray()) {
                const auto& array = std::get<Array>(args.data);
                for (size_t idx = 0; idx < array.size(); ++idx) {
                    if (idx > 0) {
                        result += " ";
                    }
                    result += array[idx].asString();
                }
            }
            i += 2;
            continue;
        }

        if (str[i + 1] == '*') {
            Value args = vm.getVariable("__args");
            if (args.isArray()) {
                const auto& array = std::get<Array>(args.data);
                for (size_t idx = 0; idx < array.size(); ++idx) {
                    if (idx > 0) {
                        result += " ";
                    }
                    result += array[idx].asString();
                }
            }
            i += 2;
            continue;
        }

        if (isdigit(static_cast<unsigned char>(str[i + 1])) || str[i + 1] == '?' || str[i + 1] == '!') {
            result += vm.getVariable(std::string(1, str[i + 1])).asString();
            i += 2;
            continue;
        }

        if (isalpha(static_cast<unsigned char>(str[i + 1])) || str[i + 1] == '_') {
            size_t start = i + 1;
            size_t j = start;
            while (j < str.size() &&
                   (isalnum(static_cast<unsigned char>(str[j])) || str[j] == '_')) {
                ++j;
            }
            result += vm.getVariable(str.substr(start, j - start)).asString();
            i = j;
            continue;
        }

        result += str[i++];
    }

    return stripLiteralMarkers(result);
}

std::vector<std::string> expandArgs(const std::vector<std::string>& args, VM& vm) {
    std::vector<std::string> expanded;
    expanded.reserve(args.size());
    for (const auto& arg : args) {
        expanded.push_back(expandString(arg, vm));
    }
    return expanded;
}

} // namespace

Runtime& getGlobalRuntime() {
    static Runtime instance;
    return instance;
}

// ============================================================================
// Runtime构造函数
// ============================================================================

Runtime::Runtime() {
    initBuiltinCommands();
    currentPath_ = getCwd();
}

// ============================================================================
// 内置命令初始化
// ============================================================================

void Runtime::initBuiltinCommands() {
    registerBuiltin("echo", builtinEcho);
    registerBuiltin("printf", builtinPrintf);
    registerBuiltin("cd", builtinCd);
    registerBuiltin("pwd", builtinPwd);
    registerBuiltin("export", builtinExportCmd);
    registerBuiltin("read", builtinRead);
    registerBuiltin("true", builtinTrue);
    registerBuiltin("false", builtinFalse);
    registerBuiltin("exit", builtinExit);
    registerBuiltin("test", builtinTest);
    registerBuiltin("[", builtinTest);
    registerBuiltin("set", builtinSet);
    registerBuiltin("unset", builtinUnset);
    registerBuiltin("shift", builtinShift);
    registerBuiltin("source", builtinSource);
    registerBuiltin(".", builtinSource);
    registerBuiltin("eval", builtinEval);
    registerBuiltin("local", builtinLocal);
    registerBuiltin("return", builtinReturn);
    registerBuiltin("break", builtinBreak);
    registerBuiltin("continue", builtinContinue);
}

// ============================================================================
// 命令执行
// ============================================================================

CommandResult Runtime::executeCommand(
    const std::string& command,
    const std::vector<std::string>& args,
    VM& vm) {

    std::string expandedCommand = expandString(command, vm);
    std::vector<std::string> expandedArgs = expandArgs(args, vm);

    if (isTraceEnabled()) {
        std::cerr << "[shellvm-trace] CMD name=\"" << expandedCommand
                  << "\" args=[" << joinArgsForTrace(expandedArgs) << "]\n";
    }

    // 检查沙箱模式
    if (sandboxMode_ && whitelistEnabled_ && !isWhitelisted(expandedCommand)) {
        return CommandResult(127, "", "Command not allowed in sandbox mode");
    }

    if (expandedCommand == "__shellvm_pipeline__") {
        if (expandedArgs.empty()) {
            return CommandResult(127, "", "Invalid pipeline encoding");
        }

        size_t index = 0;
        int commandCount = 0;
        try {
            commandCount = std::stoi(expandedArgs[index++]);
        } catch (...) {
            return CommandResult(127, "", "Invalid pipeline command count");
        }

        std::string fullCommand;
        for (int cmdIndex = 0; cmdIndex < commandCount; ++cmdIndex) {
            if (index + 1 >= expandedArgs.size()) {
                return CommandResult(127, "", "Invalid pipeline payload");
            }

            std::string cmdName = expandedArgs[index++];
            int argCount = 0;
            try {
                argCount = std::stoi(expandedArgs[index++]);
            } catch (...) {
                return CommandResult(127, "", "Invalid pipeline arg count");
            }

            if (!fullCommand.empty()) {
                fullCommand += " | ";
            }

            fullCommand += shellCommandToken(cmdName);
            for (int argIndex = 0; argIndex < argCount; ++argIndex) {
                if (index >= expandedArgs.size()) {
                    return CommandResult(127, "", "Invalid pipeline arg payload");
                }
                fullCommand += " ";
                fullCommand += shellQuoteArg(expandedArgs[index++]);
            }
        }

        CommandResult result = executeShellCommandText(fullCommand);
        if (isTraceEnabled()) {
            std::cerr << "[shellvm-trace] CMD result=" << result.exitCode
                      << " builtin=0 stdout_size=" << result.stdout_output.size() << "\n";
        }
        return result;
    }

    // 检查是否为内置命令
    if (isBuiltin(expandedCommand)) {
        CommandResult result = executeBuiltin(expandedCommand, expandedArgs, vm);
        if (isTraceEnabled()) {
            std::cerr << "[shellvm-trace] CMD result=" << result.exitCode
                      << " builtin=1 stdout_size=" << result.stdout_output.size() << "\n";
        }
        return result;
    }

    // 外部命令必须走 command + argv[] 模型，不能退化成整条 shell 字符串。
    CommandResult result = executeExternalCommandArgv(expandedCommand, expandedArgs);

    if (isTraceEnabled()) {
        std::cerr << "[shellvm-trace] CMD result=" << result.exitCode
                  << " builtin=0 stdout_size=" << result.stdout_output.size() << "\n";
    }

    return result;
}

CommandResult Runtime::executePipeline(
    const std::vector<std::pair<std::string, std::vector<std::string>>>& commands,
    VM& vm) {

    if (commands.empty()) {
        return CommandResult(0, "", "");
    }

    // 简化实现：顺序执行，将前一个命令的输出作为下一个的输入
    std::string currentOutput;

    for (size_t i = 0; i < commands.size(); ++i) {
        const auto& cmdPair = commands[i];
        std::string command = cmdPair.first;
        std::vector<std::string> args = cmdPair.second;

        if (isTraceEnabled()) {
            std::cerr << "[shellvm-trace] PIPE step=" << i
                      << " name=\"" << command
                      << "\" args=[" << joinArgsForTrace(args) << "]\n";
        }

        // 检查白名单
        if (sandboxMode_ && whitelistEnabled_ && !isWhitelisted(command)) {
            return CommandResult(127, "", "Command not allowed in sandbox mode");
        }

        std::string fullCommand = command;
        for (const auto& arg : args) {
            fullCommand += " " + shellQuoteArg(arg);
        }

        // 如果有前一个命令的输出，将其作为输入
        if (!currentOutput.empty() && i > 0) {
            // 这里简化处理，实际应该使用管道
            // 可以通过临时文件或实际的管道机制实现
        }

        FILE* pipe = popen(fullCommand.c_str(), "r");
        if (!pipe) {
            return CommandResult(127, "", "Failed to execute pipeline command");
        }

        currentOutput.clear();
        char buffer[128];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            currentOutput += buffer;
        }

        int exitCode = pclose(pipe);
#ifdef _WIN32
        exitCode >>= 8;
#endif

        if (isTraceEnabled()) {
            std::cerr << "[shellvm-trace] PIPE result=" << exitCode
                      << " step=" << i
                      << " stdout_size=" << currentOutput.size() << "\n";
        }

        if (exitCode != 0 && i == commands.size() - 1) {
            return CommandResult(exitCode, currentOutput, "");
        }
    }

    return CommandResult(0, currentOutput, "");
}

int Runtime::executeAsync(
    const std::string& command,
    const std::vector<std::string>& args,
    VM& vm) {

    std::string expandedCommand = expandString(command, vm);
    std::vector<std::string> expandedArgs = expandArgs(args, vm);

    // 检查白名单
    if (sandboxMode_ && whitelistEnabled_ && !isWhitelisted(expandedCommand)) {
        return -1;
    }

#ifdef _WIN32
    (void)expandedCommand;
    (void)expandedArgs;
    return -1;
#else
    pid_t pid = fork();
    if (pid == 0) {
        // 子进程
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(expandedCommand.c_str()));
        for (const auto& arg : expandedArgs) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execvp(expandedCommand.c_str(), argv.data());
        exit(127);
    } else if (pid > 0) {
        // 父进程
        return static_cast<int>(pid);
    } else {
        return -1;
    }
#endif
}

// ============================================================================
// 内置命令
// ============================================================================

void Runtime::registerBuiltin(const std::string& name, BuiltinCommand cmd) {
    builtinCommands_[name] = cmd;
}

bool Runtime::isBuiltin(const std::string& name) const {
    return builtinCommands_.find(name) != builtinCommands_.end();
}

CommandResult Runtime::executeBuiltin(
    const std::string& name,
    const std::vector<std::string>& args,
    VM& vm) {

    auto it = builtinCommands_.find(name);
    if (it == builtinCommands_.end()) {
        return CommandResult(127, "", "Unknown builtin command");
    }

    return it->second(args, vm);
}

// ============================================================================
// 内置命令实现
// ============================================================================

CommandResult Runtime::builtinEcho(const std::vector<std::string>& args, VM& vm) {
    std::ostringstream oss;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) oss << " ";
        oss << args[i];
    }
    oss << "\n";

    std::cout << oss.str();
    return CommandResult(0, oss.str(), "");
}

CommandResult Runtime::builtinPrintf(const std::vector<std::string>& args, VM& vm) {
    if (args.empty()) {
        return CommandResult(1, "", "printf: missing format string");
    }

    std::string format = expandVariables(args[0], vm);
    std::ostringstream oss;

    size_t argIndex = 1;
    for (size_t i = 0; i < format.length(); ++i) {
        if (format[i] == '%' && i + 1 < format.length()) {
            char spec = format[++i];
            switch (spec) {
                case 's':
                    if (argIndex < args.size()) {
                        oss << args[argIndex++];
                    }
                    break;
                case 'd':
                    if (argIndex < args.size()) {
                        oss << std::stoi(args[argIndex++]);
                    }
                    break;
                case 'f':
                    if (argIndex < args.size()) {
                        oss << std::stod(args[argIndex++]);
                    }
                    break;
                case '%':
                    oss << '%';
                    break;
                default:
                    oss << '%' << spec;
                    break;
            }
        } else if (format[i] == '\\') {
            if (i + 1 < format.length()) {
                char next = format[++i];
                switch (next) {
                    case 'n': oss << '\n'; break;
                    case 't': oss << '\t'; break;
                    case 'r': oss << '\r'; break;
                    case '\\': oss << '\\'; break;
                    default: oss << '\\' << next; break;
                }
            }
        } else {
            oss << format[i];
        }
    }

    std::cout << oss.str();
    return CommandResult(0, oss.str(), "");
}

CommandResult Runtime::builtinCd(const std::vector<std::string>& args, VM& vm) {
    std::string targetPath;
    if (args.empty()) {
        targetPath = vm.getEnv("HOME");
        if (targetPath.empty()) {
#ifdef _WIN32
            targetPath = vm.getEnv("USERPROFILE");
#else
            targetPath = "/";
#endif
        }
    } else {
        targetPath = args[0];
    }

    // 处理相对路径
    if (targetPath[0] != '/' && targetPath[0] != '\\' &&
        !(targetPath.length() > 1 && targetPath[1] == ':')) {
        targetPath = vm.getCwd() + "/" + targetPath;
    }

    // 检查路径权限（沙箱模式） - 使用全局runtime实例
    auto& runtime = getGlobalRuntime();
    if (runtime.sandboxMode_ && !runtime.isPathAllowed(targetPath)) {
        return CommandResult(1, "", "cd: Permission denied");
    }

    vm.setCwd(targetPath);
    return CommandResult(0, "", "");
}

CommandResult Runtime::builtinPwd(const std::vector<std::string>& args, VM& vm) {
    std::string cwd = vm.getCwd();
    std::cout << cwd << "\n";
    return CommandResult(0, cwd + "\n", "");
}

CommandResult Runtime::builtinExportCmd(const std::vector<std::string>& args, VM& vm) {
    if (args.empty()) {
        // 显示所有环境变量
        // TODO: 实现环境变量列表
        return CommandResult(0, "", "");
    }

    for (const auto& arg : args) {
        size_t pos = arg.find('=');
        if (pos != std::string::npos) {
            std::string name = arg.substr(0, pos);
            std::string value = arg.substr(pos + 1);
            vm.setEnv(name, value);
            vm.setVariable(name, Value(value));
        }
    }

    return CommandResult(0, "", "");
}

CommandResult Runtime::builtinRead(const std::vector<std::string>& args, VM& vm) {
    if (args.empty()) {
        return CommandResult(1, "", "read: missing variable name");
    }

    std::string line;
    std::getline(std::cin, line);

    for (const auto& name : args) {
        vm.setVariable(name, Value(line));
    }

    return CommandResult(0, "", "");
}

CommandResult Runtime::builtinTrue(const std::vector<std::string>& args, VM& vm) {
    return CommandResult(0, "", "");
}

CommandResult Runtime::builtinFalse(const std::vector<std::string>& args, VM& vm) {
    return CommandResult(1, "", "");
}

CommandResult Runtime::builtinExit(const std::vector<std::string>& args, VM& vm) {
    int exitCode = 0;
    if (!args.empty()) {
        exitCode = std::stoi(args[0]);
    }

    vm.stop();
    return CommandResult(exitCode, "", "");
}

CommandResult Runtime::builtinTest(const std::vector<std::string>& args, VM& vm) {
    auto parsedArgs = parseTestExpression(args);

    // 简化实现：只支持基本的文件测试和字符串比较
    bool result = false;

    if (parsedArgs.size() >= 2) {
        std::string op = parsedArgs[0];
        std::string arg1 = parsedArgs[1];
        std::string arg2 = parsedArgs.size() >= 3 ? parsedArgs[2] : "";

        // 文件测试 - 使用全局runtime实例
        auto& runtime = getGlobalRuntime();
        if (op == "-e") {
            result = runtime.fileExists(arg1);
        } else if (op == "-f") {
            // TODO: 检查是否为普通文件
            result = runtime.fileExists(arg1);
        } else if (op == "-d") {
            // TODO: 检查是否为目录
            result = false;
        } else if (op == "-r") {
            // TODO: 检查是否可读
            result = runtime.fileExists(arg1);
        } else if (op == "-w") {
            // TODO: 检查是否可写
            result = runtime.fileExists(arg1);
        } else if (op == "-x") {
            // TODO: 检查是否可执行
            result = runtime.fileExists(arg1);
        } else if (op == "-z") {
            result = arg1.empty();
        } else if (op == "-n") {
            result = !arg1.empty();
        } else if (op == "=" && parsedArgs.size() >= 3) {
            result = arg1 == arg2;
        } else if (op == "!=" && parsedArgs.size() >= 3) {
            result = arg1 != arg2;
        } else if (op == "-eq" && parsedArgs.size() >= 3) {
            result = std::stol(arg1) == std::stol(arg2);
        } else if (op == "-ne" && parsedArgs.size() >= 3) {
            result = std::stol(arg1) != std::stol(arg2);
        } else if (op == "-lt" && parsedArgs.size() >= 3) {
            result = std::stol(arg1) < std::stol(arg2);
        } else if (op == "-le" && parsedArgs.size() >= 3) {
            result = std::stol(arg1) <= std::stol(arg2);
        } else if (op == "-gt" && parsedArgs.size() >= 3) {
            result = std::stol(arg1) > std::stol(arg2);
        } else if (op == "-ge" && parsedArgs.size() >= 3) {
            result = std::stol(arg1) >= std::stol(arg2);
        }
    }

    return CommandResult(result ? 0 : 1, "", "");
}

CommandResult Runtime::builtinSet(const std::vector<std::string>& args, VM& vm) {
    // TODO: 实现set命令
    return CommandResult(0, "", "");
}

CommandResult Runtime::builtinUnset(const std::vector<std::string>& args, VM& vm) {
    for (const auto& name : args) {
        vm.deleteVariable(name);
    }
    return CommandResult(0, "", "");
}

CommandResult Runtime::builtinShift(const std::vector<std::string>& args, VM& vm) {
    int count = 1;
    if (!args.empty()) {
        try {
            count = std::max(0, std::stoi(args[0]));
        } catch (...) {
            count = 1;
        }
    }

    Value argValue = vm.getVariable("__args");
    Array shifted;
    if (argValue.isArray()) {
        const auto& current = std::get<Array>(argValue.data);
        size_t start = static_cast<size_t>(std::min<int>(count, static_cast<int>(current.size())));
        shifted.assign(current.begin() + start, current.end());
    }

    vm.setVariable("__args", Value(shifted));
    for (int i = 1; i <= 9; ++i) {
        vm.deleteVariable(std::to_string(i));
    }
    for (size_t i = 0; i < shifted.size() && i < 9; ++i) {
        vm.setVariable(std::to_string(i + 1), shifted[i]);
    }
    return CommandResult(0, "", "");
}

CommandResult Runtime::builtinSource(const std::vector<std::string>& args, VM& vm) {
    if (args.empty()) {
        return CommandResult(1, "", "source: missing file argument");
    }

    std::string filename = args[0];
    std::string content;

    auto& runtime = getGlobalRuntime();
    if (!runtime.readFile(filename, content)) {
        return CommandResult(1, "", "source: cannot read file");
    }

    // 编译并执行文件内容
    try {
        auto bytecode = Compiler::compile(content);
        vm.execute(bytecode);
        return CommandResult(0, "", "");
    } catch (const VMException& e) {
        return CommandResult(1, "", "source: " + std::string(e.what()));
    }
}

CommandResult Runtime::builtinEval(const std::vector<std::string>& args, VM& vm) {
    if (args.empty()) {
        return CommandResult(0, "", "");
    }

    std::string command;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) command += " ";
        command += args[i];
    }

    // eval 需要重新进行多轮展开，直到结果稳定或达到上限。
    std::string expandedCommand = command;
    for (int i = 0; i < 4; ++i) {
        std::string next = expandString(expandedCommand, vm);
        if (next == expandedCommand) {
            break;
        }
        expandedCommand = next;
    }

    try {
        auto bytecode = Compiler::compile(expandedCommand);
        vm.execute(bytecode);
        return CommandResult(0, "", "");
    } catch (const VMException& e) {
        return CommandResult(1, "", "eval: " + std::string(e.what()));
    }
}

CommandResult Runtime::builtinLocal(const std::vector<std::string>& args, VM& vm) {
    // TODO: 实现local命令
    return CommandResult(0, "", "");
}

CommandResult Runtime::builtinReturn(const std::vector<std::string>& args, VM& vm) {
    int exitCode = 0;
    if (!args.empty()) {
        exitCode = std::stoi(args[0]);
    }
    return CommandResult(exitCode, "", "");
}

CommandResult Runtime::builtinBreak(const std::vector<std::string>& args, VM& vm) {
    // TODO: 实现break（需要循环上下文）
    return CommandResult(0, "", "");
}

CommandResult Runtime::builtinContinue(const std::vector<std::string>& args, VM& vm) {
    // TODO: 实现continue（需要循环上下文）
    return CommandResult(0, "", "");
}

// ============================================================================
// 辅助函数
// ============================================================================

std::string Runtime::expandVariables(const std::string& str, VM& vm) {
    return expandString(str, vm);
}

std::vector<std::string> Runtime::parseTestExpression(const std::vector<std::string>& args) {
    std::vector<std::string> result;

    for (const auto& arg : args) {
        result.push_back(arg);
    }

    return result;
}

// ============================================================================
// 命令白名单
// ============================================================================

void Runtime::addToWhitelist(const std::string& command) {
    whitelist_.insert(command);
}

void Runtime::removeFromWhitelist(const std::string& command) {
    whitelist_.erase(command);
}

void Runtime::clearWhitelist() {
    whitelist_.clear();
}

bool Runtime::isWhitelisted(const std::string& command) const {
    return whitelist_.find(command) != whitelist_.end();
}

// ============================================================================
// 沙箱模式
// ============================================================================

bool Runtime::isPathAllowed(const std::string& path) const {
    if (allowedPath_.empty()) {
        return true;  // 没有路径限制
    }

    // 检查路径是否在允许的范围内
    // 简化实现：只检查前缀
    return path.find(allowedPath_) == 0;
}

// ============================================================================
// 文件操作
// ============================================================================

bool Runtime::fileExists(const std::string& path) const {
    std::ifstream file(resolvePathFromCwd(path), std::ios::binary);
    return file.good();
}

bool Runtime::readFile(const std::string& path, std::string& content) const {
    std::ifstream file(resolvePathFromCwd(path), std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    content = ss.str();

    return true;
}

bool Runtime::writeFile(const std::string& path, const std::string& content) const {
    std::ofstream file(resolvePathFromCwd(path), std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    file.write(content.c_str(), content.length());
    return true;
}

bool Runtime::deleteFile(const std::string& path) {
    std::string resolved = resolvePathFromCwd(path);
#ifdef _WIN32
    return DeleteFileA(resolved.c_str()) != 0;
#else
    return unlink(resolved.c_str()) == 0;
#endif
}

// ============================================================================
// 环境操作
// ============================================================================

std::string Runtime::getEnv(const std::string& name) const {
    const char* value = std::getenv(name.c_str());
    return value ? std::string(value) : "";
}

void Runtime::setEnv(const std::string& name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str());
#else
    setenv(name.c_str(), value.c_str(), 1);
#endif
}

std::string Runtime::getCwd() const {
#ifdef _WIN32
    char buffer[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, buffer);
    return std::string(buffer);
#else
    char buffer[PATH_MAX];
    getcwd(buffer, PATH_MAX);
    return std::string(buffer);
#endif
}

bool Runtime::setCwd(const std::string& path) {
#ifdef _WIN32
    return SetCurrentDirectoryA(path.c_str()) != 0;
#else
    return chdir(path.c_str()) == 0;
#endif
}

} // namespace shellvm
