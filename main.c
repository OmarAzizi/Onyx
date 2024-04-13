#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vm.h"
#include "common.h"

#include <editline/readline.h>
#include <editline/history.h>

static void repl() {
    puts("ONYX Version 9.0.1");
    puts("Press Ctrl+c to Exit\n");

    while (1) {
        char* input = readline(">> ");
        add_history(input);
        
        int scopeCount = 0;

        if (input[strlen(input) - 1] == '{') {
            ++scopeCount;
            while (1) {
                strcat(input, readline(".. "));
                if (input[strlen(input) - 1] == '{')
                    ++scopeCount;
                if (input[strlen(input) - 1] == '}')
                    --scopeCount;
                if (scopeCount == 0)
                    break;
            }

        }

        interpret(input);
        free(input);
    }
}

static char* readFile(const char* path) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "Could not open file \"%s\".\n", path);
        exit (74);
    }
     
    /* Figuring out the size of the file (in Bytes) */
    fseek(file, 0L, SEEK_END);
    size_t fileSize = ftell(file);
    rewind(file);
    
    /* Allocating memory for the file buffer */
    char* buffer = (char*)malloc(fileSize + 1);
    if (buffer == NULL) {
        fprintf(stderr, "Not enough memory to read \"%s\".\n", path);
        exit(74);
    }
    size_t bytesRead = fread(buffer, sizeof(char), fileSize, file);
    if (bytesRead < fileSize) {
        fprintf(stderr, "Could not read file \"%s\".\n", path);
        exit(74);
    }
    buffer[bytesRead + 1] = '\0';

    fclose(file);
    return buffer;
}

static void runFile(const char* path) {
    char* source = readFile(path);
    InterpretResult result = interpret(source);
    free(source);

    if (result == INTERPRET_COMPILE_ERROR) exit(65);
    if (result == INTERPRET_RUNTIME_ERROR) exit(70);
}

int main(int argc, char** argv) {
    initVM();
    
    if (argc == 1) repl(); // Read, Evaluate, Print, Loop
    else if (argc == 2) runFile(argv[1]); // Read source file
    else {
        fprintf(stderr, "Usage: clox [path]\n");
        exit(64);
    }

    freeVM();
    return 0;
} 
