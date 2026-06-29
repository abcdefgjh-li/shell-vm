/**
 * @file vm_core.cpp
 * @brief VM核心实现
 * @license MIT License
 */

#include "vm_core.h"
#include "runtime.h"
#include <iostream>
#include <cstring>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <unistd.h>
#include <limits.h>
#endif

namespace shellvm {

// ============================================================================
// VM构造函数
// ============================================================================

VM::VM(const VMConfig& config) : config_(config) {
    stack_.reserve(config.maxStackSize);
    frames_.reserve(config.maxCallFrames);

    if (config_.workingDirectory.empty()) {
        config_.workingDirectory = getCwd();
    }
}

// ============================================================================
// 字节码执行
// ============================================================================

int VM::execute(const Bytecode& bytecode) {
    bytecode_ = bytecode;
    ip_ = 0;
    running_ = true;
    exitCode_ = 0;
    startTime_ = std::chrono::steady_clock::now();
    stats_ = Stats{};

    try {
        while (running_ && ip_ < bytecode_.size()) {
            if (!executeInstruction()) {
                break;
            }
            stats_.instructionsExecuted++;

            // 检查超时
            if (config_.timeout > 0 && checkTimeout()) {
                throw VMException(VMError::Timeout, "Execution timeout");
            }

            // 检查内存限制
            if (!checkMemoryLimit()) {
                throw VMException(VMError::OutOfMemory, "Memory limit exceeded");
            }
        }
    } catch (const VMException& e) {
        std::cerr << "VM Error: " << e.what() << std::endl;
        exitCode_ = -1;
        running_ = false;
    }

    return exitCode_;
}

int VM::execute(const InstructionList& instructions) {
    Bytecode bytecode;
    for (const auto& inst : instructions) {
        auto bytes = inst.toBytes();
        bytecode.insert(bytecode.end(), bytes.begin(), bytes.end());
    }
    return execute(bytecode);
}

// ============================================================================
// 栈操作
// ============================================================================

void VM::push(const Value& value) {
    if (stack_.size() >= config_.maxStackSize) {
        throw VMException(VMError::StackOverflow, "Stack overflow");
    }
    stack_.push_back(value);
}

Value VM::pop() {
    if (stack_.empty()) {
        throw VMException(VMError::StackUnderflow, "Stack underflow");
    }
    Value value = stack_.back();
    stack_.pop_back();
    return value;
}

Value& VM::peek(size_t offset) {
    if (offset >= stack_.size()) {
        throw VMException(VMError::StackUnderflow, "Stack underflow on peek");
    }
    return stack_[stack_.size() - 1 - offset];
}

const Value& VM::peek(size_t offset) const {
    if (offset >= stack_.size()) {
        throw VMException(VMError::StackUnderflow, "Stack underflow on peek");
    }
    return stack_[stack_.size() - 1 - offset];
}

// ============================================================================
// 变量操作
// ============================================================================

void VM::setVariable(const std::string& name, const Value& value) {
    variables_[name] = value;
}

Value VM::getVariable(const std::string& name) const {
    auto it = variables_.find(name);
    if (it == variables_.end()) {
        // 尝试从环境变量获取
        std::string envValue = getEnv(name);
        if (!envValue.empty()) {
            return Value(envValue);
        }
        return Value(); // Null
    }
    return it->second;
}

bool VM::hasVariable(const std::string& name) const {
    return variables_.find(name) != variables_.end();
}

void VM::deleteVariable(const std::string& name) {
    variables_.erase(name);
}

// ============================================================================
// 环境操作
// ============================================================================

std::string VM::getEnv(const std::string& name) const {
    const char* value = std::getenv(name.c_str());
    return value ? std::string(value) : "";
}

void VM::setEnv(const std::string& name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str());
#else
    setenv(name.c_str(), value.c_str(), 1);
#endif
}

std::string VM::getCwd() const {
#ifdef _WIN32
    char buffer[1024];
    GetCurrentDirectoryA(1024, buffer);
    return std::string(buffer);
#else
    char buffer[PATH_MAX];
    getcwd(buffer, PATH_MAX);
    return std::string(buffer);
#endif
}

void VM::setCwd(const std::string& path) {
#ifdef _WIN32
    SetCurrentDirectoryA(path.c_str());
#else
    chdir(path.c_str());
#endif
}

// ============================================================================
// 调用帧操作
// ============================================================================

void VM::pushFrame(size_t returnAddress, const std::string& name) {
    if (frames_.size() >= config_.maxCallFrames) {
        throw VMException(VMError::RuntimeError, "Call stack overflow");
    }
    frames_.emplace_back(returnAddress, stack_.size(), name);
}

CallFrame VM::popFrame() {
    if (frames_.empty()) {
        throw VMException(VMError::RuntimeError, "Call stack underflow");
    }
    CallFrame frame = frames_.back();
    frames_.pop_back();
    return frame;
}

// ============================================================================
// 指令读取
// ============================================================================

uint8_t VM::readByte() {
    if (ip_ >= bytecode_.size()) {
        throw VMException(VMError::InvalidOpcode, "Unexpected end of bytecode");
    }
    return bytecode_[ip_++];
}

int16_t VM::readInt16() {
    if (ip_ + 2 > bytecode_.size()) {
        throw VMException(VMError::InvalidOperand, "Invalid operand size");
    }
    int16_t value = static_cast<int16_t>(
        bytecode_[ip_] | (bytecode_[ip_ + 1] << 8));
    ip_ += 2;
    return value;
}

int32_t VM::readInt32() {
    if (ip_ + 4 > bytecode_.size()) {
        throw VMException(VMError::InvalidOperand, "Invalid operand size");
    }
    int32_t value = static_cast<int32_t>(
        bytecode_[ip_] |
        (bytecode_[ip_ + 1] << 8) |
        (bytecode_[ip_ + 2] << 16) |
        (bytecode_[ip_ + 3] << 24));
    ip_ += 4;
    return value;
}

int64_t VM::readInt64() {
    if (ip_ + 8 > bytecode_.size()) {
        throw VMException(VMError::InvalidOperand, "Invalid operand size");
    }
    int64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<int64_t>(bytecode_[ip_ + i]) << (i * 8);
    }
    ip_ += 8;
    return value;
}

double VM::readFloat() {
    int64_t bits = readInt64();
    double value;
    std::memcpy(&value, &bits, sizeof(double));
    return value;
}

std::string VM::readString() {
    int32_t length = readInt32();
    if (length < 0 || ip_ + length > bytecode_.size()) {
        throw VMException(VMError::InvalidOperand, "Invalid string length");
    }
    std::string value(reinterpret_cast<const char*>(&bytecode_[ip_]), length);
    ip_ += length;
    return value;
}

// ============================================================================
// 指令执行
// ============================================================================

bool VM::executeInstruction() {
    OpCode opcode = static_cast<OpCode>(readByte());

    switch (opcode) {
        // ====================================================================
        // 栈操作
        // ====================================================================
        case OpCode::NOP:
            break;

        case OpCode::PUSH_NULL:
            push(Value());
            break;

        case OpCode::PUSH_INT: {
            int64_t value = readInt64();
            push(Value(value));
            break;
        }

        case OpCode::PUSH_FLOAT: {
            double value = readFloat();
            push(Value(value));
            break;
        }

        case OpCode::PUSH_STR: {
            std::string value = readString();
            push(Value(value));
            break;
        }

        case OpCode::PUSH_BOOL: {
            uint8_t value = readByte();
            push(Value(static_cast<bool>(value)));
            break;
        }

        case OpCode::POP:
            pop();
            break;

        case OpCode::DUP:
            push(peek());
            break;

        case OpCode::SWAP: {
            Value a = pop();
            Value b = pop();
            push(a);
            push(b);
            break;
        }

        // ====================================================================
        // 算术运算
        // ====================================================================
        case OpCode::ADD: {
            Value b = pop();
            Value a = pop();

            if (a.isString() || b.isString()) {
                push(Value(a.asString() + b.asString()));
            } else if (a.isFloat() || b.isFloat()) {
                push(Value(a.asFloat() + b.asFloat()));
            } else {
                push(Value(a.asInteger() + b.asInteger()));
            }
            break;
        }

        case OpCode::SUB: {
            Value b = pop();
            Value a = pop();
            if (a.isFloat() || b.isFloat()) {
                push(Value(a.asFloat() - b.asFloat()));
            } else {
                push(Value(a.asInteger() - b.asInteger()));
            }
            break;
        }

        case OpCode::MUL: {
            Value b = pop();
            Value a = pop();
            if (a.isFloat() || b.isFloat()) {
                push(Value(a.asFloat() * b.asFloat()));
            } else {
                push(Value(a.asInteger() * b.asInteger()));
            }
            break;
        }

        case OpCode::DIV: {
            Value b = pop();
            Value a = pop();
            if ((b.isInteger() && b.asInteger() == 0) ||
                (b.isFloat() && b.asFloat() == 0.0)) {
                throw VMException(VMError::DivisionByZero, "Division by zero");
            }
            if (a.isFloat() || b.isFloat()) {
                push(Value(a.asFloat() / b.asFloat()));
            } else {
                push(Value(a.asInteger() / b.asInteger()));
            }
            break;
        }

        case OpCode::MOD: {
            Value b = pop();
            Value a = pop();
            if ((b.isInteger() && b.asInteger() == 0) ||
                (b.isFloat() && b.asFloat() == 0.0)) {
                throw VMException(VMError::DivisionByZero, "Division by zero");
            }
            push(Value(a.asInteger() % b.asInteger()));
            break;
        }

        case OpCode::NEG: {
            Value a = pop();
            if (a.isFloat()) {
                push(Value(-a.asFloat()));
            } else {
                push(Value(-a.asInteger()));
            }
            break;
        }

        // ====================================================================
        // 比较运算
        // ====================================================================
        case OpCode::EQ: {
            Value b = pop();
            Value a = pop();
            if (a.type != b.type) {
                push(Value(false));
            } else {
                push(Value(a.data == b.data));
            }
            break;
        }

        case OpCode::NE: {
            Value b = pop();
            Value a = pop();
            push(Value(!(a.type == b.type && a.data == b.data)));
            break;
        }

        case OpCode::LT: {
            Value b = pop();
            Value a = pop();
            if (a.isString() && b.isString()) {
                push(Value(std::get<std::string>(a.data) < std::get<std::string>(b.data)));
            } else {
                push(Value(a.asFloat() < b.asFloat()));
            }
            break;
        }

        case OpCode::LE: {
            Value b = pop();
            Value a = pop();
            if (a.isString() && b.isString()) {
                push(Value(std::get<std::string>(a.data) <= std::get<std::string>(b.data)));
            } else {
                push(Value(a.asFloat() <= b.asFloat()));
            }
            break;
        }

        case OpCode::GT: {
            Value b = pop();
            Value a = pop();
            if (a.isString() && b.isString()) {
                push(Value(std::get<std::string>(a.data) > std::get<std::string>(b.data)));
            } else {
                push(Value(a.asFloat() > b.asFloat()));
            }
            break;
        }

        case OpCode::GE: {
            Value b = pop();
            Value a = pop();
            if (a.isString() && b.isString()) {
                push(Value(std::get<std::string>(a.data) >= std::get<std::string>(b.data)));
            } else {
                push(Value(a.asFloat() >= b.asFloat()));
            }
            break;
        }

        // ====================================================================
        // 逻辑运算
        // ====================================================================
        case OpCode::AND: {
            Value b = pop();
            Value a = pop();
            push(Value(a.asBoolean() && b.asBoolean()));
            break;
        }

        case OpCode::OR: {
            Value b = pop();
            Value a = pop();
            push(Value(a.asBoolean() || b.asBoolean()));
            break;
        }

        case OpCode::NOT: {
            Value a = pop();
            push(Value(!a.asBoolean()));
            break;
        }

        // ====================================================================
        // 变量操作
        // ====================================================================
        case OpCode::VAR_SET: {
            std::string name = readString();
            Value value = pop();
            setVariable(name, value);
            break;
        }

        case OpCode::VAR_GET: {
            std::string name = readString();
            push(getVariable(name));
            break;
        }

        case OpCode::VAR_DEL: {
            std::string name = readString();
            deleteVariable(name);
            break;
        }

        // ====================================================================
        // Shell命令
        // ====================================================================
        case OpCode::CMD: {
            std::string command = readString();
            uint8_t argCount = readByte();

            std::vector<std::string> args;
            args.reserve(argCount);
            for (int i = 0; i < argCount; ++i) {
                args.push_back(readString());
            }

            auto& runtime = getGlobalRuntime();
            CommandResult result = runtime.executeCommand(command, args, *this);

            lastOutput_ = result.stdout_output;
            lastExitCode_ = result.exitCode;
            stats_.commandsExecuted++;

            push(Value(static_cast<int64_t>(result.exitCode)));
            break;
        }

        case OpCode::CMD_ASYNC: {
            std::string command = readString();
            uint8_t argCount = readByte();

            std::vector<std::string> args;
            args.reserve(argCount);
            for (int i = 0; i < argCount; ++i) {
                args.push_back(readString());
            }

            auto& runtime = getGlobalRuntime();
            int pid = runtime.executeAsync(command, args, *this);
            push(Value(static_cast<int64_t>(pid)));
            break;
        }

        case OpCode::PIPE: {
            // TODO: 实现管道操作
            break;
        }

        case OpCode::REDIRECT_OUT: {
            Value filename = pop();
            // TODO: 实现输出重定向
            break;
        }

        case OpCode::REDIRECT_IN: {
            Value filename = pop();
            // TODO: 实现输入重定向
            break;
        }

        case OpCode::REDIRECT_APPEND: {
            Value filename = pop();
            // TODO: 实现追加重定向
            break;
        }

        case OpCode::GET_EXIT_CODE:
            push(Value(static_cast<int64_t>(lastExitCode_)));
            break;

        case OpCode::GET_OUTPUT:
            push(Value(lastOutput_));
            break;

        // ====================================================================
        // 控制流
        // ====================================================================
        case OpCode::JUMP: {
            int32_t offset = readInt32();
            ip_ = static_cast<size_t>(offset);
            break;
        }

        case OpCode::JUMP_IF: {
            int32_t offset = readInt32();
            Value condition = pop();
            if (condition.asBoolean()) {
                ip_ = static_cast<size_t>(offset);
            }
            break;
        }

        case OpCode::JUMP_IF_NOT: {
            int32_t offset = readInt32();
            Value condition = pop();
            if (!condition.asBoolean()) {
                ip_ = static_cast<size_t>(offset);
            }
            break;
        }

        case OpCode::CALL: {
            int32_t target = readInt32();
            uint8_t argCount = readByte();
            std::vector<Value> args;
            args.reserve(argCount);
            for (int i = 0; i < argCount; ++i) {
                args.push_back(pop());
            }
            std::reverse(args.begin(), args.end());

            pushFrame(ip_);

            Array argArray;
            argArray.reserve(args.size());
            for (size_t i = 0; i < args.size(); ++i) {
                argArray.push_back(args[i]);
                if (i < 9) {
                    setVariable(std::to_string(i + 1), args[i]);
                }
            }
            setVariable("__args", Value(argArray));
            setVariable("#", Value(static_cast<int64_t>(args.size())));
            ip_ = static_cast<size_t>(target);
            break;
        }

        case OpCode::RET: {
            CallFrame frame = popFrame();
            while (stack_.size() > frame.stackBase) {
                stack_.pop_back();
            }
            ip_ = frame.returnAddress;
            break;
        }

        case OpCode::HALT: {
            running_ = false;
            break;
        }

        // ====================================================================
        // 字符串操作
        // ====================================================================
        case OpCode::STR_CAT: {
            Value b = pop();
            Value a = pop();
            push(Value(a.asString() + b.asString()));
            break;
        }

        case OpCode::STR_LEN: {
            Value a = pop();
            push(Value(static_cast<int64_t>(a.asString().length())));
            break;
        }

        case OpCode::STR_SUB: {
            int32_t length = readInt32();
            int32_t start = readInt32();
            Value str = pop();
            push(Value(str.asString().substr(start, length)));
            break;
        }

        case OpCode::STR_SPLIT: {
            std::string delimiter = readString();
            Value str = pop();
            // TODO: 实现字符串分割
            break;
        }

        case OpCode::STR_EXPAND: {
            Value str = pop();
            push(Value(Runtime::expandVariables(str.asString(), *this)));
            break;
        }

        // ====================================================================
        // 数组操作
        // ====================================================================
        case OpCode::ARR_NEW:
            push(Value(Array{}));
            break;

        case OpCode::ARR_PUSH: {
            Value elem = pop();
            Value arr = pop();
            if (arr.isArray()) {
                auto& array = std::get<Array>(arr.data);
                array.push_back(elem);
                push(arr);
            }
            break;
        }

        case OpCode::ARR_GET: {
            Value index = pop();
            Value arr = pop();
            if (arr.isArray()) {
                auto& array = std::get<Array>(arr.data);
                size_t idx = static_cast<size_t>(index.asInteger());
                if (idx < array.size()) {
                    push(array[idx]);
                } else {
                    push(Value());
                }
            }
            break;
        }

        case OpCode::ARR_LEN: {
            Value arr = pop();
            if (arr.isArray()) {
                push(Value(static_cast<int64_t>(std::get<Array>(arr.data).size())));
            } else {
                push(Value(static_cast<int64_t>(0)));
            }
            break;
        }

        case OpCode::ARR_JOIN: {
            std::string delimiter = readString();
            Value arr = pop();
            if (arr.isArray()) {
                auto& array = std::get<Array>(arr.data);
                std::string result;
                for (size_t i = 0; i < array.size(); ++i) {
                    if (i > 0) result += delimiter;
                    result += array[i].asString();
                }
                push(Value(result));
            }
            break;
        }

        // ====================================================================
        // 类型操作
        // ====================================================================
        case OpCode::TYPEOF: {
            Value a = pop();
            push(Value(static_cast<int64_t>(a.type)));
            break;
        }

        case OpCode::CAST_INT: {
            Value a = pop();
            push(Value(a.asInteger()));
            break;
        }

        case OpCode::CAST_FLOAT: {
            Value a = pop();
            push(Value(a.asFloat()));
            break;
        }

        case OpCode::CAST_STR: {
            Value a = pop();
            push(Value(a.asString()));
            break;
        }

        case OpCode::CAST_BOOL: {
            Value a = pop();
            push(Value(a.asBoolean()));
            break;
        }

        // ====================================================================
        // 环境操作
        // ====================================================================
        case OpCode::ENV_GET: {
            std::string name = readString();
            push(Value(getEnv(name)));
            break;
        }

        case OpCode::ENV_SET: {
            std::string value = readString();
            std::string name = readString();
            setEnv(name, value);
            break;
        }

        case OpCode::CWD_GET:
            push(Value(getCwd()));
            break;

        case OpCode::CWD_SET: {
            Value path = pop();
            setCwd(path.asString());
            break;
        }

        // ====================================================================
        // 文件操作
        // ====================================================================
        case OpCode::FILE_READ: {
            Value path = pop();
            auto& runtime = getGlobalRuntime();
            std::string content;
            if (runtime.readFile(path.asString(), content)) {
                push(Value(content));
            } else {
                push(Value());
            }
            break;
        }

        case OpCode::FILE_WRITE: {
            Value content = pop();
            Value path = pop();
            auto& runtime = getGlobalRuntime();
            push(Value(runtime.writeFile(path.asString(), content.asString())));
            break;
        }

        case OpCode::FILE_EXISTS: {
            Value path = pop();
            auto& runtime = getGlobalRuntime();
            push(Value(runtime.fileExists(path.asString())));
            break;
        }

        case OpCode::FILE_DELETE: {
            Value path = pop();
            auto& runtime = getGlobalRuntime();
            push(Value(runtime.deleteFile(path.asString())));
            break;
        }

        default:
            throw VMException(VMError::InvalidOpcode, "Unknown opcode");
    }

    return running_;
}

// ============================================================================
// 辅助函数
// ============================================================================

bool VM::checkTimeout() const {
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTime_);
    return elapsed.count() > config_.timeout;
}

bool VM::checkMemoryLimit() const {
    // 简单估算：栈大小 + 变量表大小
    size_t estimatedMemory = stack_.size() * sizeof(Value) +
                             frames_.size() * sizeof(CallFrame);

    // 粗略估算字符串和变量占用的内存
    for (const auto& var : variables_) {
        estimatedMemory += var.first.size() + sizeof(var.second);
        if (var.second.isString()) {
            estimatedMemory += var.second.asString().size();
        }
    }

    return estimatedMemory < config_.maxMemory;
}

} // namespace shellvm
