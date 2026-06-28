# Shell-VM 架构文档

## 系统概述

Shell-VM是一个将Android Shell脚本编译为VM字节码并用C++执行的系统，采用MIT协议，允许闭源二改商用。

## 核心组件

### 1. 虚拟机 (VM)

**文件**: [vm_core.h](../include/vm_core.h), [vm_core.cpp](../src/vm_core.cpp)

**职责**:
- 字节码解释执行
- 栈管理
- 变量管理
- 调用帧管理
- 指令指针管理

**关键特性**:
- 基于栈的架构
- 支持超时控制
- 内存限制检查
- 沙箱模式支持

### 2. 编译器 (Compiler)

**文件**: [compiler.h](../include/compiler.h), [compiler.cpp](../src/compiler.cpp)

**组成**:
- **词法分析器 (Lexer)**: 将源代码转换为Token流
- **语法分析器 (Parser)**: 将Token流转换为AST
- **字节码生成器**: 将AST转换为字节码

**支持的语法**:
- 命令执行: `echo Hello`
- 变量赋值: `VAR=value`
- 变量引用: `$VAR`, `${VAR}`
- 条件语句: `if-then-else-fi`
- 循环语句: `while-do-done`, `for-in-do-done`
- 函数定义: `function name() { ... }`
- 管道: `cmd1 | cmd2`
- 重定向: `>, >>, <`

### 3. 运行时 (Runtime)

**文件**: [runtime.h](../include/runtime.h), [runtime.cpp](../src/runtime.cpp)

**职责**:
- 命令执行（内置命令和外部命令）
- 文件操作
- 环境变量管理
- 安全控制（沙箱模式、白名单）

**内置命令**:
- `echo`: 输出文本
- `printf`: 格式化输出
- `cd`: 改变目录
- `pwd`: 显示当前目录
- `export`: 设置环境变量
- `read`: 读取输入
- `true/false`: 返回成功/失败
- `exit`: 退出脚本
- `test/[`: 条件测试
- `source/.`: 执行脚本文件
- `eval`: 执行字符串命令
- `set/unset`: 变量管理
- `shift`: 参数位移
- `local`: 本地变量
- `return`: 函数返回
- `break/continue`: 循环控制

## 字节码格式

### 指令结构

每条指令由操作码（1字节）和操作数（0-N字节）组成。

```
[操作码][操作数1][操作数2]...[操作数N]
```

### 操作码分类

| 分类 | 范围 | 说明 |
|------|------|------|
| 栈操作 | 0x00-0x0F | PUSH, POP, DUP, SWAP |
| 算术运算 | 0x10-0x1F | ADD, SUB, MUL, DIV, MOD |
| 比较运算 | 0x20-0x2F | EQ, NE, LT, LE, GT, GE |
| 逻辑运算 | 0x30-0x3F | AND, OR, NOT |
| 变量操作 | 0x40-0x4F | VAR_SET, VAR_GET, VAR_DEL |
| Shell命令 | 0x50-0x5F | CMD, PIPE, REDIRECT |
| 控制流 | 0x60-0x6F | JUMP, CALL, RET, HALT |
| 字符串操作 | 0x70-0x7F | STR_CAT, STR_LEN, STR_SUB |
| 数组操作 | 0x80-0x8F | ARR_NEW, ARR_PUSH, ARR_GET |
| 类型操作 | 0x90-0x9F | TYPEOF, CAST_* |
| 环境操作 | 0xA0-0xAF | ENV_GET, ENV_SET, CWD_* |
| 文件操作 | 0xB0-0xBF | FILE_READ, FILE_WRITE |

### 示例字节码

**脚本**: `echo Hello World`

**字节码**:
```
PUSH_STR "echo"
PUSH_STR "Hello"
PUSH_STR "World"
PUSH_INT 2
CMD
HALT
```

## 执行流程

```
Shell脚本源码
    ↓
词法分析器 (Lexer)
    ↓
Token流
    ↓
语法分析器 (Parser)
    ↓
抽象语法树 (AST)
    ↓
编译器 (Compiler)
    ↓
字节码 (Bytecode)
    ↓
虚拟机 (VM) 执行
```

## 数据类型

支持的数据类型:
- `Null`: 空值
- `Integer`: 64位整数
- `Float`: 64位浮点数
- `String`: 字符串
- `Boolean`: 布尔值
- `Array`: 动态数组
- `Map`: 键值映射
- `CommandResult`: 命令执行结果

## 安全特性

### 沙箱模式

启用后可限制:
- 允许执行的命令（白名单）
- 可访问的路径
- 文件读写权限
- 网络访问权限

### 白名单机制

通过 `addToWhitelist()` 添加允许执行的命令:
```cpp
runtime.addToWhitelist("ls");
runtime.addToWhitelist("cat");
runtime.setWhitelistEnabled(true);
```

## 性能优化

### 设计考虑

1. **栈式架构**: 比寄存器式更适合脚本语言
2. **指令紧凑**: 1字节操作码减少内存占用
3. **直接执行**: 无中间AST遍历开销
4. **静态检查**: 编译时类型检查减少运行时错误

### 可扩展优化

未来可添加:
- 字节码优化（常量折叠、死代码消除）
- JIT编译
- 多线程执行
- 指令缓存

## 与其他VM对比

| 特性 | Shell-VM | Lua VM | Q3VM |
|------|----------|---------|------|
| 协议 | MIT | MIT | GPLv2 |
| 类型 | 栈式 | 寄存器式 | 寄存器式 |
| 目标 | Shell脚本 | Lua | C代码 |
| 沙箱 | 支持 | 支持 | 支持 |
| 依赖 | 无 | 无 | 无 |

## 许可证

MIT License - 可自由用于商业项目，允许闭源二改商用。

## 参考资料

基于以下开源项目的设计思想:
- uvm32 (MIT)
- LightVM (MIT)
- Crafting Interpreters教程
- Lua VM架构