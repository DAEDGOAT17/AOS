#include "core/lisp.h"
#include "ata.h"
#include "fat32.h"
#include "kmalloc.h"
#include "screen.h"
#include "string.h"
#include "timer.h"
#include <stdint.h>
#include <stddef.h>

#define LISP_MAX_SOURCE 3000U
#define LISP_MAX_NODES 512U
#define LISP_MAX_DEPTH 8U
#define LISP_MAX_ARGUMENTS 4U
#define LISP_MAX_VARIABLES 24U
#define LISP_MAX_STEPS 50000U
#define LISP_TEXT_SIZE 256U
#define LISP_CANVAS_WIDTH 60U
#define LISP_CANVAS_HEIGHT 20U

enum {
    LISP_NIL,
    LISP_INTEGER,
    LISP_STRING,
    LISP_SYMBOL,
    LISP_LIST
};

typedef struct {
    int type;
    int child;
    int next;
    int32_t integer;
    char text[LISP_TEXT_SIZE];
} lisp_node_t;

typedef struct {
    lisp_node_t nodes[LISP_MAX_NODES];
    uint32_t count;
    uint32_t depth;
    const char* cursor;
    int failed;
} lisp_parser_t;

typedef struct {
    int type;
    int32_t integer;
    char text[LISP_TEXT_SIZE];
} lisp_value_t;

typedef struct {
    char name[16];
    lisp_value_t value;
} lisp_variable_t;

typedef struct {
    const lisp_parser_t* parser;
    const char* argument;
    lisp_variable_t variables[LISP_MAX_VARIABLES];
    uint32_t variable_count;
    uint32_t steps;
} lisp_eval_context_t;

static char lisp_canvas[LISP_CANVAS_HEIGHT][LISP_CANVAS_WIDTH];
static uint32_t lisp_canvas_width;
static uint32_t lisp_canvas_height;
extern char kbd_get(void);

static void lisp_skip_space(lisp_parser_t* parser) {
    while (*parser->cursor == ' ' || *parser->cursor == '\t' ||
           *parser->cursor == '\r' || *parser->cursor == '\n') parser->cursor++;
}

static int lisp_new_node(lisp_parser_t* parser, int type) {
    if (parser->count >= LISP_MAX_NODES) {
        parser->failed = 1;
        return -1;
    }
    int node = (int)parser->count++;
    parser->nodes[node].type = type;
    parser->nodes[node].child = -1;
    parser->nodes[node].next = -1;
    parser->nodes[node].integer = 0;
    parser->nodes[node].text[0] = '\0';
    return node;
}

static int lisp_parse_expression(lisp_parser_t* parser);

static int lisp_parse_list(lisp_parser_t* parser) {
    int list = lisp_new_node(parser, LISP_LIST);
    int last = -1;
    if (list < 0 || ++parser->depth > LISP_MAX_DEPTH) {
        parser->failed = 1;
        return -1;
    }
    parser->cursor++;
    for (;;) {
        lisp_skip_space(parser);
        if (*parser->cursor == ')') {
            parser->cursor++;
            parser->depth--;
            return list;
        }
        if (!*parser->cursor) break;
        int expression = lisp_parse_expression(parser);
        if (expression < 0) break;
        if (last < 0) parser->nodes[list].child = expression;
        else parser->nodes[last].next = expression;
        last = expression;
    }
    parser->failed = 1;
    return -1;
}

static int lisp_parse_expression(lisp_parser_t* parser) {
    lisp_skip_space(parser);
    if (!*parser->cursor) {
        parser->failed = 1;
        return -1;
    }
    if (*parser->cursor == '(') return lisp_parse_list(parser);

    if (*parser->cursor == '"') {
        int node = lisp_new_node(parser, LISP_STRING);
        uint32_t length = 0;
        if (node < 0) return -1;
        parser->cursor++;
        while (*parser->cursor && *parser->cursor != '"') {
            char value = *parser->cursor++;
            if (value == '\\') {
                value = *parser->cursor++;
                if (value == 'n') value = '\n';
                else if (value == 't') value = '\t';
                else if (value != '\\' && value != '"') {
                    parser->failed = 1;
                    return -1;
                }
            }
            if (!value || length + 1 >= LISP_TEXT_SIZE) {
                parser->failed = 1;
                return -1;
            }
            parser->nodes[node].text[length++] = value;
        }
        if (*parser->cursor++ != '"') {
            parser->failed = 1;
            return -1;
        }
        parser->nodes[node].text[length] = '\0';
        return node;
    }

    int node = lisp_new_node(parser, LISP_SYMBOL);
    uint32_t length = 0;
    int negative = 0;
    int numeric = 1;
    int64_t number = 0;
    const char* start = parser->cursor;
    if (node < 0) return -1;
    while (*parser->cursor && *parser->cursor != ' ' && *parser->cursor != '\t' &&
           *parser->cursor != '\r' && *parser->cursor != '\n' &&
           *parser->cursor != '(' && *parser->cursor != ')') {
        char value = *parser->cursor++;
        if (length + 1 >= LISP_TEXT_SIZE) {
            parser->failed = 1;
            return -1;
        }
        parser->nodes[node].text[length++] = value;
    }
    if (parser->cursor == start) {
        parser->failed = 1;
        return -1;
    }
    parser->nodes[node].text[length] = '\0';

    for (uint32_t index = 0; index < length; index++) {
        if (parser->nodes[node].text[index] >= 'A' && parser->nodes[node].text[index] <= 'Z')
            parser->nodes[node].text[index] += 'a' - 'A';
    }

    uint32_t digit = 0;
    if (parser->nodes[node].text[0] == '-') {
        negative = 1;
        digit = 1;
        if (length == 1) numeric = 0;
    }
    for (; digit < length; digit++) {
        char value = parser->nodes[node].text[digit];
        if (value < '0' || value > '9') {
            numeric = 0;
            break;
        }
        number = number * 10 + (value - '0');
        if (number > 2147483648LL) {
            parser->failed = 1;
            return -1;
        }
    }
    if (numeric) {
        if ((!negative && number > 2147483647LL) || (negative && number > 2147483648LL)) {
            parser->failed = 1;
            return -1;
        }
        parser->nodes[node].type = LISP_INTEGER;
        parser->nodes[node].integer = negative ? (int32_t)-number : (int32_t)number;
    }
    return node;
}

static int lisp_parse_program(const char* source, lisp_parser_t* parser) {
    if (!source || !parser || strlen(source) > LISP_MAX_SOURCE) return -1;
    memset(parser, 0, sizeof(*parser));
    parser->cursor = source;
    int root = lisp_parse_expression(parser);
    lisp_skip_space(parser);
    if (root < 0 || parser->failed || *parser->cursor) return -1;
    return root;
}

static int lisp_is_operator(const char* name) {
    static const char* operators[] = {
        "begin", "if", "print", "concat", "+", "-", "*", "=", "!=", "<", ">", "<=", ">=",
        "mod", "not", "and", "or", "let", "var", "set!", "while", "key", "wait",
        "canvas", "cell", "draw", "uptime", "diskinfo", NULL
    };
    for (uint32_t index = 0; operators[index]; index++) {
        if (strcmp(name, operators[index]) == 0) return 1;
    }
    return 0;
}

static uint32_t lisp_argument_count(const lisp_parser_t* parser, int first) {
    uint32_t count = 0;
    int node = parser->nodes[first].next;
    while (node >= 0) {
        count++;
        node = parser->nodes[node].next;
    }
    return count;
}

static int lisp_validate_node(const lisp_parser_t* parser, int index) {
    const lisp_node_t* node = &parser->nodes[index];
    if (node->type == LISP_INTEGER || node->type == LISP_STRING) return 1;
    if (node->type == LISP_SYMBOL) return 1;
    if (node->type != LISP_LIST || node->child < 0) return 0;

    int operator_node = node->child;
    if (parser->nodes[operator_node].type != LISP_SYMBOL ||
        !lisp_is_operator(parser->nodes[operator_node].text)) return 0;
    const char* operator_name = parser->nodes[operator_node].text;
    uint32_t count = lisp_argument_count(parser, operator_node);
    if ((strcmp(operator_name, "begin") == 0 && count == 0) ||
        (strcmp(operator_name, "if") == 0 && (count < 2 || count > 3)) ||
        (strcmp(operator_name, "print") == 0 && count != 1) ||
        (strcmp(operator_name, "concat") == 0 && (count == 0 || count > LISP_MAX_ARGUMENTS)) ||
        ((strcmp(operator_name, "not") == 0 || strcmp(operator_name, "wait") == 0) && count != 1) ||
        ((strcmp(operator_name, "and") == 0 || strcmp(operator_name, "or") == 0) && count > LISP_MAX_ARGUMENTS) ||
        (strcmp(operator_name, "mod") == 0 && count != 2) ||
        ((strcmp(operator_name, "let") == 0 || strcmp(operator_name, "set!") == 0) && count != (strcmp(operator_name, "let") == 0 ? 3U : 2U)) ||
        (strcmp(operator_name, "var") == 0 && count != 2) ||
        (strcmp(operator_name, "while") == 0 && count < 2) ||
        (strcmp(operator_name, "canvas") == 0 && count != 2) ||
        (strcmp(operator_name, "cell") == 0 && count != 3) ||
        (strcmp(operator_name, "draw") == 0 && count != 0) ||
        ((strcmp(operator_name, "=") == 0 || strcmp(operator_name, "!=") == 0 || strcmp(operator_name, "<") == 0 || strcmp(operator_name, ">") == 0 || strcmp(operator_name, "<=") == 0 || strcmp(operator_name, ">=") == 0) && count != 2) ||
        ((strcmp(operator_name, "+") == 0 || strcmp(operator_name, "*") == 0) && (count < 2 || count > LISP_MAX_ARGUMENTS)) ||
        (strcmp(operator_name, "-") == 0 && (count == 0 || count > 2)) ||
        ((strcmp(operator_name, "uptime") == 0 || strcmp(operator_name, "diskinfo") == 0) && count != 0) ||
        (strcmp(operator_name, "key") == 0 && count > 1)) return 0;

    if (strcmp(operator_name, "let") == 0 || strcmp(operator_name, "var") == 0 || strcmp(operator_name, "set!") == 0) {
        int name_node = parser->nodes[operator_node].next;
        if (name_node < 0 || parser->nodes[name_node].type != LISP_SYMBOL) return 0;
        const char* name = parser->nodes[name_node].text;
        uint32_t name_length = strlen(name);
        if (name_length == 0 || name_length >= sizeof(((lisp_variable_t*)0)->name) ||
            name[0] < 'a' || name[0] > 'z') return 0;
        for (uint32_t index = 1; index < name_length; index++) {
            char value = name[index];
            if (!((value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '_' || value == '-')) return 0;
        }
        for (int child = parser->nodes[name_node].next; child >= 0; child = parser->nodes[child].next) {
            if (!lisp_validate_node(parser, child)) return 0;
        }
        return 1;
    }

    for (int child = parser->nodes[operator_node].next; child >= 0; child = parser->nodes[child].next) {
        if (!lisp_validate_node(parser, child)) return 0;
    }
    return 1;
}

int aos_lisp_validate(const char* source) {
    lisp_parser_t* parser = kmalloc(sizeof(lisp_parser_t));
    if (!parser) return 0;
    int root = lisp_parse_program(source, parser);
    int valid = root >= 0 && lisp_validate_node(parser, root);
    kfree(parser);
    return valid;
}

static int lisp_value_to_text(const lisp_value_t* value, char* output, uint32_t capacity) {
    if (value->type == LISP_STRING) {
        uint32_t length = strlen(value->text);
        if (length >= capacity) return 0;
        strcpy(output, value->text);
        return 1;
    }
    if (value->type == LISP_INTEGER) {
        char reverse[16];
        uint32_t length = 0;
        int64_t number = value->integer;
        int negative = number < 0;
        if (negative) number = -number;
        do {
            reverse[length++] = (char)('0' + (number % 10));
            number /= 10;
        } while (number && length < sizeof(reverse));
        if (negative) reverse[length++] = '-';
        if (length >= capacity) return 0;
        for (uint32_t index = 0; index < length; index++) output[index] = reverse[length - index - 1];
        output[length] = '\0';
        return 1;
    }
    return 0;
}

static int lisp_variable_set(lisp_eval_context_t* context, const char* name, const lisp_value_t* value) {
    for (uint32_t index = 0; index < context->variable_count; index++) {
        if (strcmp(context->variables[index].name, name) == 0) {
            context->variables[index].value = *value;
            return 1;
        }
    }
    if (context->variable_count >= LISP_MAX_VARIABLES) return 0;
    lisp_variable_t* variable = &context->variables[context->variable_count++];
    strncpy(variable->name, name, sizeof(variable->name) - 1);
    variable->name[sizeof(variable->name) - 1] = '\0';
    variable->value = *value;
    return 1;
}

static int lisp_variable_get(const lisp_eval_context_t* context, const char* name, lisp_value_t* value) {
    for (uint32_t index = 0; index < context->variable_count; index++) {
        if (strcmp(context->variables[index].name, name) == 0) {
            *value = context->variables[index].value;
            return 1;
        }
    }
    return 0;
}

static int lisp_value_truthy(const lisp_value_t* value) {
    if (value->type == LISP_INTEGER) return value->integer != 0;
    if (value->type == LISP_STRING) return value->text[0] != '\0';
    return 0;
}

static int lisp_eval_node(lisp_eval_context_t* context, int index, lisp_value_t* result) {
    if (++context->steps > LISP_MAX_STEPS) return 0;
    const lisp_parser_t* parser = context->parser;
    const lisp_node_t* node = &parser->nodes[index];
    result->type = LISP_NIL;
    result->integer = 0;
    result->text[0] = '\0';
    if (node->type == LISP_INTEGER) {
        result->type = LISP_INTEGER;
        result->integer = node->integer;
        return 1;
    }
    if (node->type == LISP_STRING) {
        result->type = LISP_STRING;
        strcpy(result->text, node->text);
        return 1;
    }
    if (node->type == LISP_SYMBOL) {
        if (strcmp(node->text, "arg") == 0) {
            result->type = LISP_STRING;
            strncpy(result->text, context->argument ? context->argument : "", sizeof(result->text) - 1);
            result->text[sizeof(result->text) - 1] = '\0';
            return 1;
        }
        if (strcmp(node->text, "true") == 0 || strcmp(node->text, "false") == 0) {
            result->type = LISP_INTEGER;
            result->integer = strcmp(node->text, "true") == 0;
            return 1;
        }
        return lisp_variable_get(context, node->text, result);
    }

    int operator_node = node->child;
    const char* operator_name = parser->nodes[operator_node].text;
    int first = parser->nodes[operator_node].next;
    if (strcmp(operator_name, "begin") == 0) {
        for (int item = first; item >= 0; item = parser->nodes[item].next) {
            if (!lisp_eval_node(context, item, result)) return 0;
        }
        return 1;
    }
    if (strcmp(operator_name, "if") == 0) {
        lisp_value_t condition;
        if (!lisp_eval_node(context, first, &condition)) return 0;
        int then_branch = parser->nodes[first].next;
        int else_branch = parser->nodes[then_branch].next;
        if (lisp_value_truthy(&condition)) return lisp_eval_node(context, then_branch, result);
        if (else_branch >= 0) return lisp_eval_node(context, else_branch, result);
        result->type = LISP_NIL;
        return 1;
    }
    if (strcmp(operator_name, "let") == 0 || strcmp(operator_name, "var") == 0 || strcmp(operator_name, "set!") == 0) {
        int name_node = first;
        int value_node = parser->nodes[name_node].next;
        lisp_value_t value;
        if (strcmp(operator_name, "set!") == 0 &&
            !lisp_variable_get(context, parser->nodes[name_node].text, &value)) return 0;
        if (!lisp_eval_node(context, value_node, &value) ||
            !lisp_variable_set(context, parser->nodes[name_node].text, &value)) return 0;
        if (strcmp(operator_name, "set!") == 0 || strcmp(operator_name, "var") == 0) {
            result->type = LISP_NIL;
            return 1;
        }
        int body_node = parser->nodes[value_node].next;
        return lisp_eval_node(context, body_node, result);
    }
    if (strcmp(operator_name, "while") == 0) {
        int condition_node = first;
        int body_node = parser->nodes[condition_node].next;
        for (uint32_t iteration = 0; iteration < LISP_MAX_STEPS; iteration++) {
            lisp_value_t condition;
            if (!lisp_eval_node(context, condition_node, &condition)) return 0;
            if (!lisp_value_truthy(&condition)) {
                result->type = LISP_NIL;
                return 1;
            }
            for (int expression = body_node; expression >= 0; expression = parser->nodes[expression].next) {
                if (!lisp_eval_node(context, expression, result)) return 0;
            }
            if (context->steps >= LISP_MAX_STEPS) return 0;
        }
        return 0;
    }
    if (strcmp(operator_name, "and") == 0 || strcmp(operator_name, "or") == 0) {
        int is_and = strcmp(operator_name, "and") == 0;
        int truth = is_and;
        for (int item = first; item >= 0; item = parser->nodes[item].next) {
            lisp_value_t value;
            if (!lisp_eval_node(context, item, &value)) return 0;
            int current = lisp_value_truthy(&value);
            if (is_and && !current) { truth = 0; break; }
            if (!is_and && current) { truth = 1; break; }
        }
        result->type = LISP_INTEGER;
        result->integer = truth;
        return 1;
    }
    if (strcmp(operator_name, "not") == 0) {
        if (!lisp_eval_node(context, first, result)) return 0;
        result->type = LISP_INTEGER;
        result->integer = !lisp_value_truthy(result);
        return 1;
    }
    if (strcmp(operator_name, "key") == 0) {
        result->type = LISP_INTEGER;
        result->integer = (unsigned char)kbd_get();
        if (result->integer == 0 && first >= 0) {
            lisp_value_t fallback;
            if (!lisp_eval_node(context, first, &fallback) || fallback.type != LISP_INTEGER) return 0;
            result->integer = fallback.integer;
        }
        return 1;
    }
    if (strcmp(operator_name, "wait") == 0) {
        lisp_value_t ticks;
        if (!lisp_eval_node(context, first, &ticks) || ticks.type != LISP_INTEGER ||
            ticks.integer < 1 || ticks.integer > 10) return 0;
        timer_wait((uint32_t)ticks.integer);
        result->type = LISP_NIL;
        return 1;
    }
    if (strcmp(operator_name, "canvas") == 0) {
        lisp_value_t width, height;
        if (!lisp_eval_node(context, first, &width) ||
            !lisp_eval_node(context, parser->nodes[first].next, &height) ||
            width.type != LISP_INTEGER || height.type != LISP_INTEGER ||
            width.integer < 1 || width.integer > (int32_t)LISP_CANVAS_WIDTH ||
            height.integer < 1 || height.integer > (int32_t)LISP_CANVAS_HEIGHT) return 0;
        lisp_canvas_width = (uint32_t)width.integer;
        lisp_canvas_height = (uint32_t)height.integer;
        for (uint32_t row = 0; row < lisp_canvas_height; row++)
            for (uint32_t column = 0; column < lisp_canvas_width; column++) lisp_canvas[row][column] = ' ';
        result->type = LISP_NIL;
        return 1;
    }
    if (strcmp(operator_name, "cell") == 0) {
        lisp_value_t x, y, value;
        int value_node = parser->nodes[first].next;
        if (!lisp_eval_node(context, first, &x) ||
            !lisp_eval_node(context, value_node, &y) ||
            !lisp_eval_node(context, parser->nodes[value_node].next, &value) ||
            x.type != LISP_INTEGER || y.type != LISP_INTEGER ||
            x.integer < 0 || y.integer < 0 || (uint32_t)x.integer >= lisp_canvas_width ||
            (uint32_t)y.integer >= lisp_canvas_height) return 0;
        char pixel;
        if (value.type == LISP_INTEGER && value.integer >= 32 && value.integer <= 126) pixel = (char)value.integer;
        else if (value.type == LISP_STRING && strlen(value.text) == 1) pixel = value.text[0];
        else return 0;
        lisp_canvas[y.integer][x.integer] = pixel;
        result->type = LISP_NIL;
        return 1;
    }
    if (strcmp(operator_name, "draw") == 0) {
        char frame[LISP_CANVAS_HEIGHT * (LISP_CANVAS_WIDTH + 1U) + 1U];
        uint32_t offset = 0;
        for (uint32_t row = 0; row < lisp_canvas_height; row++) {
            memcpy(frame + offset, lisp_canvas[row], lisp_canvas_width);
            offset += lisp_canvas_width;
            frame[offset++] = '\n';
        }
        frame[offset] = '\0';
        screen_show_text(frame);
        result->type = LISP_NIL;
        return 1;
    }
    if (strcmp(operator_name, "print") == 0) {
        lisp_value_t value;
        char text[LISP_TEXT_SIZE];
        if (!lisp_eval_node(context, first, &value)) return 0;
        if (value.type != LISP_INTEGER && value.type != LISP_STRING) return 0;
        if (!lisp_value_to_text(&value, text, sizeof(text))) return 0;
        print_string(text);
        print_char('\n');
        return 1;
    }
    if (strcmp(operator_name, "concat") == 0) {
        char output[LISP_TEXT_SIZE];
        uint32_t length = 0;
        output[0] = '\0';
        for (int item = first; item >= 0; item = parser->nodes[item].next) {
            lisp_value_t value;
            char piece[LISP_TEXT_SIZE];
            if (!lisp_eval_node(context, item, &value) ||
                !lisp_value_to_text(&value, piece, sizeof(piece))) return 0;
            uint32_t piece_length = strlen(piece);
            if (length + piece_length >= sizeof(output)) return 0;
            memcpy(output + length, piece, piece_length + 1);
            length += piece_length;
        }
        result->type = LISP_STRING;
        strcpy(result->text, output);
        return 1;
    }
    if (strcmp(operator_name, "uptime") == 0) {
        uint32_t hours, minutes, seconds;
        timer_get_uptime(&hours, &minutes, &seconds);
        print_string("Uptime: "); kprint_dec(hours); print_string("h ");
        kprint_dec(minutes); print_string("m "); kprint_dec(seconds); print_string("s\n");
        result->type = LISP_INTEGER;
        result->integer = (int32_t)(hours * 3600U + minutes * 60U + seconds);
        return 1;
    }
    if (strcmp(operator_name, "diskinfo") == 0) {
        char label[13];
        fat32_get_label(label);
        print_string("Storage: ");
        if (ata_is_ramdisk()) print_string("RAM-backed\n");
        else if (ata_is_ahci()) print_string("AHCI\n");
        else print_string("ATA/IDE PIO\n");
        print_string("Sectors: "); kprint_dec(ata_get_sector_count());
        print_string("\nFAT32 volume label: "); print_string(label[0] ? label : "(none)"); print_char('\n');
        result->type = LISP_INTEGER;
        result->integer = (int32_t)ata_get_sector_count();
        return 1;
    }

    lisp_value_t values[LISP_MAX_ARGUMENTS];
    uint32_t count = 0;
    for (int item = first; item >= 0; item = parser->nodes[item].next) {
        if (count >= LISP_MAX_ARGUMENTS || !lisp_eval_node(context, item, &values[count])) return 0;
        count++;
    }
    if (strcmp(operator_name, "=") == 0) {
        int equal = values[0].type == values[1].type;
        if (equal && values[0].type == LISP_INTEGER) equal = values[0].integer == values[1].integer;
        else if (equal && values[0].type == LISP_STRING) equal = strcmp(values[0].text, values[1].text) == 0;
        else equal = 0;
        result->type = LISP_INTEGER;
        result->integer = equal;
        return 1;
    }
    if (strcmp(operator_name, "<") == 0 || strcmp(operator_name, ">") == 0 ||
        strcmp(operator_name, "<=") == 0 || strcmp(operator_name, ">=") == 0 ||
        strcmp(operator_name, "!=") == 0) {
        if (values[0].type != LISP_INTEGER || values[1].type != LISP_INTEGER) return 0;
        result->type = LISP_INTEGER;
        if (strcmp(operator_name, "<") == 0) result->integer = values[0].integer < values[1].integer;
        else if (strcmp(operator_name, ">") == 0) result->integer = values[0].integer > values[1].integer;
        else if (strcmp(operator_name, "<=") == 0) result->integer = values[0].integer <= values[1].integer;
        else if (strcmp(operator_name, ">=") == 0) result->integer = values[0].integer >= values[1].integer;
        else result->integer = values[0].integer != values[1].integer;
        return 1;
    }
    if (strcmp(operator_name, "mod") == 0) {
        if (values[0].type != LISP_INTEGER || values[1].type != LISP_INTEGER || values[1].integer == 0) return 0;
        result->type = LISP_INTEGER;
        result->integer = values[0].integer % values[1].integer;
        return 1;
    }
    if (strcmp(operator_name, "+") == 0 || strcmp(operator_name, "-") == 0 || strcmp(operator_name, "*") == 0) {
        int64_t total;
        if (values[0].type != LISP_INTEGER) return 0;
        total = values[0].integer;
        if (strcmp(operator_name, "-") == 0 && count == 1) total = -total;
        else for (uint32_t value_index = 1; value_index < count; value_index++) {
            if (values[value_index].type != LISP_INTEGER) return 0;
            if (strcmp(operator_name, "+") == 0) total += values[value_index].integer;
            else if (strcmp(operator_name, "-") == 0) total -= values[value_index].integer;
            else total *= values[value_index].integer;
            if (total > 2147483647LL || total < -2147483648LL) return 0;
        }
        if (total > 2147483647LL || total < -2147483648LL) return 0;
        result->type = LISP_INTEGER;
        result->integer = (int32_t)total;
        return 1;
    }
    return 0;
}

static int lisp_run(const char* source, const char* argument, int print_result) {
    lisp_parser_t* parser = kmalloc(sizeof(lisp_parser_t));
    if (!parser) return -1;
    int root = lisp_parse_program(source, parser);
    if (root < 0 || !lisp_validate_node(parser, root)) {
        kfree(parser);
        return -1;
    }
    lisp_value_t result;
    lisp_eval_context_t context;
    memset(&context, 0, sizeof(context));
    context.parser = parser;
    context.argument = argument;
    int ok = lisp_eval_node(&context, root, &result);
    if (ok && print_result) {
        if (result.type == LISP_INTEGER) { kprint_dec((uint32_t)result.integer); print_char('\n'); }
        else if (result.type == LISP_STRING) { print_string(result.text); print_char('\n'); }
    }
    kfree(parser);
    return ok ? 0 : -1;
}

int aos_lisp_execute(const char* source, const char* argument) {
    int result = lisp_run(source, argument, 1);
    if (result != 0) print_string("Lisp runtime error: invalid or failed expression.\n");
    return result;
}