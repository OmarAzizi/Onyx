#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "compiler.h"
#include "vm.h"
#include "debug.h"

VM vm;

static void runtimeError(const char* format, ...);

/*
    This native function returns the elapsed time since the program started running, in seconds.
*/
static Value clockNative(int argCount, Value* args) {
    return NUMBER_VAL((double)clock() / CLOCKS_PER_SEC);
}

static Value inputNative(int argCount, Value* args) {
    char input[2048];
    printf("%s", AS_CSTRING(args[0]));
    fgets(input, sizeof(input), stdin);
    ObjString* str = copyString(input, strlen(input));
    return OBJ_VAL(str);
}

static Value numNative(int argCount, Value* args) {
    char* ptr;
    double number = strtod(AS_CSTRING(args[0]), &ptr);
    return NUMBER_VAL(number);
}

static void resetStack() { 
    vm.stackTop = vm.stack;
    vm.frameCount = 0;
    vm.openUpvalues = NULL;
}

static void runtimeError(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputs("\n", stderr);
    
/*
    Implementing stack traces that print out each function that was still executing when the program died
*/
    for (int i = vm.frameCount - 1; i >= 0; --i) {
        CallFrame* frame = &vm.frames[i];
        ObjFunction* function = frame->closure->function;
        
        size_t instruction = frame->ip - function->chunk.code - 1;
        fprintf(stderr, "[line %d] in ", function->chunk.lines[instruction]);
        
        if (function->name == NULL) {
            fprintf(stderr, "script\n");
        } else {
            fprintf(stderr, "%s()\n", function->name->chars);
        }
    }

    resetStack();
}

/*
    This is a helper to define a new native function exposed to the users of the language
    It takes a pointer to a C function and a name it will be known as in the language.
*/
static void defineNative(const char* name, NativeFn function) {
    push(OBJ_VAL(copyString(name, (int)strlen(name))));
    push(OBJ_VAL(newNative(function)));
    tableSet(&vm.globals, AS_STRING(vm.stack[0]), vm.stack[1]);
    pop();
    pop();
}

void initVM() {
    resetStack();
    vm.objects = NULL;
    initTable(&vm.globals);
    initTable(&vm.strings);

    /* Using the `defineNative` helper interface to define a new native function */
    defineNative("clock", clockNative); 
    defineNative("input", inputNative);
    defineNative("num", numNative);
}

void freeVM() {
    freeTable(&vm.globals);
    freeTable(&vm.strings);
    freeObjects();
}

void push(Value value) {
    *vm.stackTop = value; // store value
    ++vm.stackTop;        // increment 'top' ptr
}

Value pop() {
    --vm.stackTop;        // decrement 'top' ptr
    return *vm.stackTop;  // return popped value
}

static Value peek(int distance) {
    return vm.stackTop[-1 - distance];
}

static bool call(ObjClosure* closure, int argCount) {
/*
    This simply initializes the next CallFrame on the stack. It stores a pointer to the function being called 
    and points the frame’s ip to the beginning of the function’s bytecode.

    Finally, it sets up the slots pointer to give the frame its window into the stack
*/

    /* Handling error of passing too many or too less arguments */
    if (argCount != closure->function->arity) {
        runtimeError("Expected %d arguments but got %d.", closure->function->arity, argCount);
        return false;
    }
    
    /* There’s another error we need to report.  Because the CallFrame array has a fixed size, we need to ensure a deep call chain doesn’t overflow */
    if (vm.frameCount == FRAMES_MAX) {
        runtimeError("Stack overflow.");
        return false;
    }

    CallFrame* frame = &vm.frames[vm.frameCount++];
    frame->closure = closure;
    frame->ip = closure->function->chunk.code;
    frame->slots = vm.stackTop - argCount - 1; /* The `-1` is to account for stack slot zero which the compiler set aside for when we add methods later. */
    return true;
}

static bool callValue(Value callee, int argCount) {
    if (IS_OBJ(callee)) {
        switch (OBJ_TYPE(callee)) {
            case OBJ_CLOSURE:
                return call(AS_CLOSURE(callee), argCount);
            case OBJ_NATIVE: {
            /*
                If the object being called is a native function, we invoke the C function right then and there. 
                There’s no need to muck with CallFrames or anything. We just hand off to C, get the result, and stuff it back in the stack.
            */
                NativeFn native = AS_NATIVE(callee);
                Value result = native(argCount, vm.stackTop - argCount);
                vm.stackTop -= argCount + 1;
                push(result);
                return true;
            }
            default:
                break; /* Non-callable object type. */
        }
    }
    runtimeError("Can only call functions and classes.");
    return false;
}

static ObjUpvalue* captureUpvalue(Value* local) {
    ObjUpvalue* prevUpvalue = NULL;
    ObjUpvalue* upvalue = vm.openUpvalues;
    while (upvalue != NULL && upvalue->location > local) {
        prevUpvalue = upvalue;
        upvalue = upvalue->next;
    }
    
    if (upvalue != NULL && upvalue->location == local) {
        return upvalue;
    }

    ObjUpvalue* createdUpvalue = newUpvalue(local);
    createdUpvalue->next = upvalue;

    if (prevUpvalue == NULL) {
        vm.openUpvalues = createdUpvalue;
    } else {
        prevUpvalue->next = createdUpvalue;
    }

    return createdUpvalue;
}

static void closeUpvalues(Value* last) {
    while (vm.openUpvalues != NULL && vm.openUpvalues->location >= last) {
        ObjUpvalue* upvalue = vm.openUpvalues;
        upvalue->closed = *upvalue->location;
        upvalue->location = &upvalue->closed;
        vm.openUpvalues = upvalue->next;
    }
}

/*
    Here i'm following the rule in Ruby that `nil` and `false` are falsey and every other value behaves like `true`
*/
static bool isFalsey(Value value) {
    return IS_NIL(value) || (IS_BOOL(value) && !AS_BOOL(value));
}

static void concatenate() {
    ObjString* b = AS_STRING(pop());
    ObjString* a = AS_STRING(pop());

    int length = a->length + b->length;
    char* chars = ALLOCATE(char, length);
    memcpy(chars, a->chars, a->length);
    memcpy(chars + a->length, b->chars, b->length);
    chars[length] = '\0';

    ObjString* result = takeString(chars, length);
    push(OBJ_VAL(result));
}

static InterpretResult modulus() {
    if (!IS_NUMBER(peek(0)) || !IS_NUMBER(peek(1))) { 
        runtimeError("Operands must be numbers."); 
        return INTERPRET_RUNTIME_ERROR; 
    } 
    double b = AS_NUMBER(pop());
    double a = AS_NUMBER(pop()); 

    push(NUMBER_VAL(a - ((int)(a / b) * b)));
    return INTERPRET_OK;
}

static InterpretResult intDivison() {
    if (!IS_NUMBER(peek(0)) || !IS_NUMBER(peek(1))) { 
        runtimeError("Operands must be numbers."); 
        return INTERPRET_RUNTIME_ERROR; 
    } 
    double b = AS_NUMBER(pop());
    double a = AS_NUMBER(pop()); 
    
    push(NUMBER_VAL(((int)a / (int)b)));
    return INTERPRET_OK;
}

static InterpretResult run() {
    CallFrame* frame = &vm.frames[vm.frameCount - 1];

#define READ_BYTE() (*frame->ip++) // This macro reads the byte currently pointed at by the instruction pointer and then it increments it
                                  
#define READ_CONSTANT() \
    (frame->closure->function->chunk.constants.values[READ_BYTE()])

#define READ_STRING() AS_STRING(READ_CONSTANT())

#define READ_SHORT() \
    (frame->ip += 2, \
    (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))

#define BINARY_OP(valueType, op) \
    do { \
        if (!IS_NUMBER(peek(0)) || !IS_NUMBER(peek(1))) { \
            runtimeError("Operands must be numbers."); \
            return INTERPRET_RUNTIME_ERROR; \
        } \
        double b = AS_NUMBER(pop()); \
        double a = AS_NUMBER(pop()); \
        push(valueType(a op b)); \
    } while (false)

    for (;;) {

#ifdef DEBUG_TRACE_EXECUTION
        // Stack Tracing (Printing contents of the VM's stack from bottom up)
        printf("            ");
        for (Value* slot = vm.stack; slot < vm.stackTop; ++slot) {
            printf("[");
            printValue(*slot);
            printf("]");
        }
        printf("\n");

        // When this flag is defined the VM disassembles and prints each instruction right before executing it    
        disassembleInstruction(&frame->closure->function->chunk, 
                (int)(frame->ip - frame->closure->function->chunk.code));
#endif
        uint8_t instruction;
        
        // Decoding (dispatching) the instruction
        switch (instruction = READ_BYTE()) {
            // The body of each case implements that opcode’s behavior.
            case OP_CONSTANT: {
                Value constant = READ_CONSTANT();
                push(constant);
                break;
            }
            case OP_NIL:        push(NIL_VAL); break;
            case OP_TRUE:       push(BOOL_VAL(true)); break;
            case OP_FALSE:      push(BOOL_VAL(false)); break;
            case OP_POP:        pop(); break;
            case OP_GET_LOCAL: {
                uint8_t slot = READ_BYTE();
                push(frame->slots[slot]);
                break;
            }
            case OP_SET_LOCAL: {
                uint8_t slot = READ_BYTE();
                frame->slots[slot] = peek(0);
                break;
            }
            case OP_GET_GLOBAL: {
                ObjString* name = READ_STRING();
                Value value;
                if (!tableGet(&vm.globals, name, &value)) {
                    runtimeError("Undefined variable '%s'.", name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(value);
                break;
            }
            case OP_DEFINE_GLOBAL: {
                ObjString* name = READ_STRING(); /* We get the name of the variable from the constants table */
                tableSet(&vm.globals, name, peek(0));
                pop();
                break;
            }
            case OP_SET_GLOBAL: {
                ObjString* name = READ_STRING();
                if (tableSet(&vm.globals, name, peek(0))) {
                    tableDelete(&vm.globals, name);
                    runtimeError("Undefined variable '%s'.", name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case OP_GET_UPVALUE: {
                uint8_t slot = READ_BYTE();
                push(*frame->closure->upvalues[slot]->location);
                break;
            }
            case OP_SET_UPVALUE: {
                uint8_t slot = READ_BYTE();
                *frame->closure->upvalues[slot]->location = peek(0);
                break;
            }
            case OP_EQUAL: {
                Value b = pop();
                Value a = pop();
                push(BOOL_VAL(valuesEqual(a, b)));
                break;
            }
            case OP_GREATER:    BINARY_OP(BOOL_VAL, >); break;
            case OP_LESS:       BINARY_OP(BOOL_VAL, <); break;
            case OP_ADD: {
                if (IS_STRING(peek(0)) && IS_STRING(peek(1))) 
                    concatenate();
                else if (IS_NUMBER(peek(0)) && IS_NUMBER(peek(1))) {
                    double b = AS_NUMBER(pop());
                    double a = AS_NUMBER(pop());
                    push(NUMBER_VAL(a + b));
                } else {
                    runtimeError("Operands must be two numbers of two strings.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case OP_SUBTRACT:   BINARY_OP(NUMBER_VAL, -); break;
            case OP_MULTIPLY:   BINARY_OP(NUMBER_VAL, *); break;
            case OP_DIVIDE:     BINARY_OP(NUMBER_VAL, /); break;
            case OP_INT_DIVIDE: {
                if (intDivison() == INTERPRET_RUNTIME_ERROR) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case OP_MODULUS:    {
                if (modulus() == INTERPRET_RUNTIME_ERROR) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case OP_NOT:        push(BOOL_VAL(isFalsey(pop()))); break;
            case OP_NEGATE:     
                if (!IS_NUMBER(peek(0))) {
                    runtimeError("Operand must be a number.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(NUMBER_VAL(-AS_NUMBER(pop())));
                break;
            case OP_PRINT: {
                printValue(pop());
                printf("\n");
                break;
            }
            case OP_JUMP: {
                uint16_t offset = READ_SHORT();
                frame->ip += offset;
                break;
            }
            case OP_JUMP_IF_FALSE: {
                /* It reads the 16-bit operand from the chunk */
                uint16_t offset = READ_SHORT();
                if (isFalsey(peek(0))) frame->ip += offset;
                break;
            }
            case OP_LOOP: {
                uint16_t offset = READ_SHORT();
                frame->ip -= offset;
                break;
            }
            case OP_CALL: {
                int argCount = READ_BYTE();
                if (!callValue(peek(argCount), argCount)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                frame = &vm.frames[vm.frameCount - 1];
                break;
            }
            case OP_CLOSURE: {
                ObjFunction* function = AS_FUNCTION(READ_CONSTANT());
                ObjClosure* closure = newClosure(function);
                push(OBJ_VAL(closure));
                
                /*
                    We iterate over each upvalue the closure expects.
                */
                for (int i = 0; i < closure->upvalueCount; ++i) {
                    /* For each we read a pair of operand bytes. */
                    uint8_t isLocal = READ_BYTE();
                    uint8_t index = READ_BYTE();

                    if (isLocal) {
                        /* Id the upvalue closes over a local variable in the enclosing function we let `captureUpvalue` do the work */
                        closure->upvalues[i] = captureUpvalue(frame->slots + index);
                    } else {
                        /* Otherwise we capture upvalue from the surrounding function */
                        closure->upvalues[i] = frame->closure->upvalues[index];
                    }
                }
                break;
            }
            case OP_CLOSE_UPVALUE:
                closeUpvalues(vm.stackTop - 1);
                pop();
                break;
            case OP_RETURN: {
                /* We are about to discard the function's stack window so we pop the return value and hang it */
                Value result = pop(); 
                
                /* Discarding the function's CallFrame */
                closeUpvalues(frame->slots);
                vm.frameCount--;

                if (vm.frameCount == 0) {
                /* 
                    If it was the ver last CallFrame, this means we finished executing top-level code/script 
                */
                    pop();
                    return INTERPRET_OK;
                }

                vm.stackTop = frame->slots;
                push(result); /* pushing the return value onto the stack window of the caller */
                frame = &vm.frames[vm.frameCount - 1]; /* Update the `run` function's  cached pointer */
                break;
            }
        }
    }

#undef READ_BYTE
#undef READ_CONSTANT
#undef READ_CONSTANT
#undef READ_CONSTANT
#undef BINARY_OP
}

InterpretResult interpret(const char* source) {
    ObjFunction* function = compile(source);
    if (function == NULL) return INTERPRET_COMPILE_ERROR;

    push(OBJ_VAL(function));
    ObjClosure* closure = newClosure(function);
    pop();
    push(OBJ_VAL(closure));
    call(closure, 0);

    return run();
}

