/*
 * sexp_io.c - S-expression binary to text conversion
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "sexp.h"

/* String buffer for building output */
typedef struct {
    char   *data;
    size_t  len;
    size_t  cap;
} StrBuf;

static int strbuf_init(StrBuf *sb, size_t initial) {
    sb->cap = initial;
    sb->len = 0;
    sb->data = malloc(initial);
    return sb->data ? 0 : -1;
}

static void strbuf_free(StrBuf *sb) {
    free(sb->data);
    sb->data = NULL;
    sb->len = sb->cap = 0;
}

static int strbuf_ensure(StrBuf *sb, size_t need) {
    if (sb->len + need <= sb->cap) return 0;
    size_t new_cap = sb->cap * 2;
    while (new_cap < sb->len + need) new_cap *= 2;
    char *new_data = realloc(sb->data, new_cap);
    if (!new_data) return -1;
    sb->data = new_data;
    sb->cap = new_cap;
    return 0;
}

static int strbuf_append(StrBuf *sb, const char *str, size_t len) {
    if (strbuf_ensure(sb, len) < 0) return -1;
    memcpy(sb->data + sb->len, str, len);
    sb->len += len;
    return 0;
}

static int strbuf_append_char(StrBuf *sb, char c) {
    if (strbuf_ensure(sb, 1) < 0) return -1;
    sb->data[sb->len++] = c;
    return 0;
}

__attribute__((format(printf, 2, 3)))
static int strbuf_printf(StrBuf *sb, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    
    /* First, try with remaining space */
    size_t avail = sb->cap - sb->len;
    int n = vsnprintf(sb->data + sb->len, avail, fmt, args);
    va_end(args);
    
    if (n < 0) return -1;
    
    if ((size_t)n >= avail) {
        /* Need more space */
        if (strbuf_ensure(sb, n + 1) < 0) return -1;
        va_start(args, fmt);
        n = vsnprintf(sb->data + sb->len, sb->cap - sb->len, fmt, args);
        va_end(args);
    }
    
    sb->len += n;
    return 0;
}

/* Read helpers */
static uint32_t read_uint32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | 
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static double read_double(const uint8_t *p) {
    double d;
    memcpy(&d, p, 8);
    return d;
}

/* Symbol table reader - uses stack for small counts, heap for large */
#define SYMREADER_STACK_MAX 16

typedef struct {
    const char **symbols;
    int         *lengths;
    int          count;
    int          heap_allocated;  /* 0 = using stack, 1 = using heap */
    /* Stack buffers for small symbol counts */
    const char  *stack_symbols[SYMREADER_STACK_MAX];
    int          stack_lengths[SYMREADER_STACK_MAX];
} SymbolReader;

static int read_symbol_table(const uint8_t *data, size_t len, 
                              SymbolReader *sr, const uint8_t **value_start) {
    if (len < 2) return -1;
    
    /* Skip version byte */
    const uint8_t *p = data + 1;
    const uint8_t *end = data + len;
    
    /* Read symbol count */
    uint64_t sym_count;
    int n = sexp_decode_varint(p, end - p, &sym_count);
    if (n == 0 || sym_count > SEXP_MAX_SYMBOLS) return -1;
    p += n;
    
    sr->count = (int)sym_count;
    sr->heap_allocated = 0;
    
    if (sym_count > 0) {
        /* Use stack arrays for small counts, heap for large */
        if (sym_count <= SYMREADER_STACK_MAX) {
            sr->symbols = sr->stack_symbols;
            sr->lengths = sr->stack_lengths;
        } else {
            sr->symbols = malloc(sym_count * sizeof(char*));
            sr->lengths = malloc(sym_count * sizeof(int));
            if (!sr->symbols || !sr->lengths) {
                free((void*)sr->symbols);
                free(sr->lengths);
                return -1;
            }
            sr->heap_allocated = 1;
        }
        
        for (int i = 0; i < (int)sym_count; i++) {
            uint64_t sym_len;
            n = sexp_decode_varint(p, end - p, &sym_len);
            if (n == 0) {
                if (sr->heap_allocated) {
                    free((void*)sr->symbols);
                    free(sr->lengths);
                }
                return -1;
            }
            p += n;
            
            if (p + sym_len > end) {
                if (sr->heap_allocated) {
                    free((void*)sr->symbols);
                    free(sr->lengths);
                }
                return -1;
            }
            sr->symbols[i] = (const char*)p;
            sr->lengths[i] = (int)sym_len;
            p += sym_len;
        }
    } else {
        sr->symbols = NULL;
        sr->lengths = NULL;
    }
    
    *value_start = p;
    return 0;
}

static void free_symbol_reader(SymbolReader *sr) {
    if (sr->heap_allocated) {
        free((void*)sr->symbols);
        free(sr->lengths);
    }
    /* Stack buffers don't need freeing */
}

/* Forward declaration */
static int format_value(const uint8_t *p, const uint8_t *end, 
                        SymbolReader *sr, StrBuf *sb, const uint8_t **next);

static int format_string(const uint8_t *str, int len, StrBuf *sb) {
    if (strbuf_append_char(sb, '"') < 0) return -1;
    
    for (int i = 0; i < len; i++) {
        char c = str[i];
        switch (c) {
            case '"':  if (strbuf_append(sb, "\\\"", 2) < 0) return -1; break;
            case '\\': if (strbuf_append(sb, "\\\\", 2) < 0) return -1; break;
            case '\n': if (strbuf_append(sb, "\\n", 2) < 0) return -1; break;
            case '\r': if (strbuf_append(sb, "\\r", 2) < 0) return -1; break;
            case '\t': if (strbuf_append(sb, "\\t", 2) < 0) return -1; break;
            default:
                if ((unsigned char)c < 32) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\x%02x", (unsigned char)c);
                    if (strbuf_append(sb, buf, 4) < 0) return -1;
                } else {
                    if (strbuf_append_char(sb, c) < 0) return -1;
                }
        }
    }
    
    return strbuf_append_char(sb, '"');
}

static int format_list(const uint8_t *p, const uint8_t *end, int count,
                       SymbolReader *sr, StrBuf *sb, const uint8_t **next) {
    if (strbuf_append_char(sb, '(') < 0) return -1;
    
    for (int i = 0; i < count; i++) {
        if (i > 0) {
            if (strbuf_append_char(sb, ' ') < 0) return -1;
        }
        if (format_value(p, end, sr, sb, &p) < 0) return -1;
    }
    
    if (strbuf_append_char(sb, ')') < 0) return -1;
    *next = p;
    return 0;
}

static int format_value(const uint8_t *p, const uint8_t *end, 
                        SymbolReader *sr, StrBuf *sb, const uint8_t **next) {
    if (p >= end) return -1;
    
    uint8_t byte = *p++;
    uint8_t tag = byte & SEXP_TAG_MASK;
    uint8_t val = byte & SEXP_VALUE_MASK;
    
    switch (tag) {
        case SEXP_TAG_NIL:
            if (val == SEXP_TAG_INLINE_SYMBOL) {
                /* Inline symbol: 0x10 + varint len + bytes */
                uint64_t len;
                int n = sexp_decode_varint_fast(p, end - p, &len);
                if (n == 0) return -1;
                p += n;
                if (p + len > end) return -1;
                if (strbuf_append(sb, (const char*)p, len) < 0) return -1;
                *next = p + len;
                return 0;
            }
            if (strbuf_append(sb, "()", 2) < 0) return -1;
            *next = p;
            return 0;
            
        case SEXP_TAG_SMALLINT: {
            int ival = (int)val - SEXP_SMALLINT_BIAS;
            if (strbuf_printf(sb, "%d", ival) < 0) return -1;
            *next = p;
            return 0;
        }
        
        case SEXP_TAG_LARGEINT: {
            uint64_t zz;
            int n = sexp_decode_varint(p, end - p, &zz);
            if (n == 0) return -1;
            p += n;
            int64_t ival = sexp_zigzag_decode(zz);
            if (strbuf_printf(sb, "%lld", (long long)ival) < 0) return -1;
            *next = p;
            return 0;
        }
        
        case SEXP_TAG_FLOAT: {
            if (p + 8 > end) return -1;
            double dval = read_double(p);
            p += 8;
            /* Use %g for compact representation */
            if (strbuf_printf(sb, "%g", dval) < 0) return -1;
            *next = p;
            return 0;
        }
        
        case SEXP_TAG_SYMBOL: {
            uint64_t idx;
            int n = sexp_decode_varint(p, end - p, &idx);
            if (n == 0 || idx >= (uint64_t)sr->count) return -1;
            p += n;
            if (strbuf_append(sb, sr->symbols[idx], sr->lengths[idx]) < 0) return -1;
            *next = p;
            return 0;
        }
        
        case SEXP_TAG_SHORTSTR: {
            int len = val;
            if (p + len > end) return -1;
            if (format_string(p, len, sb) < 0) return -1;
            *next = p + len;
            return 0;
        }
        
        case SEXP_TAG_LONGSTR: {
            uint64_t len;
            int n = sexp_decode_varint(p, end - p, &len);
            if (n == 0) return -1;
            p += n;
            if (p + len > end) return -1;
            if (format_string(p, (int)len, sb) < 0) return -1;
            *next = p + len;
            return 0;
        }
        
        case SEXP_TAG_LIST: {
            /* Check for key index flag (bit 4) */
            int has_key_idx = (val & 0x10) != 0;
            int count = val & 0x0F;  /* Lower 4 bits only */
            
            if (count == 0 && !has_key_idx) {
                /* Large list format */
                if (p + 4 > end) return -1;
                count = (int)read_uint32_le(p);
                p += 4;
                
                /* Skip SEntry table */
                if (p + count * 4 > end) return -1;
                p += count * 4;
                
                return format_list(p, end, count, sr, sb, next);
            } else {
                /* Small list format */
                uint64_t payload_size;
                int n = sexp_decode_varint(p, end - p, &payload_size);
                if (n == 0) return -1;
                p += n;
                
                if (p + payload_size > end) return -1;
                const uint8_t *list_end = p + payload_size;
                
                /* If has key index, skip it to get to element data */
                if (has_key_idx) {
                    uint64_t num_keys;
                    n = sexp_decode_varint(p, list_end - p, &num_keys);
                    if (n == 0) return -1;
                    p += n;
                    
                    /* Skip key entries (sym_idx, elem_idx pairs) */
                    for (uint64_t i = 0; i < num_keys; i++) {
                        uint64_t tmp;
                        n = sexp_decode_varint(p, list_end - p, &tmp);
                        if (n == 0) return -1;
                        p += n;
                        n = sexp_decode_varint(p, list_end - p, &tmp);
                        if (n == 0) return -1;
                        p += n;
                    }
                }
                
                return format_list(p, list_end, count, sr, sb, next);
            }
        }
        
        default:
            return -1;
    }
}

/* Public API */

int sexp_read_init(SexpReadState *state, const uint8_t *data, size_t len) {
    if (len < 2) {
        state->error = SEXP_ERR_INVALID;
        return -1;
    }
    
    /* Check version */
    if (data[0] != SEXP_FORMAT_VERSION) {
        state->error = SEXP_ERR_INVALID;
        return -1;
    }
    
    state->data = data;
    state->data_len = len;
    state->symbols = data + 1;
    state->error = SEXP_OK;
    state->sym_strs = NULL;
    state->sym_lens = NULL;
    state->sym_hashes = NULL;
    state->sym_parsed = 0;  /* Lazy: don't parse symbols until needed */
    state->key_index = NULL; /* Lazy: built on first key lookup */
    
    /* Only read symbol count and skip to find root position */
    const uint8_t *p = data + 1;
    const uint8_t *end = data + len;
    
    uint64_t sym_count;
    int n = sexp_decode_varint_fast(p, end - p, &sym_count);
    if (n == 0 || sym_count > SEXP_MAX_SYMBOLS) {
        state->error = SEXP_ERR_INVALID;
        return -1;
    }
    p += n;
    state->sym_count = (int)sym_count;
    
    /* Skip symbol data without parsing - just find root position */
    for (int i = 0; i < (int)sym_count; i++) {
        uint64_t sym_len;
        n = sexp_decode_varint_fast(p, end - p, &sym_len);
        if (n == 0) {
            state->error = SEXP_ERR_INVALID;
            return -1;
        }
        p += n;
        
        if (p + sym_len > end) {
            state->error = SEXP_ERR_INVALID;
            return -1;
        }
        p += sym_len;
    }
    
    state->root = p;
    state->header_len = p - data;
    return 0;
}

/*
 * Lazy symbol table parsing - called only when symbols are accessed
 */
int sexp_ensure_symbols_parsed(SexpReadState *state) {
    if (state->sym_parsed) return 0;  /* Already parsed */
    if (state->sym_count == 0) {
        state->sym_parsed = 1;
        return 0;  /* No symbols to parse */
    }
    
    /* Allocate symbol arrays */
    state->sym_strs = malloc(state->sym_count * sizeof(char*));
    state->sym_lens = malloc(state->sym_count * sizeof(int));
    state->sym_hashes = malloc(state->sym_count * sizeof(uint32_t));
    
    if (!state->sym_strs || !state->sym_lens || !state->sym_hashes) {
        free(state->sym_strs);
        free(state->sym_lens);
        free(state->sym_hashes);
        state->sym_strs = NULL;
        state->sym_lens = NULL;
        state->sym_hashes = NULL;
        state->error = SEXP_ERR_MEMORY;
        return -1;
    }
    
    /* Parse symbol table */
    const uint8_t *p = state->symbols;
    const uint8_t *end = state->data + state->data_len;
    
    uint64_t sym_count;
    int n = sexp_decode_varint_fast(p, end - p, &sym_count);
    p += n;
    
    for (int i = 0; i < state->sym_count; i++) {
        uint64_t sym_len = 0;
        n = sexp_decode_varint_fast(p, end - p, &sym_len);
        p += n;
        
        state->sym_strs[i] = (const char*)p;
        state->sym_lens[i] = (int)sym_len;
        state->sym_hashes[i] = sexp_hash_bytes((const char*)p, (int)sym_len);
        p += sym_len;
    }
    
    state->sym_parsed = 1;
    return 0;
}

void sexp_read_free(SexpReadState *state) {
    free(state->sym_strs);
    free(state->sym_lens);
    free(state->sym_hashes);
    state->sym_strs = NULL;
    state->sym_lens = NULL;
    state->sym_hashes = NULL;
    
    /* Free key index if allocated */
    if (state->key_index) {
        free(state->key_index->buckets);
        free(state->key_index->entries);
        free(state->key_index);
        state->key_index = NULL;
    }
}

SexpType sexp_get_type(SexpReadState *state, const uint8_t *pos) {
    if (!pos || pos >= state->data + state->data_len) {
        state->error = SEXP_ERR_BOUNDS;
        return SEXP_TYPE_NIL;
    }
    
    uint8_t tag = *pos & SEXP_TAG_MASK;
    
    switch (tag) {
        case SEXP_TAG_NIL:
            /* Check for inline symbol */
            if ((*pos & 0x1F) == SEXP_TAG_INLINE_SYMBOL) return SEXP_TYPE_SYMBOL;
            return SEXP_TYPE_NIL;
        case SEXP_TAG_SMALLINT:
        case SEXP_TAG_LARGEINT: return SEXP_TYPE_INTEGER;
        case SEXP_TAG_FLOAT:    return SEXP_TYPE_FLOAT;
        case SEXP_TAG_SYMBOL:   return SEXP_TYPE_SYMBOL;
        case SEXP_TAG_SHORTSTR:
        case SEXP_TAG_LONGSTR:  return SEXP_TYPE_STRING;
        case SEXP_TAG_LIST:     return SEXP_TYPE_LIST;
        default:                return SEXP_TYPE_NIL;
    }
}

const char *sexp_type_name(SexpType type) {
    switch (type) {
        case SEXP_TYPE_NIL:     return "nil";
        case SEXP_TYPE_INTEGER: return "integer";
        case SEXP_TYPE_FLOAT:   return "float";
        case SEXP_TYPE_SYMBOL:  return "symbol";
        case SEXP_TYPE_STRING:  return "string";
        case SEXP_TYPE_LIST:    return "list";
        default:                return "unknown";
    }
}

char *sexp_to_text(SexpReadState *state) {
    SymbolReader sr;
    const uint8_t *value_start;
    
    if (read_symbol_table(state->data, state->data_len, &sr, &value_start) < 0) {
        state->error = SEXP_ERR_INVALID;
        return NULL;
    }
    
    StrBuf sb;
    if (strbuf_init(&sb, 256) < 0) {
        free_symbol_reader(&sr);
        state->error = SEXP_ERR_MEMORY;
        return NULL;
    }
    
    const uint8_t *next;
    if (format_value(value_start, state->data + state->data_len, &sr, &sb, &next) < 0) {
        strbuf_free(&sb);
        free_symbol_reader(&sr);
        state->error = SEXP_ERR_INVALID;
        return NULL;
    }
    
    /* Null terminate */
    if (strbuf_append_char(&sb, '\0') < 0) {
        strbuf_free(&sb);
        free_symbol_reader(&sr);
        state->error = SEXP_ERR_MEMORY;
        return NULL;
    }
    
    free_symbol_reader(&sr);
    return sb.data;
}
