/*
 * sexp_parser.c - S-expression text to binary parser
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include "sexp.h"

#define SMALL_LIST_MAX 4
#define LARGE_LIST_THRESHOLD 5

/* Initial buffer sizes */
#define INITIAL_OUTPUT_SIZE  256
#define INITIAL_SYMTAB_SIZE  16
#define INITIAL_HASH_SIZE    32

/* FNV-1a hash constants */
#define FNV_OFFSET 2166136261u
#define FNV_PRIME  16777619u

static uint32_t hash_bytes(const char *data, int len) {
    uint32_t hash = FNV_OFFSET;
    for (int i = 0; i < len; i++) {
        hash ^= (uint8_t)data[i];
        hash *= FNV_PRIME;
    }
    return hash;
}

/* Symbol table operations */
static int symtab_init(SexpSymbolTable *st) {
    st->capacity = INITIAL_SYMTAB_SIZE;
    st->count = 0;
    st->hash_size = INITIAL_HASH_SIZE;
    
    st->symbols = malloc(st->capacity * sizeof(const char*));
    st->lengths = malloc(st->capacity * sizeof(int));
    st->hashes = malloc(st->capacity * sizeof(uint32_t));
    st->hash_table = malloc(st->hash_size * sizeof(int));
    
    if (!st->symbols || !st->lengths || !st->hashes || !st->hash_table) {
        return -1;
    }
    
    for (int i = 0; i < st->hash_size; i++) {
        st->hash_table[i] = -1;
    }
    return 0;
}

static void symtab_free(SexpSymbolTable *st) {
    /* Symbols are pointers into input, no need to free them individually */
    free(st->symbols);
    free(st->lengths);
    free(st->hashes);
    free(st->hash_table);
    memset(st, 0, sizeof(*st));
}

static int symtab_rehash(SexpSymbolTable *st) {
    int new_size = st->hash_size * 2;
    int *new_table = malloc(new_size * sizeof(int));
    if (!new_table) return -1;
    
    for (int i = 0; i < new_size; i++) {
        new_table[i] = -1;
    }
    
    for (int i = 0; i < st->count; i++) {
        uint32_t hash = st->hashes[i];
        int slot = hash & (new_size - 1);
        while (new_table[slot] != -1) {
            slot = (slot + 1) & (new_size - 1);
        }
        new_table[slot] = i;
    }
    
    free(st->hash_table);
    st->hash_table = new_table;
    st->hash_size = new_size;
    return 0;
}

/* Intern a symbol, returns index */
static int symtab_intern(SexpSymbolTable *st, const char *sym, int len) {
    uint32_t hash = hash_bytes(sym, len);
    int slot = hash & (st->hash_size - 1);
    
    /* Search for existing */
    while (st->hash_table[slot] != -1) {
        int idx = st->hash_table[slot];
        if (st->hashes[idx] == hash && st->lengths[idx] == len &&
            memcmp(st->symbols[idx], sym, len) == 0) {
            return idx;
        }
        slot = (slot + 1) & (st->hash_size - 1);
    }
    
    /* Need to add new symbol */
    if (st->count >= st->capacity) {
        int new_cap = st->capacity * 2;
        const char **new_syms = realloc(st->symbols, new_cap * sizeof(const char*));
        int *new_lens = realloc(st->lengths, new_cap * sizeof(int));
        uint32_t *new_hashes = realloc(st->hashes, new_cap * sizeof(uint32_t));
        if (!new_syms || !new_lens || !new_hashes) return -1;
        st->symbols = new_syms;
        st->lengths = new_lens;
        st->hashes = new_hashes;
        st->capacity = new_cap;
    }
    
    /* Check if hash table needs rehashing */
    if (st->count * 2 >= st->hash_size) {
        if (symtab_rehash(st) < 0) return -1;
        /* Recalculate slot after rehash */
        slot = hash & (st->hash_size - 1);
        while (st->hash_table[slot] != -1) {
            slot = (slot + 1) & (st->hash_size - 1);
        }
    }
    
    /* Add the symbol - zero-copy: just store pointer into input */
    int idx = st->count;
    st->symbols[idx] = sym;  /* Points into input, valid during parsing */
    st->lengths[idx] = len;
    st->hashes[idx] = hash;
    st->hash_table[slot] = idx;
    st->count++;
    
    return idx;
}

/* Output buffer operations */
static int output_ensure(SexpParseState *st, size_t need) {
    if (st->output_len + need <= st->output_cap) return 0;
    
    size_t new_cap = st->output_cap * 2;
    while (new_cap < st->output_len + need) new_cap *= 2;
    
    uint8_t *new_buf = realloc(st->output, new_cap);
    if (!new_buf) return -1;
    st->output = new_buf;
    st->output_cap = new_cap;
    return 0;
}

static int output_byte(SexpParseState *st, uint8_t b) {
    if (output_ensure(st, 1) < 0) return -1;
    st->output[st->output_len++] = b;
    return 0;
}

static int output_bytes(SexpParseState *st, const uint8_t *data, size_t len) {
    if (output_ensure(st, len) < 0) return -1;
    memcpy(st->output + st->output_len, data, len);
    st->output_len += len;
    return 0;
}

static int output_varint(SexpParseState *st, uint64_t value) {
    uint8_t buf[10];
    int n = sexp_encode_varint(buf, sizeof(buf), value);
    if (n == 0) return -1;
    return output_bytes(st, buf, n);
}

static int output_uint32(SexpParseState *st, uint32_t value) {
    if (output_ensure(st, 4) < 0) return -1;
    /* Little-endian */
    st->output[st->output_len++] = value & 0xFF;
    st->output[st->output_len++] = (value >> 8) & 0xFF;
    st->output[st->output_len++] = (value >> 16) & 0xFF;
    st->output[st->output_len++] = (value >> 24) & 0xFF;
    return 0;
}

static int output_double(SexpParseState *st, double value) {
    if (output_ensure(st, 8) < 0) return -1;
    uint8_t *p = st->output + st->output_len;
    memcpy(p, &value, 8);
    st->output_len += 8;
    return 0;
}

/* Parser helpers */
static void skip_whitespace(SexpParseState *st) {
    while (st->pos < st->end) {
        char c = *st->pos;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            st->pos++;
        } else if (c == ';') {
            /* Comment - skip to end of line */
            while (st->pos < st->end && *st->pos != '\n') st->pos++;
        } else {
            break;
        }
    }
}

static void set_error(SexpParseState *st, SexpError err, const char *msg) {
    st->error = err;
    st->error_pos = st->pos;
    snprintf(st->error_msg, sizeof(st->error_msg), "%s", msg);
}

static int is_symbol_char(char c) {
    if (isalnum((unsigned char)c)) return 1;
    switch (c) {
        case '_': case '-': case '+': case '*': case '/':
        case '<': case '>': case '=': case '!': case '?':
        case '@': case '#': case '$': case '%': case '^':
        case '&': case '~': case '.': case ':':
            return 1;
    }
    return 0;
}

typedef struct {
    const char *sym_start;
    int         sym_len;
    int         inlineable;
} PendingInline;

static int parse_list(SexpParseState *st);
static int parse_string(SexpParseState *st);
static int parse_number_or_symbol(SexpParseState *st, int inlineable_hint);
static int parse_value_dispatch(SexpParseState *st, PendingInline *hint);
static int parse_value(SexpParseState *st);



static int parse_value(SexpParseState *st) {
    return parse_value_dispatch(st, NULL);
}

static int parse_value_dispatch(SexpParseState *st, PendingInline *hint) {
    skip_whitespace(st);
    
    if (st->pos >= st->end) {
        set_error(st, SEXP_ERR_SYNTAX, "Unexpected end of input");
        return -1;
    }
    
    char c = *st->pos;
    
    if (c == '(') {
        return parse_list(st);
    } else if (c == '"') {
        return parse_string(st);
    } else {
        return parse_number_or_symbol(st, hint ? hint->inlineable : 0);
    }
}

/* Public API */

void sexp_parse_init(SexpParseState *state, const char *input, size_t len) {
    memset(state, 0, sizeof(*state));
    state->input = input;
    state->pos = input;
    state->end = input + len;
    state->depth = 0;
    state->error = SEXP_OK;
    
    symtab_init(&state->symtab);
    
    state->output_cap = INITIAL_OUTPUT_SIZE;
    state->output = malloc(state->output_cap);
    state->output_len = 0;
}

void sexp_parse_free(SexpParseState *state) {
    symtab_free(&state->symtab);
    free(state->output);
    memset(state, 0, sizeof(*state));
}

uint8_t *sexp_parse(SexpParseState *state, size_t *out_len) {
    /* Parse the value first (this populates the symbol table) */
    if (parse_value(state) < 0) {
        return NULL;
    }
    
    skip_whitespace(state);
    if (state->pos < state->end) {
        set_error(state, SEXP_ERR_SYNTAX, "Unexpected data after expression");
        return NULL;
    }
    
    /* Now build the final output with header */
    size_t value_len = state->output_len;
    uint8_t *value_data = state->output;
    
    /* Calculate symbol table size */
    size_t symtab_size = 0;
    uint8_t symcount_buf[10];
    int symcount_len = sexp_encode_varint(symcount_buf, sizeof(symcount_buf), 
                                           state->symtab.count);
    symtab_size += symcount_len;
    
    for (int i = 0; i < state->symtab.count; i++) {
        uint8_t len_buf[10];
        int len_n = sexp_encode_varint(len_buf, sizeof(len_buf), state->symtab.lengths[i]);
        symtab_size += len_n + state->symtab.lengths[i];
    }
    
    /* Allocate final buffer */
    size_t total_size = 1 + symtab_size + value_len;  /* version + symtab + value */
    uint8_t *result = malloc(total_size);
    if (!result) {
        set_error(state, SEXP_ERR_MEMORY, "Out of memory");
        return NULL;
    }
    
    /* Write version */
    size_t pos = 0;
    result[pos++] = SEXP_FORMAT_VERSION;
    
    /* Write symbol count */
    memcpy(result + pos, symcount_buf, symcount_len);
    pos += symcount_len;
    
    /* Write symbol table */
    for (int i = 0; i < state->symtab.count; i++) {
        uint8_t len_buf[10];
        int len_n = sexp_encode_varint(len_buf, sizeof(len_buf), state->symtab.lengths[i]);
        memcpy(result + pos, len_buf, len_n);
        pos += len_n;
        memcpy(result + pos, state->symtab.symbols[i], state->symtab.lengths[i]);
        pos += state->symtab.lengths[i];
    }
    
    /* Write value data */
    memcpy(result + pos, value_data, value_len);
    pos += value_len;
    
    *out_len = pos;
    return result;
}

/*
 * parse_string - Parse a quoted string literal
 * Input: position at opening quote
 * Output: string element written to output buffer
 */
static int parse_string(SexpParseState *st) {
    if (*st->pos != '"') {
        set_error(st, SEXP_ERR_SYNTAX, "Expected '\"'");
        return -1;
    }
    st->pos++;  /* Skip opening quote */
    
    /* First pass: compute decoded length and validate */
    const char *start = st->pos;
    size_t decoded_len = 0;
    const char *p = start;
    
    while (p < st->end && *p != '"') {
        if (*p == '\\') {
            p++;
            if (p >= st->end) {
                set_error(st, SEXP_ERR_SYNTAX, "Unterminated escape sequence");
                return -1;
            }
            /* Handle escape */
            switch (*p) {
                case 'n': case 't': case 'r': case '\\': case '"':
                    decoded_len++;
                    p++;
                    break;
                default:
                    set_error(st, SEXP_ERR_SYNTAX, "Invalid escape sequence");
                    return -1;
            }
        } else {
            decoded_len++;
            p++;
        }
    }
    
    if (p >= st->end) {
        set_error(st, SEXP_ERR_SYNTAX, "Unterminated string");
        return -1;
    }
    
    /* Check size limit */
    if (decoded_len > SEXP_MAX_STRING) {
        set_error(st, SEXP_ERR_OVERFLOW, "String too long");
        return -1;
    }
    
    /* Allocate space for decoded string */
    uint8_t *decoded = malloc(decoded_len);
    if (!decoded) {
        set_error(st, SEXP_ERR_MEMORY, "Out of memory");
        return -1;
    }
    
    /* Second pass: decode string */
    p = start;
    size_t di = 0;
    while (p < st->end && *p != '"') {
        if (*p == '\\') {
            p++;
            switch (*p) {
                case 'n':  decoded[di++] = '\n'; break;
                case 't':  decoded[di++] = '\t'; break;
                case 'r':  decoded[di++] = '\r'; break;
                case '\\': decoded[di++] = '\\'; break;
                case '"':  decoded[di++] = '"';  break;
                default: break; /* Already validated */
            }
            p++;
        } else {
            decoded[di++] = (uint8_t)*p++;
        }
    }
    
    st->pos = p + 1;  /* Skip closing quote */
    
    /* Write to output */
    int result;
    if (decoded_len <= SEXP_SHORTSTR_MAX) {
        /* Short string: tag with length in lower bits + data */
        result = output_byte(st, SEXP_TAG_SHORTSTR | (uint8_t)decoded_len);
        if (result == 0) {
            result = output_bytes(st, decoded, decoded_len);
        }
    } else {
        /* Long string: tag + varint length + data */
        result = output_byte(st, SEXP_TAG_LONGSTR);
        if (result == 0) {
            result = output_varint(st, decoded_len);
        }
        if (result == 0) {
            result = output_bytes(st, decoded, decoded_len);
        }
    }
    
    free(decoded);
    return result;
}

/*
 * parse_number_or_symbol - Parse a number or symbol
 * Numbers: optional sign, digits, optional decimal/exponent
 * Symbols: identifiers with symbol characters
 */
static int parse_number_or_symbol(SexpParseState *st, int inlineable_hint) {
    (void)inlineable_hint;  /* Reserved for future inline symbol optimization */
    
    const char *start = st->pos;
    
    /* Check for 'nil' */
    if (st->end - st->pos >= 3 && 
        st->pos[0] == 'n' && st->pos[1] == 'i' && st->pos[2] == 'l') {
        /* Check it's not part of a longer symbol */
        if (st->end - st->pos == 3 || !is_symbol_char(st->pos[3])) {
            st->pos += 3;
            return output_byte(st, SEXP_TAG_NIL);
        }
    }
    
    /* Collect token characters */
    while (st->pos < st->end && is_symbol_char(*st->pos)) {
        st->pos++;
    }
    
    int len = (int)(st->pos - start);
    if (len == 0) {
        set_error(st, SEXP_ERR_SYNTAX, "Expected value");
        return -1;
    }
    
    /* Try to parse as number */
    const char *p = start;
    int has_digits = 0;
    int has_dot = 0;
    int has_exp = 0;
    
    /* Optional sign */
    if (*p == '-' || *p == '+') {
        p++;
    }
    
    /* Digits before decimal */
    while (p < st->pos && isdigit((unsigned char)*p)) {
        has_digits = 1;
        p++;
    }
    
    /* Optional decimal part */
    if (p < st->pos && *p == '.') {
        has_dot = 1;
        p++;
        while (p < st->pos && isdigit((unsigned char)*p)) {
            has_digits = 1;
            p++;
        }
    }
    
    /* Optional exponent */
    if (p < st->pos && (*p == 'e' || *p == 'E')) {
        has_exp = 1;
        p++;
        if (p < st->pos && (*p == '+' || *p == '-')) p++;
        while (p < st->pos && isdigit((unsigned char)*p)) p++;
    }
    
    /* If we consumed the whole token and it has digits, it's a number */
    if (p == st->pos && has_digits) {
        /* Use stack buffer for number string (numbers rarely exceed 32 chars) */
        char stack_buf[64];
        char *tmp;
        int need_free = 0;
        
        if (len < (int)sizeof(stack_buf)) {
            tmp = stack_buf;
        } else {
            tmp = malloc(len + 1);
            if (!tmp) {
                set_error(st, SEXP_ERR_MEMORY, "Out of memory");
                return -1;
            }
            need_free = 1;
        }
        memcpy(tmp, start, len);
        tmp[len] = '\0';
        
        if (has_dot || has_exp) {
            /* Float */
            char *endptr;
            errno = 0;
            double d = strtod(tmp, &endptr);
            if (need_free) free(tmp);
            
            if (errno == ERANGE) {
                set_error(st, SEXP_ERR_OVERFLOW, "Float overflow");
                return -1;
            }
            
            if (output_byte(st, SEXP_TAG_FLOAT) < 0) return -1;
            return output_double(st, d);
        } else {
            /* Integer */
            char *endptr;
            errno = 0;
            long long val = strtoll(tmp, &endptr, 10);
            if (need_free) free(tmp);
            
            if (errno == ERANGE) {
                set_error(st, SEXP_ERR_OVERFLOW, "Integer overflow");
                return -1;
            }
            
            /* Check for small integer encoding */
            if (val >= SEXP_SMALLINT_MIN && val <= SEXP_SMALLINT_MAX) {
                return output_byte(st, SEXP_TAG_SMALLINT | ((int)val + SEXP_SMALLINT_BIAS));
            } else {
                /* Large integer with zigzag encoding */
                if (output_byte(st, SEXP_TAG_LARGEINT) < 0) return -1;
                return output_varint(st, sexp_zigzag_encode(val));
            }
        }
    }
    
    /* It's a symbol - intern it */
    int sym_idx = symtab_intern(&st->symtab, start, len);
    if (sym_idx < 0) {
        set_error(st, SEXP_ERR_MEMORY, "Failed to intern symbol");
        return -1;
    }
    
    /* Output symbol reference */
    if (output_byte(st, SEXP_TAG_SYMBOL) < 0) return -1;
    return output_varint(st, sym_idx);
}

/*
 * parse_list - Parse a parenthesized list
 * Input: position at opening paren
 * Output: list element written to output buffer
 */
static int parse_list(SexpParseState *st) {
    if (*st->pos != '(') {
        set_error(st, SEXP_ERR_SYNTAX, "Expected '('");
        return -1;
    }
    st->pos++;  /* Skip opening paren */
    
    /* Check depth limit */
    if (st->depth >= SEXP_MAX_DEPTH) {
        set_error(st, SEXP_ERR_DEPTH, "Maximum nesting depth exceeded");
        return -1;
    }
    st->depth++;
    
    skip_whitespace(st);
    
    /* Check for empty list (nil) */
    if (st->pos < st->end && *st->pos == ')') {
        st->pos++;
        st->depth--;
        return output_byte(st, SEXP_TAG_NIL);
    }
    
    /* Parse elements into a temporary buffer */
    /* Save current output position */
    size_t list_start = st->output_len;
    
    /* Count elements and collect their offsets (for large lists) */
    size_t elem_offsets[256];  /* Stack-allocated for small lists */
    size_t *offsets = elem_offsets;
    size_t offsets_cap = 256;
    int elem_count = 0;
    
    while (st->pos < st->end) {
        skip_whitespace(st);
        
        if (st->pos >= st->end) {
            set_error(st, SEXP_ERR_SYNTAX, "Unterminated list");
            if (offsets != elem_offsets) free(offsets);
            return -1;
        }
        
        if (*st->pos == ')') {
            st->pos++;
            break;
        }
        
        /* Record offset of this element */
        if ((size_t)elem_count >= offsets_cap) {
            size_t new_cap = offsets_cap * 2;
            size_t *new_offsets;
            if (offsets == elem_offsets) {
                new_offsets = malloc(new_cap * sizeof(size_t));
                if (new_offsets) memcpy(new_offsets, elem_offsets, elem_count * sizeof(size_t));
            } else {
                new_offsets = realloc(offsets, new_cap * sizeof(size_t));
            }
            if (!new_offsets) {
                set_error(st, SEXP_ERR_MEMORY, "Out of memory");
                if (offsets != elem_offsets) free(offsets);
                return -1;
            }
            offsets = new_offsets;
            offsets_cap = new_cap;
        }
        offsets[elem_count] = st->output_len - list_start;
        
        /* Parse the element */
        if (parse_value(st) < 0) {
            if (offsets != elem_offsets) free(offsets);
            return -1;
        }
        
        elem_count++;
    }
    
    st->depth--;
    
    /* Now we need to reformat the output as a proper list */
    size_t elem_data_len = st->output_len - list_start;
    
    /* Copy element data to temporary buffer - use stack for small lists */
    #define ELEM_DATA_STACK_MAX 512
    uint8_t elem_data_stack[ELEM_DATA_STACK_MAX];
    uint8_t *elem_data;
    int elem_data_heap = 0;
    
    if (elem_data_len <= ELEM_DATA_STACK_MAX) {
        elem_data = elem_data_stack;
    } else {
        elem_data = malloc(elem_data_len);
        if (!elem_data) {
            set_error(st, SEXP_ERR_MEMORY, "Out of memory");
            if (offsets != elem_offsets) free(offsets);
            return -1;
        }
        elem_data_heap = 1;
    }
    memcpy(elem_data, st->output + list_start, elem_data_len);
    
    /* Reset output position to list start */
    st->output_len = list_start;
    
    if (elem_count == 0) {
        /* Empty list - already handled above, but just in case */
        if (elem_data_heap) free(elem_data);
        if (offsets != elem_offsets) free(offsets);
        return output_byte(st, SEXP_TAG_NIL);
    } else if (elem_count <= SEXP_SMALLLIST_MAX) {
        /* Small list: check if we should add key index */
        /* Build key index: find sublists that start with symbols */
        typedef struct { int sym_idx; int elem_idx; } KeyEntry;
        #define KEY_INDEX_STACK_MAX 32
        KeyEntry key_index_stack[KEY_INDEX_STACK_MAX];
        KeyEntry *key_index = key_index_stack;
        int key_count = 0;
        int key_cap = KEY_INDEX_STACK_MAX;
        int key_heap = 0;
        
        for (int i = 0; i < elem_count; i++) {
            uint8_t elem_tag = elem_data[offsets[i]] & SEXP_TAG_MASK;
            if (elem_tag == SEXP_TAG_LIST) {
                /* Check if this sublist starts with a symbol */
                size_t sublist_offset = offsets[i];
                uint8_t sub_tag_byte = elem_data[sublist_offset];
                int sub_count = sub_tag_byte & SEXP_VALUE_MASK;
                
                if (sub_count > 0 && sub_count <= SEXP_SMALLLIST_MAX) {
                    /* Small sublist - skip payload size varint to get to first elem */
                    size_t pos = sublist_offset + 1;
                    uint64_t payload_size = 0;
                    int n = sexp_decode_varint_fast(elem_data + pos, elem_data_len - pos, &payload_size);
                    if (n > 0) {
                        pos += n;
                        /* Check first element */
                        if (pos < elem_data_len && 
                            (elem_data[pos] & SEXP_TAG_MASK) == SEXP_TAG_SYMBOL) {
                            /* Found a keyed sublist */
                            uint64_t sym_idx = 0;
                            sexp_decode_varint_fast(elem_data + pos + 1, 
                                                    elem_data_len - pos - 1, &sym_idx);
                            
                            /* Add to key index */
                            if (key_count >= key_cap) {
                                int new_cap = key_cap * 2;
                                KeyEntry *new_idx;
                                if (key_heap) {
                                    new_idx = realloc(key_index, new_cap * sizeof(KeyEntry));
                                } else {
                                    new_idx = malloc(new_cap * sizeof(KeyEntry));
                                    if (new_idx) memcpy(new_idx, key_index_stack, 
                                                        key_count * sizeof(KeyEntry));
                                    key_heap = 1;
                                }
                                if (!new_idx) goto skip_key_index;
                                key_index = new_idx;
                                key_cap = new_cap;
                            }
                            key_index[key_count].sym_idx = (int)sym_idx;
                            key_index[key_count].elem_idx = i;
                            key_count++;
                        }
                    }
                }
            }
        }
        
        if (key_count > 0) {
            /* Emit list with key index */
            /* Calculate key index size */
            uint8_t key_idx_buf[512];
            size_t key_idx_len = 0;
            
            /* num_keys varint */
            key_idx_len += sexp_encode_varint(key_idx_buf + key_idx_len, 
                                               sizeof(key_idx_buf) - key_idx_len, key_count);
            /* key entries */
            for (int k = 0; k < key_count; k++) {
                key_idx_len += sexp_encode_varint(key_idx_buf + key_idx_len,
                                                   sizeof(key_idx_buf) - key_idx_len,
                                                   key_index[k].sym_idx);
                key_idx_len += sexp_encode_varint(key_idx_buf + key_idx_len,
                                                   sizeof(key_idx_buf) - key_idx_len,
                                                   key_index[k].elem_idx);
            }
            
            /* Tag with HAS_KEYIDX flag */
            if (output_byte(st, SEXP_TAG_LIST | SEXP_LIST_HAS_KEYIDX | elem_count) < 0) {
                if (key_heap) free(key_index);
                goto fail;
            }
            /* Total payload = key_index + elem_data */
            if (output_varint(st, key_idx_len + elem_data_len) < 0) {
                if (key_heap) free(key_index);
                goto fail;
            }
            /* Key index */
            if (output_bytes(st, key_idx_buf, key_idx_len) < 0) {
                if (key_heap) free(key_index);
                goto fail;
            }
            /* Element data */
            if (output_bytes(st, elem_data, elem_data_len) < 0) {
                if (key_heap) free(key_index);
                goto fail;
            }
            
            if (key_heap) free(key_index);
        } else {
skip_key_index:
            if (key_heap) free(key_index);
            /* No key index, emit standard small list */
            if (output_byte(st, SEXP_TAG_LIST | elem_count) < 0) goto fail;
            if (output_varint(st, elem_data_len) < 0) goto fail;
            if (output_bytes(st, elem_data, elem_data_len) < 0) goto fail;
        }
    } else {
        /* Large list: tag(0) + uint32 count + SEntry table + elements */
        if (output_byte(st, SEXP_TAG_LIST) < 0) goto fail;
        if (output_uint32(st, elem_count) < 0) goto fail;
        
        /* Write SEntry table */
        for (int i = 0; i < elem_count; i++) {
            /* Get type tag from element data */
            uint8_t elem_tag = (elem_data[offsets[i]] & SEXP_TAG_MASK) >> 5;
            uint32_t offset = (uint32_t)offsets[i];
            uint32_t sentry = SENTRY_MAKE(elem_tag, offset);
            if (output_uint32(st, sentry) < 0) goto fail;
        }
        
        /* Write element data */
        if (output_bytes(st, elem_data, elem_data_len) < 0) goto fail;
    }
    
    if (elem_data_heap) free(elem_data);
    if (offsets != elem_offsets) free(offsets);
    return 0;
    
fail:
    if (elem_data_heap) free(elem_data);
    if (offsets != elem_offsets) free(offsets);
    return -1;
}
