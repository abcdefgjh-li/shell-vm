/**
 * @file shell_vm.h
 * @brief Shell-VM 主头文件
 * @license MIT License - 允许闭源二改商用
 *
 * Android Shell脚本虚拟化系统
 */

#ifndef SHELL_VM_H
#define SHELL_VM_H

#include <vector>
#include <string>
#include <unordered_map>
#include <memory>
#include <variant>
#include <functional>
#include <stdexcept>

namespace shellvm {

// ============================================================================
// 版本信息
// ============================================================================

#define SHELL_VM_VERSION_MAJOR 1
#define SHELL_VM_VERSION_MINOR 0
#define SHELL_VM_VERSION_PATCH 0

// ============================================================================
// 错误处理
// ============================================================================

enum class VMError {
    None = 0,
    StackOverflow,
    StackUnderflow,
    InvalidOpcode,
    InvalidOperand,
    DivisionByZero,
    UnknownCommand,
    FileNotFound,
    PermissionDenied,
    OutOfMemory,
    Timeout,
    RuntimeError
};

class VMException : public std::runtime_error {
public:
    explicit VMException(VMError err, const std::string& msg)
        : std::runtime_error(msg), error_(err) {}

    VMError error() const noexcept { return error_; }

private:
    VMError error_;
};

// ============================================================================
// 数据类型
// ============================================================================

enum class ValueType : uint8_t {
    Null = 0,
    Integer,
    Float,
    String,
    Boolean,
    Array,
    Map,
    CommandResult
};

struct Value;
using ValuePtr = std::shared_ptr<Value>;
using Array = std::vector<Value>;
using Map = std::unordered_map<std::string, Value>;

struct Value {
    ValueType type = ValueType::Null;
    std::variant<
        std::monostate,
        int64_t,
        double,
        std::string,
        bool,
        Array,
        Map
    > data;

    Value() = default;

    explicit Value(int64_t v) : type(ValueType::Integer), data(v) {}
    explicit Value(double v) : type(ValueType::Float), data(v) {}
    explicit Value(const std::string& v) : type(ValueType::String), data(v) {}
    Value(bool v) : type(ValueType::Boolean), data(v) {}  // bool不需要explicit
    explicit Value(const Array& v) : type(ValueType::Array), data(v) {}
    explicit Value(const Map& v) : type(ValueType::Map), data(v) {}

    bool isNull() const { return type == ValueType::Null; }
    bool isInteger() const { return type == ValueType::Integer; }
    bool isFloat() const { return type == ValueType::Float; }
    bool isString() const { return type == ValueType::String; }
    bool isBoolean() const { return type == ValueType::Boolean; }
    bool isArray() const { return type == ValueType::Array; }
    bool isMap() const { return type == ValueType::Map; }

    // 添加operator==以支持std::unordered_map
    bool operator==(const Value& other) const {
        if (type != other.type) return false;
        return data == other.data;
    }

    bool operator!=(const Value& other) const {
        return !(*this == other);
    }

    int64_t asInteger() const {
        if (type == ValueType::Integer) return std::get<int64_t>(data);
        if (type == ValueType::Float) return static_cast<int64_t>(std::get<double>(data));
        throw VMException(VMError::InvalidOperand, "Cannot convert to integer");
    }

    double asFloat() const {
        if (type == ValueType::Float) return std::get<double>(data);
        if (type == ValueType::Integer) return static_cast<double>(std::get<int64_t>(data));
        throw VMException(VMError::InvalidOperand, "Cannot convert to float");
    }

    std::string asString() const {
        switch (type) {
            case ValueType::String:
                return std::get<std::string>(data);
            case ValueType::Integer:
                return std::to_string(std::get<int64_t>(data));
            case ValueType::Float:
                return std::to_string(std::get<double>(data));
            case ValueType::Boolean:
                return std::get<bool>(data) ? "true" : "false";
            case ValueType::Null:
                return "null";
            default:
                return "[object]";
        }
    }

    bool asBoolean() const {
        if (type == ValueType::Boolean) return std::get<bool>(data);
        if (type == ValueType::Integer) return std::get<int64_t>(data) != 0;
        if (type == ValueType::String) return !std::get<std::string>(data).empty();
        return !isNull();
    }
};

// ============================================================================
// 操作码定义
// ============================================================================

enum class OpCode : uint8_t {
    // 栈操作 (0x00-0x0F)
    NOP = 0x00,            // 无操作
    PUSH_NULL = 0x01,      // 压入null
    PUSH_INT = 0x02,       // 压入整数
    PUSH_FLOAT = 0x03,     // 压入浮点数
    PUSH_STR = 0x04,       // 压入字符串
    PUSH_BOOL = 0x05,      // 压入布尔值
    POP = 0x06,            // 弹出栈顶
    DUP = 0x07,            // 复制栈顶
    SWAP = 0x08,           // 交换栈顶两个元素

    // 算术运算 (0x10-0x1F)
    ADD = 0x10,            // 加法
    SUB = 0x11,            // 减法
    MUL = 0x12,            // 乘法
    DIV = 0x13,            // 除法
    MOD = 0x14,            // 取模
    NEG = 0x15,            // 取负

    // 比较运算 (0x20-0x2F)
    EQ = 0x20,             // 等于
    NE = 0x21,             // 不等于
    LT = 0x22,             // 小于
    LE = 0x23,             // 小于等于
    GT = 0x24,             // 大于
    GE = 0x25,             // 大于等于

    // 逻辑运算 (0x30-0x3F)
    AND = 0x30,            // 逻辑与
    OR = 0x31,             // 逻辑或
    NOT = 0x32,            // 逻辑非

    // 变量操作 (0x40-0x4F)
    VAR_SET = 0x40,        // 设置变量
    VAR_GET = 0x41,        // 获取变量
    VAR_DEL = 0x42,        // 删除变量

    // Shell命令 (0x50-0x5F)
    CMD = 0x50,            // 执行命令
    CMD_ASYNC = 0x51,      // 异步执行命令
    PIPE = 0x52,           // 管道操作
    REDIRECT_OUT = 0x53,   // 输出重定向
    REDIRECT_IN = 0x54,    // 输入重定向
    REDIRECT_APPEND = 0x55, // 追加重定向
    GET_EXIT_CODE = 0x56,  // 获取退出码
    GET_OUTPUT = 0x57,     // 获取输出

    // 控制流 (0x60-0x6F)
    JUMP = 0x60,           // 无条件跳转
    JUMP_IF = 0x61,        // 条件跳转(true)
    JUMP_IF_NOT = 0x62,    // 条件跳转(false)
    CALL = 0x63,           // 函数调用
    RET = 0x64,            // 函数返回
    HALT = 0x65,           // 停止执行

    // 字符串操作 (0x70-0x7F)
    STR_CAT = 0x70,        // 字符串拼接
    STR_LEN = 0x71,        // 字符串长度
    STR_SUB = 0x72,        // 子字符串
    STR_SPLIT = 0x73,      // 字符串分割
    STR_EXPAND = 0x74,     // 运行时字符串展开

    // 数组操作 (0x80-0x8F)
    ARR_NEW = 0x80,        // 创建数组
    ARR_PUSH = 0x81,       // 数组添加元素
    ARR_GET = 0x82,        // 数组获取元素
    ARR_LEN = 0x83,        // 数组长度
    ARR_JOIN = 0x84,       // 数组连接为字符串

    // 类型操作 (0x90-0x9F)
    TYPEOF = 0x90,         // 获取类型
    CAST_INT = 0x91,       // 转换为整数
    CAST_FLOAT = 0x92,     // 转换为浮点数
    CAST_STR = 0x93,       // 转换为字符串
    CAST_BOOL = 0x94,      // 转换为布尔值

    // 环境操作 (0xA0-0xAF)
    ENV_GET = 0xA0,        // 获取环境变量
    ENV_SET = 0xA1,        // 设置环境变量
    CWD_GET = 0xA2,        // 获取当前目录
    CWD_SET = 0xA3,        // 设置当前目录

    // 文件操作 (0xB0-0xBF)
    FILE_READ = 0xB0,      // 读取文件
    FILE_WRITE = 0xB1,     // 写入文件
    FILE_EXISTS = 0xB2,    // 文件是否存在
    FILE_DELETE = 0xB3,    // 删除文件

    // 扩展指令 (0xC0-0xFF)
    EXTENDED = 0xC0        // 扩展指令前缀
};

// ============================================================================
// 字节码定义
// ============================================================================

struct Instruction {
    OpCode opcode;
    std::vector<uint8_t> operands;

    Instruction(OpCode op = OpCode::NOP) : opcode(op) {}

    Instruction& addByte(uint8_t b) {
        operands.push_back(b);
        return *this;
    }

    Instruction& addInt16(int16_t v) {
        operands.push_back(static_cast<uint8_t>(v & 0xFF));
        operands.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        return *this;
    }

    Instruction& addInt32(int32_t v) {
        operands.push_back(static_cast<uint8_t>(v & 0xFF));
        operands.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        operands.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        operands.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
        return *this;
    }

    Instruction& addInt64(int64_t v) {
        for (int i = 0; i < 8; ++i) {
            operands.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
        }
        return *this;
    }

    Instruction& addFloat(double v) {
        uint64_t bits;
        memcpy(&bits, &v, sizeof(double));
        return addInt64(static_cast<int64_t>(bits));
    }

    Instruction& addString(const std::string& s) {
        addInt32(static_cast<int32_t>(s.length()));
        for (char c : s) {
            operands.push_back(static_cast<uint8_t>(c));
        }
        return *this;
    }

    std::vector<uint8_t> toBytes() const {
        std::vector<uint8_t> bytes;
        bytes.push_back(static_cast<uint8_t>(opcode));
        bytes.insert(bytes.end(), operands.begin(), operands.end());
        return bytes;
    }
};

using Bytecode = std::vector<uint8_t>;
using InstructionList = std::vector<Instruction>;

// ============================================================================
// 运行时配置
// ============================================================================

struct VMConfig {
    size_t maxStackSize = 4096;          // 最大栈大小
    size_t maxCallFrames = 256;           // 最大调用帧数
    size_t maxMemory = 1024 * 1024 * 64;  // 最大内存使用 (64MB)
    uint32_t timeout = 30000;             // 超时时间 (毫秒)
    bool enableSandbox = true;            // 启用沙箱模式
    bool enableFileAccess = true;         // 允许文件访问
    bool enableNetworkAccess = false;     // 允许网络访问
    std::string workingDirectory;         // 工作目录
    std::vector<std::string> allowedCommands; // 允许的命令白名单
};

// ============================================================================
// 前向声明
// ============================================================================

class Compiler;
class VM;
class Runtime;

} // namespace shellvm

#endif // SHELL_VM_H
