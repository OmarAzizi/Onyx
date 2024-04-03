#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "chunk.h"
#include "common.h"
#include "compiler.h"
#include "object.h"
#include "scanner.h"
#include "value.h"

#ifdef DEBUG_PRINT_CODE
#include "debug.h"
#endif

typedef struct {
    Token current;
    Token previous;
    bool hadError;
    bool panicMode;
} Parser;

/* Precedence levels as an Enum */
typedef enum {
    PREC_NONE,
    PREC_ASSIGNMENT,  // =
    PREC_OR,          // or
    PREC_AND,         // and
    PREC_EQUALITY,    // == !=
    PREC_COMPARISON,  // < > <= >=
    PREC_TERM,        // + -
    PREC_FACTOR,      // * /
    PREC_UNARY,       // ! -
    PREC_CALL,        // . ()
    PREC_PRIMARY
} Precedence;

typedef void (*ParseFn)(); /* This is a typedef for a function that takes no parameters and returns nothing */

/* This struct represents a single row in the parser table */
typedef struct {
    ParseFn prefix;
    ParseFn infix;
    Precedence precedence;
} ParseRule;

Parser parser;
Chunk* compilingChunk;

static Chunk* currentChunk() { 
    return compilingChunk; 
}

static void errorAt(Token* token, const char* message) {
    if (parser.panicMode) return;
    parser.panicMode = true;
    fprintf(stderr, "[line %d] Error", token->line);

    if (token->type == TOKEN_EOF) fprintf(stderr, " at end");
    else if (token->type == TOKEN_ERROR) { /* Nothing */ }
    else fprintf(stderr, " at '%.*s'", token->length, token->start);

    fprintf(stderr, ": %s\n", message);
    parser.hadError = true;
}

static void error(const char* message) {
    errorAt(&parser.previous, message);
}

static void errorAtCurrent(const char* message) { 
    errorAt(&parser.current, message); 
}

static void advance() {
    parser.previous = parser.current;

    for (;;) {
        parser.current = scanToken();
        if (parser.current.type != TOKEN_ERROR) break;

        errorAtCurrent(parser.current.start);
    }
}

/*
    It’s similar to 'advance' in that it reads the next token. 
    But it also validates that the token has an expected type
*/
static void consume(TokenType type, const char* message) {
    if (parser.current.type == type) {
        advance();
        return;
    }
    errorAtCurrent(message);
}

static bool check(TokenType type) {
    return parser.current.type == type;
}

static bool match(TokenType type) {
    if (!check(type)) return false;
    advance();
    return true;
}

static void emitByte(uint8_t byte) {
    writeChunk(currentChunk(), byte, parser.previous.line);
}

static void emitBytes(uint8_t byte1, uint8_t byte2) {
    emitByte(byte1);
    emitByte(byte2);
}

static void emitReturn() { 
    emitByte(OP_RETURN); 
}

static uint8_t makeConstant(Value value) {
    int constant = addConstant(currentChunk(), value);
    if (constant > UINT8_MAX) {
        error("Too many constants in one chunk.");
        return 0;
    }
    return (uint8_t)constant;
}

static void emitConstant(Value value) {
    emitBytes(OP_CONSTANT, makeConstant(value));
}

static void endCompiler() { 
    emitReturn(); 
#ifdef DEBUG_PRINT_CODE
    if(!parser.hadError) disassembleChunk(currentChunk(), "code");
#endif
}

static void expression();
static void statement();
static void decleration();
static ParseRule* getRule(TokenType type);
static void parsePrecedence(Precedence precedence);
static uint8_t identifierConstant(Token* name);
static uint8_t parseVariable(const char* errorMessage);
static void defineVariable(uint8_t global);

static void binary() {
    TokenType operatorType = parser.previous.type;
    ParseRule* rule = getRule(operatorType);
    parsePrecedence((Precedence)(rule->precedence + 1));

    switch (operatorType) {
        case TOKEN_BANG_EQUAL:      emitBytes(OP_EQUAL, OP_NOT); break;
        case TOKEN_EQUAL_EQUAL:     emitByte(OP_EQUAL); break;
        case TOKEN_GREATER:         emitByte(OP_GREATER); break;
        case TOKEN_GREATER_EQUAL:   emitBytes(OP_LESS, OP_NOT); break;
        case TOKEN_LESS:            emitByte(OP_LESS); break;
        case TOKEN_LESS_EQUAL:      emitBytes(OP_GREATER, OP_NOT); break;
        case TOKEN_PLUS:            emitByte(OP_ADD); break;
        case TOKEN_MINUS:           emitByte(OP_SUBTRACT); break;
        case TOKEN_STAR:            emitByte(OP_MULTIPLY); break;
        case TOKEN_SLASH:           emitByte(OP_DIVIDE); break;
        default:                    return; // Unreachable
    }
}

/*
    When the parser encouters false, nil, or true it calls this new parser function
*/
static void literal() {
    switch (parser.previous.type) {
        case TOKEN_FALSE:           emitByte(OP_FALSE); break;
        case TOKEN_NIL:             emitByte(OP_NIL); break;
        case TOKEN_TRUE:            emitByte(OP_TRUE); break;
        default:                    return; // Unreachable
    }
}

static void expression() {
    parsePrecedence(PREC_ASSIGNMENT);
}

static void varDecleration() {
    /* The `var` keyword is followed by a variable name that's compiled by `parseVariable` */
    uint8_t global = parseVariable("Expect variable name.");

/*
    Then we look for an = followed by an initializer expression. If the user doesn’t initialize the variable, 
    the compiler implicitly initializes it to nil by emitting an OP_NIL instruction.
*/
    if (match(TOKEN_EQUAL)) {
        expression();
    } else {
        emitByte(OP_NIL);
    }
    consume(TOKEN_SEMICOLON, "Expect ';' after variable decleration."); /* statement should be terminated using a semicolon */
    defineVariable(global);
}

/*
    An "expression statement" is simply an expression followed by a semicolon
    Semanitcally, an expression statement evaluates the exptession and discard the results.
*/
static void expressionStatement() {
    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after expression.");
    emitByte(OP_POP); /* Discarding the results */
}

/*
    if we did match the `print` token, then we compile the rest of the statement here.
*/
static void printStatement() {
    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after value.");
    emitByte(OP_PRINT); 
}

/*
    If we hit a compile error while parsing the previous statement, we enter panic mode. 
    When that happens, after the statement we start synchronizing
*/
static void synchronize() {
    parser.panicMode = false;

    while (parser.current.type != TOKEN_EOF) {

/*
        We skip tokens indiscriminately until we reach something that looks like a statement boundary.
*/
        if (parser.previous.type == TOKEN_SEMICOLON) return;
        switch (parser.current.type) {
            case TOKEN_CLASS:
            case TOKEN_FUN:
            case TOKEN_VAR:
            case TOKEN_FOR:
            case TOKEN_IF:
            case TOKEN_WHILE:
            case TOKEN_PRINT:
            case TOKEN_RETURN:
                return;

            default: 
                ; // Do nothing
        }
        advance();
    }
}

static void decleration() {
    if (match(TOKEN_VAR)) {
        varDecleration(); 
    } else {
        statement();
    }

    if (parser.panicMode) synchronize();
}

static void statement() {
    if (match(TOKEN_PRINT)) {
        printStatement();
    } else {
        expressionStatement();
    }
}

static void grouping() {
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
}

static void number() {
    double value = strtod(parser.previous.start, NULL);
    emitConstant(NUMBER_VAL(value));
}

static void string() {
    emitConstant(OBJ_VAL(copyString(parser.previous.start + 1, parser.previous.length - 2)));
}

static void namedVariable(Token name) {
    uint8_t arg = identifierConstant(&name);
    emitBytes(OP_GET_GLOBAL, arg);
}

static void variable() {
    namedVariable(parser.previous);
}

static void unary() {
    TokenType operatorType = parser.previous.type;
     
    // Compile the operand
    parsePrecedence(PREC_UNARY);

    // Emit the operator instruction
    switch (operatorType) {
        case TOKEN_BANG:            emitByte(OP_NOT); break;
        case TOKEN_MINUS:           emitByte(OP_NEGATE); break;
        default:                    return; // Unreachable
    }
}

/* The table that drives our whole parser is an array of ParseRules */
ParseRule rules[] = {
    [TOKEN_LEFT_PAREN]    = {grouping,  NULL,         PREC_NONE},
    [TOKEN_RIGHT_PAREN]   = {NULL,      NULL,         PREC_NONE},
    [TOKEN_LEFT_BRACE]    = {NULL,      NULL,         PREC_NONE}, 
    [TOKEN_RIGHT_BRACE]   = {NULL,      NULL,         PREC_NONE},
    [TOKEN_COMMA]         = {NULL,      NULL,         PREC_NONE},
    [TOKEN_DOT]           = {NULL,      NULL,         PREC_NONE},
    [TOKEN_MINUS]         = {unary,     binary,       PREC_TERM},
    [TOKEN_PLUS]          = {NULL,      binary,       PREC_TERM},
    [TOKEN_SEMICOLON]     = {NULL,      NULL,         PREC_NONE},
    [TOKEN_SLASH]         = {NULL,      binary,     PREC_FACTOR},
    [TOKEN_STAR]          = {NULL,      binary,     PREC_FACTOR},
    [TOKEN_BANG]          = {unary,     NULL,         PREC_NONE},
    [TOKEN_BANG_EQUAL]    = {NULL,      binary,   PREC_EQUALITY},
    [TOKEN_EQUAL]         = {NULL,      NULL,         PREC_NONE},
    [TOKEN_EQUAL_EQUAL]   = {NULL,      binary,   PREC_EQUALITY},
    [TOKEN_GREATER]       = {NULL,      binary, PREC_COMPARISON},
    [TOKEN_GREATER_EQUAL] = {NULL,      binary, PREC_COMPARISON},
    [TOKEN_LESS]          = {NULL,      binary, PREC_COMPARISON},
    [TOKEN_LESS_EQUAL]    = {NULL,      binary, PREC_COMPARISON},
    [TOKEN_IDENTIFIER]    = {variable,  NULL,         PREC_NONE},
    [TOKEN_STRING]        = {string,    NULL,         PREC_NONE},
    [TOKEN_NUMBER]        = {number,    NULL,         PREC_NONE},
    [TOKEN_AND]           = {NULL,      NULL,         PREC_NONE},
    [TOKEN_CLASS]         = {NULL,      NULL,         PREC_NONE},
    [TOKEN_ELSE]          = {NULL,      NULL,         PREC_NONE},
    [TOKEN_FALSE]         = {literal,   NULL,         PREC_NONE},
    [TOKEN_FOR]           = {NULL,      NULL,         PREC_NONE},
    [TOKEN_FUN]           = {NULL,      NULL,         PREC_NONE},
    [TOKEN_IF]            = {NULL,      NULL,         PREC_NONE},
    [TOKEN_NIL]           = {literal,   NULL,         PREC_NONE},
    [TOKEN_OR]            = {NULL,      NULL,         PREC_NONE},
    [TOKEN_PRINT]         = {NULL,      NULL,         PREC_NONE},
    [TOKEN_RETURN]        = {NULL,      NULL,         PREC_NONE},
    [TOKEN_SUPER]         = {NULL,      NULL,         PREC_NONE},
    [TOKEN_THIS]          = {NULL,      NULL,         PREC_NONE},
    [TOKEN_TRUE]          = {literal,   NULL,         PREC_NONE},
    [TOKEN_VAR]           = {NULL,      NULL,         PREC_NONE},
    [TOKEN_WHILE]         = {NULL,      NULL,         PREC_NONE},
    [TOKEN_ERROR]         = {NULL,      NULL,         PREC_NONE},
    [TOKEN_EOF]           = {NULL,      NULL,         PREC_NONE},
};

/*
    This function starts at the current token and recursively parses any expression at the given precedence level or higher
*/
static void parsePrecedence(Precedence precedence) {
    advance();
    ParseFn prefixRule = getRule(parser.previous.type)->prefix;
    if (prefixRule == NULL) {
        error("Expect expression.");
        return;
    }
    prefixRule();

    while (precedence <= getRule(parser.current.type)->precedence) {
        advance();
        ParseFn infixRule = getRule(parser.previous.type)->infix;
        infixRule();
    }
}

/*
    This function takes the given token and adds its lexeme to the chunk’s constant table as a string. It then returns the index of that constant in the constant table.
*/
static uint8_t identifierConstant(Token* name) {
    return makeConstant(OBJ_VAL(copyString(name->start, name->length)));
}

static uint8_t parseVariable(const char* errorMessage) {
    consume(TOKEN_IDENTIFIER, errorMessage);
    return identifierConstant(&parser.previous);
}

static void defineVariable(uint8_t global) {
    emitBytes(OP_DEFINE_GLOBAL, global);
}

static ParseRule* getRule(TokenType type) {
    return &rules[type];
}

/*
    We pass the function the chunk of code where it will write 
    the code and then returns whether or not compilation succeeded
*/
bool compile(const char* source, Chunk* chunk) {
    initScanner(source);
    compilingChunk = chunk;

    parser.hadError = false;
    parser.panicMode = false;

    advance();
   
    /* We keep compiling declerations until we hit the end of a source file */
    while (!match(TOKEN_EOF)) {
        decleration();
    }

    endCompiler();
    return !parser.hadError;
}
