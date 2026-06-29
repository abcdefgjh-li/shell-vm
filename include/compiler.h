/**
 * @file compiler.h
 * @brief Shell脚本编译器 - 完整版支持黑水脚本
 * @license MIT License
 *
 * 完整支持的语法:
 * - 函数定义和调用
 * - 命令替换 $() 和 ``
 * - 子shell ()
 * - 后台执行 &
 * - 管道 |
 * - 重定向 > >> < 2> 2>&1
 * - 条件判断 if-elif-else-fi
 * - 循环 for/while/until
 * - 算术运算 $(( )) 和 $(())
 * - 字符串操作 ${var} ${#var} ${var:offset:len}
 * - 特殊变量 $@ $* $# $$ $! $? $0-$9
 * - case语句
 * - 数组
 */

#ifndef COMPILER_H
#define COMPILER_H

#include "shell_vm.h"
#include <regex>

namespace shellvm {

// ============================================================================
// Token类型
// ============================================================================

enum class TokenType {
    EndOfFile,
    Newline,
    Semicolon,

    // 字面量
    Integer,
    Float,
    String,
    Identifier,

    // 关键字
    KW_If, KW_Then, KW_Else, KW_Elif, KW_Fi,
    KW_For, KW_While, KW_Until, KW_Do, KW_Done, KW_In,
    KW_Function, KW_Return, KW_Exit, KW_Export, KW_Local,
    KW_True, KW_False, KW_Case, KW_Esac, KW_Break, KW_Continue,
    KW_Undef, KW_Declare, KW_Readonly,

    // 操作符
    OP_Assign, OP_Equals, OP_NotEquals,
    OP_Less, OP_LessEqual, OP_Greater, OP_GreaterEqual,
    OP_Plus, OP_Minus, OP_Multiply, OP_Divide, OP_Modulo,
    OP_And, OP_Or, OP_Not,
    OP_BitAnd, OP_BitOr, OP_BitNot, OP_BitXor, OP_ShiftLeft, OP_ShiftRight,

    // 特殊符号
    OP_Pipe, OP_PipeAnd,
    OP_RedirectOut, OP_RedirectAppend, OP_RedirectIn, OP_RedirectInAnd,
    OP_RedirectErr, OP_RedirectErrOut, OP_RedirectOutErr,
    OP_Background,

    // 括号
    LParen, RParen, LBrace, RBrace, LBracket, RBracket, LDollarParen, RDollarParen,

    // 其他
    Comment, Dollar, Backtick, DollarLBrace, RBraceClose,
    Unknown
};

struct Token {
    TokenType type = TokenType::Unknown;
    std::string value;
    int line = 0;
    int column = 0;
    Token(TokenType t = TokenType::Unknown, const std::string& v = "", int l = 0, int c = 0)
        : type(t), value(v), line(l), column(c) {}
};

// ============================================================================
// 词法分析器
// ============================================================================

class Lexer {
public:
    explicit Lexer(const std::string& source);
    Token nextToken();
    Token peekToken();
    void skipWhitespace();
    void skipComment();
    bool atEnd() const { return pos_ >= source_.length(); }
    int currentLine() const { return line_; }
    int currentColumn() const { return column_; }

private:
    char current() const;
    char peek(size_t offset = 1) const;
    char advance();
    Token readNumber();
    Token readString(char quote);
    Token readIdentifier();
    Token readOperator();
    Token readDollarExpr();
    Token readPath();        // 读取路径如 /dev/null
    Token readPlusWord();    // 读取 +%s / +"%Y..." 这类参数
    Token readOption();      // 读取选项如 -0 --help -e
    TokenType lookupKeyword(const std::string& id) const;

    std::string source_;
    size_t pos_ = 0;
    int line_ = 1;
    int column_ = 1;
    Token peeked_;
    bool hasPeek_ = false;
};

// ============================================================================
// AST节点类型
// ============================================================================

enum class NodeType {
    Program,
    Command,
    CommandSubstitution,    // $() 或 ``
    Pipeline,
    Assignment,
    Variable,
    ArrayVariable,
    Literal,
    BinaryOp,
    UnaryOp,
    IfStatement,
    WhileStatement,
    UntilStatement,
    ForStatement,
    ForInStatement,
    CaseStatement,
    FunctionDef,
    FunctionCall,
    Block,
    Subshell,               // ()
    Redirection,
    Background,             // &
    ReturnStatement,
    ExitStatement,
    BreakStatement,
    ContinueStatement,
    ArithmeticExpansion,    // $(())
    ParameterExpansion,      // ${...}
    TestExpression,
};

struct ASTNode;
using ASTNodePtr = std::shared_ptr<ASTNode>;

struct ASTNode {
    NodeType type;
    int line = 0;
    std::string value;
    Value literalValue;
    TokenType op = TokenType::Unknown;
    ASTNodePtr left;
    ASTNodePtr right;
    std::vector<ASTNodePtr> children;
    std::vector<ASTNodePtr> args;        // 函数参数/命令参数
    std::string redirectFile;
    int redirectType = 0;
    int redirectFd = -1;                  // 重定向的文件描述符
    std::vector<std::pair<int, std::string>> redirections; // 多重重定向
    ASTNodePtr condition;                 // 条件表达式
    ASTNodePtr body;                      // 循环体或then块
    ASTNodePtr elseBody;                  // else块
    std::vector<std::pair<ASTNodePtr, ASTNodePtr>> elifBranches; // elif分支

    ASTNode(NodeType t, int l = 0) : type(t), line(l) {}
};

// ============================================================================
// 解析器
// ============================================================================

class Parser {
public:
    explicit Parser(const std::string& source);
    ASTNodePtr parse();
    bool hasError() const { return hasError_; }
    const std::string& errorMessage() const { return errorMessage_; }

private:
    ASTNodePtr parseProgram();
    ASTNodePtr parseStatement();
    ASTNodePtr parsePipeline();
    ASTNodePtr parseCommand();
    ASTNodePtr parseSimpleCommand();
    ASTNodePtr parseAssignment();
    ASTNodePtr parseExpression();
    ASTNodePtr parseIfStatement();
    ASTNodePtr parseWhileStatement();
    ASTNodePtr parseUntilStatement();
    ASTNodePtr parseForStatement();
    ASTNodePtr parseCaseStatement();
    ASTNodePtr parseFunctionDef();
    ASTNodePtr parseFunctionDefKeyword();
    ASTNodePtr parseBlock();
    ASTNodePtr parseSubshell();
    ASTNodePtr parseTestExpression();
    ASTNodePtr parseRedirection(ASTNodePtr cmd);
    ASTNodePtr parseWord();
    ASTNodePtr parseDollarExpression();
    ASTNodePtr parseArithmeticExpansion();
    ASTNodePtr parseParameterExpansion(const std::string& varName);

    Token currentToken() const { return current_; }
    Token advanceToken();
    bool match(TokenType type);
    bool check(TokenType type) const;
    void expect(TokenType type, const std::string& message);
    void error(const std::string& message);
    bool isCommandTerminator() const;

    Lexer lexer_;
    Token current_;
    bool hasError_ = false;
    std::string errorMessage_;
};

// ============================================================================
// 编译器
// ============================================================================

class Compiler {
public:
    static Bytecode compile(const std::string& source);
    static Bytecode compileAST(ASTNodePtr ast);
    static std::string disassemble(const Bytecode& bytecode);
    static std::string bytecodeToCppArray(const Bytecode& bytecode, const std::string& symbolName);

    // 公开的patchJump以便外部使用
    static void patchJump(InstructionList& instructions, size_t offset, size_t target);

private:
    static void compileNode(ASTNodePtr node, InstructionList& instructions);
    static void compileProgram(ASTNodePtr node, InstructionList& instructions);
    static void compileCommand(ASTNodePtr node, InstructionList& instructions);
    static void compileCommandSubstitution(ASTNodePtr node, InstructionList& instructions);
    static void compilePipeline(ASTNodePtr node, InstructionList& instructions);
    static void compileAssignment(ASTNodePtr node, InstructionList& instructions);
    static void compileBinaryOp(ASTNodePtr node, InstructionList& instructions);
    static void compileUnaryOp(ASTNodePtr node, InstructionList& instructions);
    static void compileIfStatement(ASTNodePtr node, InstructionList& instructions);
    static void compileWhileStatement(ASTNodePtr node, InstructionList& instructions);
    static void compileUntilStatement(ASTNodePtr node, InstructionList& instructions);
    static void compileForStatement(ASTNodePtr node, InstructionList& instructions);
    static void compileCaseStatement(ASTNodePtr node, InstructionList& instructions);
    static void compileFunctionDef(ASTNodePtr node, InstructionList& instructions);
    static void compileFunctionCall(ASTNodePtr node, InstructionList& instructions);
    static void compileBlock(ASTNodePtr node, InstructionList& instructions);
    static void compileSubshell(ASTNodePtr node, InstructionList& instructions);
    static void compileReturn(ASTNodePtr node, InstructionList& instructions);
    static void compileExit(ASTNodePtr node, InstructionList& instructions);
    static void compileBreak(ASTNodePtr node, InstructionList& instructions);
    static void compileContinue(ASTNodePtr node, InstructionList& instructions);
    static void compileArithmeticExpansion(ASTNodePtr node, InstructionList& instructions);
    static void compileParameterExpansion(ASTNodePtr node, InstructionList& instructions);
    static void compileTestExpression(ASTNodePtr node, InstructionList& instructions);
    static void compileBackground(ASTNodePtr node, InstructionList& instructions);
    static void compileVariable(ASTNodePtr node, InstructionList& instructions);
    static void compileLiteral(ASTNodePtr node, InstructionList& instructions);
};

} // namespace shellvm

#endif // COMPILER_H
