/**
 * @file runtime.h
 * @brief Shell运行时库
 * @license MIT License
 */

#ifndef RUNTIME_H
#define RUNTIME_H

#include "shell_vm.h"
#include <functional>
#include <unordered_set>

namespace shellvm {

// ============================================================================
// 命令结果
// ============================================================================

struct CommandResult {
    int exitCode = 0;
    std::string stdout_output;
    std::string stderr_output;
    bool success = false;

    CommandResult() = default;
    CommandResult(int code, const std::string& out, const std::string& err)
        : exitCode(code), stdout_output(out), stderr_output(err), success(code == 0) {}
};

// ============================================================================
// 内置命令类型
// ============================================================================

using BuiltinCommand = std::function<CommandResult(
    const std::vector<std::string>& args, VM& vm)>;

// ============================================================================
// 运行时类
// ============================================================================

class Runtime {
public:
    Runtime();
    ~Runtime() = default;

    // ========================================================================
    // 命令执行
    // ========================================================================

    /**
     * @brief 执行Shell命令
     * @param command 命令名称
     * @param args 参数列表
     * @param vm VM实例
     * @return 命令结果
     */
    CommandResult executeCommand(
        const std::string& command,
        const std::vector<std::string>& args,
        VM& vm);

    /**
     * @brief 执行管道命令
     * @param commands 命令列表（每个命令包含名称和参数）
     * @param vm VM实例
     * @return 最后一个命令的结果
     */
    CommandResult executePipeline(
        const std::vector<std::pair<std::string, std::vector<std::string>>>& commands,
        VM& vm);

    /**
     * @brief 异步执行命令
     * @param command 命令名称
     * @param args 参数列表
     * @param vm VM实例
     * @return 进程ID
     */
    int executeAsync(
        const std::string& command,
        const std::vector<std::string>& args,
        VM& vm);

    // ========================================================================
    // 内置命令注册
    // ========================================================================

    void registerBuiltin(const std::string& name, BuiltinCommand cmd);
    bool isBuiltin(const std::string& name) const;
    CommandResult executeBuiltin(
        const std::string& name,
        const std::vector<std::string>& args,
        VM& vm);

    // ========================================================================
    // 命令白名单
    // ========================================================================

    void addToWhitelist(const std::string& command);
    void removeFromWhitelist(const std::string& command);
    void clearWhitelist();
    bool isWhitelisted(const std::string& command) const;
    void setWhitelistEnabled(bool enabled) { whitelistEnabled_ = enabled; }

    // ========================================================================
    // 沙箱模式
    // ========================================================================

    void setSandboxMode(bool enabled) { sandboxMode_ = enabled; }
    bool isSandboxMode() const { return sandboxMode_; }

    void setAllowedPath(const std::string& path) { allowedPath_ = path; }
    bool isPathAllowed(const std::string& path) const;

    // ========================================================================
    // 文件操作
    // ========================================================================

    bool fileExists(const std::string& path) const;
    bool readFile(const std::string& path, std::string& content) const;
    bool writeFile(const std::string& path, const std::string& content) const;
    bool deleteFile(const std::string& path);

    // ========================================================================
    // 环境操作
    // ========================================================================

    std::string getEnv(const std::string& name) const;
    void setEnv(const std::string& name, const std::string& value);
    std::string getCwd() const;
    bool setCwd(const std::string& path);

private:
    // 初始化内置命令
    void initBuiltinCommands();

    // 内置命令实现
    static CommandResult builtinEcho(const std::vector<std::string>& args, VM& vm);
    static CommandResult builtinPrintf(const std::vector<std::string>& args, VM& vm);
    static CommandResult builtinCd(const std::vector<std::string>& args, VM& vm);
    static CommandResult builtinPwd(const std::vector<std::string>& args, VM& vm);
    static CommandResult builtinExport(const std::vector<std::string>& args, VM& vm);
    static CommandResult builtinRead(const std::vector<std::string>& args, VM& vm);
    static CommandResult builtinTrue(const std::vector<std::string>& args, VM& vm);
    static CommandResult builtinFalse(const std::vector<std::string>& args, VM& vm);
    static CommandResult builtinExit(const std::vector<std::string>& args, VM& vm);
    static CommandResult builtinTest(const std::vector<std::string>& args, VM& vm);
    static CommandResult builtinExportCmd(const std::vector<std::string>& args, VM& vm);
    static CommandResult builtinSet(const std::vector<std::string>& args, VM& vm);
    static CommandResult builtinUnset(const std::vector<std::string>& args, VM& vm);
    static CommandResult builtinShift(const std::vector<std::string>& args, VM& vm);
    static CommandResult builtinSource(const std::vector<std::string>& args, VM& vm);
    static CommandResult builtinEval(const std::vector<std::string>& args, VM& vm);
    static CommandResult builtinLocal(const std::vector<std::string>& args, VM& vm);
    static CommandResult builtinReturn(const std::vector<std::string>& args, VM& vm);
    static CommandResult builtinBreak(const std::vector<std::string>& args, VM& vm);
    static CommandResult builtinContinue(const std::vector<std::string>& args, VM& vm);

    // 辅助函数
    static std::string expandVariables(const std::string& str, VM& vm);
    static std::vector<std::string> parseTestExpression(const std::vector<std::string>& args);

private:
    std::unordered_map<std::string, BuiltinCommand> builtinCommands_;
    std::unordered_set<std::string> whitelist_;
    bool whitelistEnabled_ = false;
    bool sandboxMode_ = true;
    std::string allowedPath_;
    std::string currentPath_;
};

// ============================================================================
// 全局运行时实例
// ============================================================================

Runtime& getGlobalRuntime();

} // namespace shellvm

#endif // RUNTIME_H