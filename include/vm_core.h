/**
 * @file vm_core.h
 * @brief VM核心实现
 * @license MIT License
 */

#ifndef VM_CORE_H
#define VM_CORE_H

#include "shell_vm.h"
#include <stack>
#include <chrono>

namespace shellvm {

// ============================================================================
// 调用帧
// ============================================================================

struct CallFrame {
    size_t returnAddress = 0;           // 返回地址
    size_t stackBase = 0;               // 栈基址
    std::string functionName;           // 函数名

    CallFrame(size_t ret, size_t base, const std::string& name = "")
        : returnAddress(ret), stackBase(base), functionName(name) {}
};

// ============================================================================
// VM核心类
// ============================================================================

class VM {
public:
    explicit VM(const VMConfig& config = VMConfig{});
    ~VM() = default;

    // 禁止拷贝
    VM(const VM&) = delete;
    VM& operator=(const VM&) = delete;

    // 允许移动
    VM(VM&&) noexcept = default;
    VM& operator=(VM&&) noexcept = default;

    // ========================================================================
    // 字节码执行
    // ========================================================================

    /**
     * @brief 执行字节码
     * @param bytecode 字节码数据
     * @return 退出码
     */
    int execute(const Bytecode& bytecode);

    /**
     * @brief 执行指令列表
     * @param instructions 指令列表
     * @return 退出码
     */
    int execute(const InstructionList& instructions);

    /**
     * @brief 停止执行
     */
    void stop() { running_ = false; }

    /**
     * @brief 是否正在运行
     */
    bool isRunning() const { return running_; }

    // ========================================================================
    // 栈操作
    // ========================================================================

    void push(const Value& value);
    Value pop();
    Value& peek(size_t offset = 0);
    const Value& peek(size_t offset = 0) const;
    size_t stackSize() const { return stack_.size(); }
    void clearStack() { stack_.clear(); }

    // ========================================================================
    // 变量操作
    // ========================================================================

    void setVariable(const std::string& name, const Value& value);
    Value getVariable(const std::string& name) const;
    bool hasVariable(const std::string& name) const;
    void deleteVariable(const std::string& name);
    void clearVariables() { variables_.clear(); }

    // ========================================================================
    // 环境操作
    // ========================================================================

    std::string getEnv(const std::string& name) const;
    void setEnv(const std::string& name, const std::string& value);
    std::string getCwd() const;
    void setCwd(const std::string& path);

    // ========================================================================
    // 调用帧操作
    // ========================================================================

    void pushFrame(size_t returnAddress, const std::string& name = "");
    CallFrame popFrame();
    size_t frameCount() const { return frames_.size(); }

    // ========================================================================
    // 指令读取
    // ========================================================================

    uint8_t readByte();
    int16_t readInt16();
    int32_t readInt32();
    int64_t readInt64();
    double readFloat();
    std::string readString();

    size_t instructionPointer() const { return ip_; }
    void jump(size_t address) { ip_ = address; }

    // ========================================================================
    // 配置访问
    // ========================================================================

    const VMConfig& config() const { return config_; }
    VMConfig& config() { return config_; }

    // ========================================================================
    // 统计信息
    // ========================================================================

    struct Stats {
        uint64_t instructionsExecuted = 0;
        uint64_t commandsExecuted = 0;
        std::chrono::microseconds executionTime{0};
    };

    const Stats& stats() const { return stats_; }
    void resetStats() { stats_ = Stats{}; }

private:
    // 执行单条指令
    bool executeInstruction();

    // 检查超时
    bool checkTimeout() const;

    // 检查内存限制
    bool checkMemoryLimit() const;

private:
    VMConfig config_;
    Bytecode bytecode_;
    size_t ip_ = 0;                        // 指令指针
    std::vector<Value> stack_;             // 数据栈
    std::vector<CallFrame> frames_;        // 调用帧栈
    std::unordered_map<std::string, Value> variables_; // 变量表
    bool running_ = false;
    int exitCode_ = 0;
    Stats stats_;
    std::chrono::steady_clock::time_point startTime_;

    // 命令输出缓冲
    std::string lastOutput_;
    int lastExitCode_ = 0;
};

} // namespace shellvm

#endif // VM_CORE_H