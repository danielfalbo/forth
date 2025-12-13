#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <string.h>
#include <assert.h>

/* ======================== Data structures ======================= */

#define TF_OK 0
#define TF_ERR 1

#define TFOBJ_TYPE_INT    0
#define TFOBJ_TYPE_STR    1
#define TFOBJ_TYPE_BOOL   2
#define TFOBJ_TYPE_LIST   3
#define TFOBJ_TYPE_SYMBOL 4
#define TFOBJ_TYPE_ALL    255 // Used by listPopType() and other functions.

typedef struct tfobj {
    int refcount;
    int type; // TFOBJ_TYPE_*
    union {
        int i;
        struct {
            char *ptr;
            size_t len;
            // TODO: int quoted; // True or false
        } str;
        struct {
            struct tfobj **ele;
            size_t len;
        } list;
    };
} tfobj;

typedef struct tfparser {
    char *prgtext; // The program to compile into a list.
    char *p;       // Next token to parse.
} tfparser;

typedef struct tfctx tfctx;

/* Function table entry: each of this entry represents a symbol name
 * associated with a function implementation. */
typedef struct FunctionTableEntry {
    tfobj *name;
    int (*callback) (tfctx *ctx, char *name);
    tfobj *user_func;
} tffunc;

typedef struct FunctionTable {
    tffunc **func_table;
    size_t func_count;
} FunctionTable;

/* Our execution context. */
struct tfctx {
    tfobj *stack;
    FunctionTable functable;
};

/* =================== Function prototypes ======================== */

// Standard library prototypes.
int basicMathFunctions(tfctx *ctx, char *name);

/* =================== Allocation wrappers ======================== */

void *xmalloc(size_t size) {
    void *ptr = malloc(size);
    if (ptr == NULL) {
        fprintf(stderr, "Out of memory allocating %zu bytes\n", size);
        exit(1);
    }
    return ptr;
}

void *xrealloc(void *oldptr, size_t size) {
    void *ptr = realloc(oldptr, size);
    if (ptr == NULL) {
        fprintf(stderr, "Out of memory reallocating %zu bytes\n", size);
        exit(1);
    }
    return ptr;
}

/* =================== Object related functions ===================
 * The following functions allocate toy forth objects of different types. */

/* Allocate and initialize a new Toy Forth object. */
tfobj *createObject(int type) {
    tfobj *o = xmalloc(sizeof(tfobj));
    o->type = type;
    o->refcount = 1;
    return o;
}

tfobj *createIntObject(int i) {
    tfobj *o = createObject(TFOBJ_TYPE_INT);
    o->i = i;
    return o;
}

tfobj *createBoolObject(int i) {
    tfobj *o = createIntObject(i);
    o->type = TFOBJ_TYPE_BOOL;
    return o;
}

void retain(tfobj *o) {
    o->refcount++;
}

void freeObject(tfobj *o);

void release(tfobj *o) {
    assert(o->refcount > 0);
    o->refcount--;
    if (o->refcount == 0) freeObject(o);
}

/* Free an object and all the other nested objects. */
void freeObject(tfobj *o) {
    assert (o->refcount == 0);

    switch (o->type) {
    case TFOBJ_TYPE_LIST:
        for (size_t j = 0; j < o->list.len; j++) {
            tfobj *ele = o->list.ele[j];
            release(ele);
        }
        break;
    case TFOBJ_TYPE_SYMBOL:
    case TFOBJ_TYPE_STR:
        free(o->str.ptr);
        break;
    }

    free(o);
}

void printObject(tfobj *o) {
    switch (o->type) {
    case TFOBJ_TYPE_BOOL:
    case TFOBJ_TYPE_INT:
        fprintf(stdout, "%d", o->i);
        break;
    case TFOBJ_TYPE_LIST:
        fprintf(stdout, "[");
        for (size_t j = 0; j < o->list.len; j++) {
            tfobj *ele = o->list.ele[j];
            printObject(ele);
            if (j != o->list.len - 1) fprintf(stdout, " ");
        }
        fprintf(stdout, "]\n");
        break;
    case TFOBJ_TYPE_STR:
        fprintf(stdout, "\"%s\"", o->str.ptr);
        break;
    case TFOBJ_TYPE_SYMBOL:
        fprintf(stdout, "%s", o->str.ptr);
        break;
    default:
        fprintf(stdout, "?");
        break;
    }
}

/* ===================== String object ============================ */

tfobj *createStringObject(char *s, size_t len) {
    tfobj *o = createObject(TFOBJ_TYPE_STR);
    o->str.ptr = xmalloc(len+1);
    o->str.len = len;
    memcpy(o->str.ptr, s, len);
    o->str.ptr[len] = 0;
    return o;
}

tfobj *createSymbolObject(char *s, size_t len) {
    tfobj *o = createStringObject(s, len);
    o->type = TFOBJ_TYPE_SYMBOL;
    return o;
}

/* Compare the two string objects 'a' and 'b', returns 0 if they are
 * the same, '1' if a>b, '-1' if a<b. The comparison is performed
 * using memcmp(). */
int compareStringObject(tfobj *a, tfobj *b) {
    size_t minlen = a->str.len < b->str.len ? a->str.len : b->str.len;
    int cmp = memcmp(a->str.ptr, b->str.ptr, minlen);

    if (cmp == 0) {
        if (a->str.len == b->str.len) return 0;
        else if (a->str.len > b->str.len) return 1;
        else return -1;
    } else {
        if (cmp < 0) return -1;
        else return 1;
    }
}

/* ===================== List object ============================== */

tfobj *createListObject(void) {
    tfobj *o = createObject(TFOBJ_TYPE_LIST);
    o->list.ele = NULL;
    o->list.len = 0;
    return o;
}

/* Add the new element at the end of the list 'l'.
 * It is up to the caller to increment the reference count of the
 * element added to the list if needed. */
void listPush(tfobj *l, tfobj *ele) {
    l->list.ele = xrealloc(l->list.ele, sizeof(tfobj*) * (l->list.len+1));
    l->list.ele[l->list.len] = ele;
    l->list.len++;
}

tfobj *listPopType(tfctx *ctx, int type) {
    tfobj *stack = ctx->stack;
    if (stack->list.len == 0) return NULL;
    tfobj *to_pop = stack->list.ele[stack->list.len-1];
    if (type != TFOBJ_TYPE_ALL && to_pop->type != type) return NULL;

    stack->list.len--;
    if (stack->list.len == 0) {
        free(stack->list.ele);
        stack->list.ele = NULL;
    } else {
        stack->list.ele = xrealloc(stack->list.ele,
                                    sizeof(tfobj*) * (stack->list.len));
    }

    return to_pop;
}

tfobj *listPop(tfctx *ctx) {
    return listPopType(ctx, TFOBJ_TYPE_ALL);
}

/* ============== Turn program into toy forth list ================ */

void parseSpaces(tfparser *parser) {
    while (isspace(parser->p[0])) parser->p++;
}

#define MAX_NUM_LEN 128
tfobj *parseNumber(tfparser *parser) {
    char buf[MAX_NUM_LEN];
    char *start = parser->p;
    char *end;

    if (parser->p[0] == '-') parser->p++;
    while (parser->p[0] && isdigit(parser->p[0])) parser->p++;
    end = parser->p;
    int numlen = end-start;
    if (numlen >= MAX_NUM_LEN) return NULL;

    memcpy(buf, start, numlen);
    buf[numlen] = 0;

    tfobj *o = createIntObject(atoi(buf));
    return o;
}

/* Return true if the character 'c' is one of the characters
 * acceptable for our symbols. */
int isSymbolChar(int c) {
    char symchars[] = "+-/%*";
    return isalpha(c) || strchr(symchars, c) != NULL;
}

tfobj *parseSymbol(tfparser *parser) {
    char *start = parser->p;
    while (parser->p[0] && isSymbolChar(parser->p[0])) parser->p++;
    char *end = parser->p;
    int len = end-start;
    return createSymbolObject(start, len);
}


tfobj *compile(char *prgtext) {
    tfparser parser;
    parser.prgtext = prgtext;
    parser.p = prgtext;

    tfobj *parsed = createListObject();

    while (parser.p) {
        tfobj *o;
        char *token_start = parser.p;

        parseSpaces(&parser);
        if (parser.p[0] == 0) break; // End of program reached.

        if (isdigit(parser.p[0])
            || (parser.p[0] == '-' && isdigit(parser.p[1]))) {
            o = parseNumber(&parser);
        } else if (isSymbolChar(parser.p[0])) {
            o = parseSymbol(&parser);
        } else {
            o = NULL;
        }

        // Check if the current token produced a parsing error.
        if (o == NULL) {
            release(parsed);
            fprintf(stderr, "Syntax error near %32s ... \n", token_start);
            return NULL;
        } else {
            listPush(parsed, o);
        }
    }
    return parsed;
}

/* ===================== Execution and context ==================== */

int ctxCheckStackMinLen(tfctx *ctx, size_t min) {
    return (ctx->stack->list.len < min) ? TF_ERR : TF_OK;
}

/* Pop the top element from the interpreter main stack, assuming it
 * will match 'type', otherwise NULL is returned. Also the function
 * returns NULL if the stack is empty.
 *
 * The reference counting of the popped object is not modified: it
 * is assumed that we just transfer the ownership from the stack to
 * the caller. */
tfobj *ctxStackPop(tfctx *ctx, int type) {
    return listPopType(ctx, type);
}

/* Just push the object on the interpreter main stack. */
void ctxStackPush(tfctx *ctx, tfobj *obj) {
    listPush(ctx->stack, obj);
}

/* Resolve the function scanning the function table looking for a matching
 * name. If a matching function was not found, NULL is returned, otherwise
 * the function returns the function entry object. */
tffunc *getFunctionByName(tfctx *ctx, tfobj *name) {
    for (size_t j = 0; j < ctx->functable.func_count; j++) {
        tffunc *fe = ctx->functable.func_table[j];
        if (compareStringObject(fe->name, name) == 0)
            return fe;
    }
    return NULL;
}

/* Push a new function entry in the context. It's up to the caller
 * to set either the C callback or the list representing the user
 * defined function. */
tffunc *registerFunction(tfctx *ctx, tfobj *name) {
    ctx->functable.func_table =
        xrealloc(ctx->functable.func_table,
                 sizeof(tffunc*) * (ctx->functable.func_count+1));
    tffunc *fe = xmalloc(sizeof(tffunc));
    ctx->functable.func_table[ctx->functable.func_count] = fe;
    ctx->functable.func_count++;
    fe->name = name;
    retain(name);
    fe->callback = NULL;
    fe->user_func = NULL;
    return fe;
}

/* Register a new function with the given name in the function table
 * of the context. The function can't fail since if a function with the
 * same name already exists, it gets replaced by the new one. */
void registerCFunction(tfctx *ctx, char *name,
                      int (*callback) (tfctx *ctx, char *name)
) {
    tffunc *fe;
    tfobj *oname = createStringObject(name, strlen(name));
    fe = getFunctionByName(ctx, oname);
    if (fe) {
        if (fe->user_func) {
            release(fe->user_func);
            fe->user_func = NULL;
        }
        fe->callback = callback;
    } else {
        fe = registerFunction(ctx, oname);
        fe->callback = callback;
    }
    release(oname);
}

// tffunc registerUserFunction() {}

tfctx *createContext(void) {
    tfctx *ctx = xmalloc(sizeof(*ctx));
    ctx->stack = createListObject();
    ctx->functable.func_table = NULL;
    ctx->functable.func_count = 0;
    registerCFunction(ctx, "+", basicMathFunctions);
    // registerUserFunction();
    return ctx;
}

/* Try to resolve and call the function associated with the symbol
 * name 'word'. Return TF_OK if the symbol was actually bound to some
 * function and was executed, return TF_ERR otherwise (on error). */
int callSymbol(tfctx *ctx, tfobj *word) {
    // Scan function table from ctx
    tffunc *fe = getFunctionByName(ctx, word);
    if (fe == NULL) return TF_ERR;

    if (fe->user_func) {
        // TODO: exec
        return TF_ERR;
    } else {
        return fe->callback(ctx, fe->name->str.ptr);
    }

    // char *sym = word->str.ptr;
    // if (strcmp(sym, "print") == 0) {
    //     printObject(ctx->stack);
    // } else if (strcmp(sym, "dup") == 0) {
    //     // TODO: listPush deep copy of last word.
    // } else if (strcmp(sym, "if") == 0) {
    //     // TODO:
    // } else if (word->str.len == 1 && isSymbolChar(sym[0])) {
    //     // TODO: check object types, implement < > = support for bools
    // }

    return TF_OK;
}

/* Execute the Toy Forth program stored into the list 'prg'.  */
int exec(tfctx *ctx, tfobj *prg) {
    assert(prg->type == TFOBJ_TYPE_LIST);
    for (size_t j = 0; j < prg->list.len; j++) {
        tfobj *word = prg->list.ele[j];
        switch (word->type) {
        case TFOBJ_TYPE_SYMBOL:
            if (callSymbol(ctx, word) == TF_ERR) {
                printf("Run time error\n");
                return TF_ERR;
            }
            break;
        default:
            ctxStackPush(ctx, word);
            retain(word);
            break;
        }
    }
    return TF_OK;
}

/* ===================== Basic standard library =================== */

int basicMathFunctions(tfctx *ctx, char *name) {
    if (ctxCheckStackMinLen(ctx,2)) return TF_ERR;

    // TODO: if (ctxCheckTypes(ctx, TFOBJ_TYPE_INT, TFOBJ_TYPE_INT, -1))

    tfobj *b = ctxStackPop(ctx,TFOBJ_TYPE_INT);
    if (b == NULL) return TF_ERR;
    tfobj *a = ctxStackPop(ctx,TFOBJ_TYPE_INT);
    if (a == NULL) {
        ctxStackPush(ctx, b);
        return TF_ERR;
    }

    int result;
    switch (name[0]) {
    case '+': result = a->i + b->i; break;
    case '-': result = a->i - b->i; break;
    case '*': result = a->i * b->i; break;
    case '/': result = a->i / b->i; break;
    case '%': result = a->i % b->i; break;
    // default:                        break;
    }
    release(a);
    release(b);

    ctxStackPush(ctx, createIntObject(result));
    return TF_OK;
}

/* ============================ Main ============================== */

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <filename>\n", argv[0]);
        return 1;
    }

    /* Read the program in memory for later parsing. */
    int fd = open(argv[1], O_RDONLY);
    if (fd == -1) {
        perror("Opening Toy Forth program");
        return 1;
    }
    off_t file_size = lseek(fd, 0, SEEK_END);
    char *prgtext = xmalloc(file_size+1);
    lseek(fd, 0, SEEK_SET);
    read(fd, prgtext, file_size);
    prgtext[file_size] = 0;
    close(fd);

    tfobj *prg = compile(prgtext);
    printObject(prg);
    tfctx *ctx = createContext();

    exec(ctx, prg);

    fprintf(stdout, "Stack content at end: ");
    printObject(ctx->stack);
    return 0;
}
