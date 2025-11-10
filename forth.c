#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <strings.h>

/* ======================== Data structures ======================= */

#define TFOBJ_TYPE_INT    0
#define TFOBJ_TYPE_STR    1
#define TFOBJ_TYPE_BOOL   2
#define TFOBJ_TYPE_LIST   3
#define TFOBJ_TYPE_SYMBOL 4

typedef struct tfobj {
    int refcount;
    int type; // TFOBJ_TYPE_*
    union {
        int i;
        struct {
            char *ptr;
            size_t len;
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

typedef struct {
    tfobj *stack;
} tfctx;

/* =================== Allocation wrappers ======================== */

void *xmalloc(size_t size) {
    void *ptr = malloc(size);
    if (ptr == NULL) {
        fprintf(stderr, "Out of memory allocating %zu bytes\n", size);
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
    l->list.ele = realloc(l->list.ele, sizeof(tfobj*) * (l->list.len+1));
    l->list.ele[l->list.len] = ele;
    l->list.len++;
}

/* Remove the last element from the list and return it.
 * It is up to the caller to decrement the reference count of the
 * element popped from the list if needed. */
tfobj *listPop(tfobj *l) {
    tfobj *last = l->list.ele[l->list.len-1];
    l->list.len--;
    return last;
}

/* ============== Turn program into toy forth list ================ */

void parseSpaces(tfparser *parser) {
    while (isspace(parser->p[0])) {
        parser->p++;
    }
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
int is_symbol_char(int c) {
    char symchars[] = "+-/%*";
    return isalpha(c) || strchr(symchars, c) != NULL;
}

tfobj *parseSymbol(tfparser *parser) {
    char *start = parser->p;
    while (parser->p[0] && is_symbol_char(parser->p[0])) parser->p++;
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
        } else if (is_symbol_char(parser.p[0])) {
            o = parseSymbol(&parser);
        } else {
            o = NULL;
        }

        // Check if the current token produced a parsing error.
        if (o == NULL) {
            // FIXME: release parsed here.
            fprintf(stderr, "Syntax error near %32s ... \n", token_start);
            return NULL;
        } else {
            listPush(parsed, o);
        }
    }
    return parsed;
}

/* ===================== Execute the program ====================== */

void print_object(tfobj *o) {
    switch (o->type) {
    case TFOBJ_TYPE_INT:
        fprintf(stdout, "%d", o->i);
        break;
    case TFOBJ_TYPE_LIST:
        fprintf(stdout, "[");
        for (size_t j = 0; j < o->list.len; j++) {
            tfobj *ele = o->list.ele[j];
            print_object(ele);
            fprintf(stdout, " ");
        }
        fprintf(stdout, "]");
        break;
    case TFOBJ_TYPE_SYMBOL:
        fprintf(stdout, "%s", o->str.ptr);
        break;
    case TFOBJ_TYPE_STR:
        break;
    case TFOBJ_TYPE_BOOL:
        break;
    default:
        fprintf(stdout, "?");
        break;
    }
}

void exec(tfobj *prg) {
    tfctx ctx;
    ctx.stack = createListObject();
    for (size_t j = 0; j < prg->list.len; j++) {
        tfobj *ele = prg->list.ele[j];
        switch (ele->type) {
        case TFOBJ_TYPE_SYMBOL: {
            char *sym = ele->str.ptr;
            if (strcmp(sym, "print") == 0) {
                print_object(ctx.stack);
            } else if (is_symbol_char(sym[0])) {
                // FIXME: decrement reference counters and free memory
                tfobj *b = listPop(ctx.stack);
                tfobj *a = listPop(ctx.stack);
                if (strcmp(sym, "+") == 0)
                    listPush(ctx.stack, createIntObject(a->i+b->i));
                else if (strcmp(sym, "-") == 0)
                    listPush(ctx.stack, createIntObject(a->i-b->i));
                else if (strcmp(sym, "*") == 0)
                    listPush(ctx.stack, createIntObject(a->i*b->i));
                else if (strcmp(sym, "/") == 0)
                    listPush(ctx.stack, createIntObject(a->i/b->i));
                else if (strcmp(sym, "%") == 0)
                    listPush(ctx.stack, createIntObject(a->i%b->i));
            }
            break;
        }
        default:
            listPush(ctx.stack, ele);
            break;
        }
    }
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
    exec(prg);
    return 0;
}
