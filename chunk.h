/*
    This module will define our code representation.
    We will use "chunk" to refer to sequences of bytecode.
*/


#ifndef clox_chunk_h
#define clox_chunk_h

#include "common.h"

/*
    each instruction has a one-byte operation code 
    (universally shortened to opcode)
*/
typedef enum {
    OP_RETURN, // return instruction
} OpCode;

/*
    Bytecode is a series of instructions. Eventually, 
    we’ll store some other data along with the instruction
*/
typedef struct {
    int count;
    int capacity;
    uint8_t* code;
} Chunk;

void initChunk(Chunk* chunk);
void freeChunk(Chunk* chunk);
void writeChunk(Chunk* chunk, uint8_t byte);

#endif
