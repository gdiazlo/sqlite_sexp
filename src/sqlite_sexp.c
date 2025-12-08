/*
 * sqlite_sexp.c - SQLite extension for S-expression data type
 *
 * This extension provides:
 * - sexp(text) - Parse s-expression text to binary blob
 * - sexp_text(blob) - Convert binary sexp back to text
 * - sexp_typeof(blob) - Get type name of sexp value
 * - sexp_car(blob) - Get first element of list
 * - sexp_cdr(blob) - Get rest of list (tail)
 * - sexp_nth(blob, n) - Get nth element (0-indexed)
 * - sexp_length(blob) - Get length of list
 * - sexp_contains(blob, blob) - Structural containment (needle appears as subtree)
 * - sexp_contains_key(blob, blob) - Key-based containment (like JSONB @>)
 * - is_nil(blob), is_list(blob), is_atom(blob), is_symbol(blob), 
 *   is_string(blob), is_number(blob) - Type predicates
 *
 * Containment operations use Bloom filter for fast rejection.
 */

#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1

/* Compatibility for older SQLite versions */
#ifndef SQLITE_SUBTYPE
#define SQLITE_SUBTYPE 0x000100000
#endif
#ifndef SQLITE_RESULT_SUBTYPE
#define SQLITE_RESULT_SUBTYPE 0x001000000
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "sexp.h"

/*
 * sexp(text) -> blob
 * Parse s-expression text and return binary blob
 */
static void sexp_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    if (argc != 1) {
        sqlite3_result_error(ctx, "sexp() requires 1 argument", -1);
        return;
    }
    
    if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_null(ctx);
        return;
    }
    
    const char *text = (const char *)sqlite3_value_text(argv[0]);
    int text_len = sqlite3_value_bytes(argv[0]);
    
    SexpParseState state;
    sexp_parse_init(&state, text, text_len);
    
    size_t blob_len;
    uint8_t *blob = sexp_parse(&state, &blob_len);
    
    if (!blob) {
        char errmsg[512];
        snprintf(errmsg, sizeof(errmsg), "Parse error: %s", state.error_msg);
        sqlite3_result_error(ctx, errmsg, -1);
        sexp_parse_free(&state);
        return;
    }
    
    sqlite3_result_blob(ctx, blob, blob_len, free);
    sqlite3_result_subtype(ctx, SEXP_SUBTYPE);  /* Mark as sexp for fast path */
    sexp_parse_free(&state);
}

/*
 * sexp_text(blob) -> text
 * Convert binary sexp to text representation
 */
static void sexp_text_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    if (argc != 1) {
        sqlite3_result_error(ctx, "sexp_text() requires 1 argument", -1);
        return;
    }
    
    if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_null(ctx);
        return;
    }
    
    const uint8_t *blob = sqlite3_value_blob(argv[0]);
    int blob_len = sqlite3_value_bytes(argv[0]);
    
    SexpReadState state;
    if (sexp_read_init(&state, blob, blob_len) < 0) {
        sqlite3_result_error(ctx, "Invalid sexp blob", -1);
        return;
    }
    
    char *text = sexp_to_text(&state);
    if (!text) {
        sqlite3_result_error(ctx, "Failed to convert sexp to text", -1);
        return;
    }
    
    sqlite3_result_text(ctx, text, -1, free);
}

/*
 * sexp_typeof(blob) -> text
 * Return type name: 'nil', 'integer', 'float', 'symbol', 'string', 'list'
 */
static void sexp_typeof_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    if (argc != 1) {
        sqlite3_result_error(ctx, "sexp_typeof() requires 1 argument", -1);
        return;
    }
    
    if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_null(ctx);
        return;
    }
    
    const uint8_t *blob = sqlite3_value_blob(argv[0]);
    int blob_len = sqlite3_value_bytes(argv[0]);
    
    SexpReadState state;
    if (sexp_read_init(&state, blob, blob_len) < 0) {
        sqlite3_result_error(ctx, "Invalid sexp blob", -1);
        return;
    }
    
    SexpType type = sexp_get_type(&state, state.root);
    const char *name = sexp_type_name(type);
    sqlite3_result_text(ctx, name, -1, SQLITE_STATIC);
}

/*
 * Helper: Set SQLite result to native SQL type based on sexp element type.
 * 
 * This is the key function that makes our API behave like SQLite's JSON:
 *   - nil      -> NULL
 *   - integer  -> INTEGER  
 *   - float    -> REAL
 *   - symbol   -> TEXT (symbol name), or INTEGER for true/false
 *   - string   -> TEXT (string content)
 *   - list     -> TEXT (s-expression representation, like json_extract)
 *
 * Returns 0 on success, -1 on error.
 */
static int sexp_result_native(sqlite3_context *ctx, SexpReadState *state, 
                              const uint8_t *elem_start, const uint8_t *elem_end) {
    if (!elem_start || elem_start >= elem_end) {
        sqlite3_result_null(ctx);
        return 0;
    }
    
    SexpType type = sexp_get_type(state, elem_start);
    
    switch (type) {
        case SEXP_TYPE_NIL:
            sqlite3_result_null(ctx);
            break;
            
        case SEXP_TYPE_INTEGER: {
            int64_t val = sexp_get_integer(state, elem_start);
            sqlite3_result_int64(ctx, val);
            break;
        }
        
        case SEXP_TYPE_FLOAT: {
            double val = sexp_get_float(state, elem_start);
            sqlite3_result_double(ctx, val);
            break;
        }
        
        case SEXP_TYPE_SYMBOL: {
            int len = 0;
            const char *sym = sexp_get_symbol(state, elem_start, &len);
            if (sym) {
                /* Check for boolean symbols - return SQL INTEGER like JSON */
                if (len == 4 && memcmp(sym, "true", 4) == 0) {
                    sqlite3_result_int(ctx, 1);
                } else if (len == 5 && memcmp(sym, "false", 5) == 0) {
                    sqlite3_result_int(ctx, 0);
                } else {
                    sqlite3_result_text(ctx, sym, len, SQLITE_TRANSIENT);
                }
            } else {
                sqlite3_result_null(ctx);
            }
            break;
        }
        
        case SEXP_TYPE_STRING: {
            int len = 0;
            const char *str = sexp_get_string(state, elem_start, &len);
            if (str) {
                sqlite3_result_text(ctx, str, len, SQLITE_TRANSIENT);
            } else {
                sqlite3_result_null(ctx);
            }
            break;
        }
        
        case SEXP_TYPE_LIST: {
            /* Lists are returned as TEXT (s-expression representation) like json_extract */
            size_t result_len;
            uint8_t *extracted = sexp_extract_element(state, elem_start, elem_end, &result_len);
            if (!extracted) {
                sqlite3_result_error_nomem(ctx);
                return -1;
            }
            
            /* Convert extracted blob to text representation */
            SexpReadState temp_state;
            if (sexp_read_init(&temp_state, extracted, result_len) < 0) {
                free(extracted);
                sqlite3_result_error(ctx, "Invalid extracted element", -1);
                return -1;
            }
            
            char *text = sexp_to_text(&temp_state);
            sexp_read_free(&temp_state);
            free(extracted);
            
            if (text) {
                sqlite3_result_text(ctx, text, -1, free);
            } else {
                sqlite3_result_error_nomem(ctx);
                return -1;
            }
            break;
        }
        
        default:
            sqlite3_result_null(ctx);
            break;
    }
    
    return 0;
}

/*
 * Helper: skip element and return pointer to next
 */
static const uint8_t *skip_elem(const uint8_t *p, const uint8_t *end) {
    if (p >= end) return NULL;
    
    uint8_t byte = *p++;
    uint8_t tag = byte & SEXP_TAG_MASK;
    uint8_t val = byte & SEXP_VALUE_MASK;
    
    switch (tag) {
        case SEXP_TAG_NIL:
        case SEXP_TAG_SMALLINT:
            return p;
        case SEXP_TAG_LARGEINT:
        case SEXP_TAG_SYMBOL:
            while (p < end && (*p & 0x80)) p++;
            return (p < end) ? p + 1 : NULL;
        case SEXP_TAG_FLOAT:
            return (p + 8 <= end) ? p + 8 : NULL;
        case SEXP_TAG_SHORTSTR:
            return (p + val <= end) ? p + val : NULL;
        case SEXP_TAG_LONGSTR: {
            uint64_t len;
            int n = sexp_decode_varint(p, end - p, &len);
            return (n && p + n + len <= end) ? p + n + len : NULL;
        }
        case SEXP_TAG_LIST:
            if (val == 0) {
                if (p + 4 > end) return NULL;
                uint32_t count = p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24);
                p += 4 + count * 4;
                for (uint32_t i = 0; i < count; i++) {
                    p = skip_elem(p, end);
                    if (!p) return NULL;
                }
                return p;
            } else {
                uint64_t size;
                int n = sexp_decode_varint(p, end - p, &size);
                return (n && p + n + size <= end) ? p + n + size : NULL;
            }
        default:
            return NULL;
    }
}

/*
 * sexp_car(blob) -> native SQL type
 * Get first element of list
 * Returns: INTEGER, REAL, TEXT (for symbols/strings), BLOB (for lists), or NULL
 */
static void sexp_car_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    if (argc != 1) {
        sqlite3_result_error(ctx, "sexp_car() requires 1 argument", -1);
        return;
    }
    
    if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_null(ctx);
        return;
    }
    
    const uint8_t *blob = sqlite3_value_blob(argv[0]);
    int blob_len = sqlite3_value_bytes(argv[0]);
    
    SexpReadState state;
    if (sexp_read_init(&state, blob, blob_len) < 0) {
        sqlite3_result_error(ctx, "Invalid sexp blob", -1);
        return;
    }
    
    /* Get first element pointer */
    const uint8_t *elem_start = sexp_car(&state, state.root);
    if (!elem_start) {
        sexp_read_free(&state);
        sqlite3_result_null(ctx);
        return;
    }
    
    /* Find element end */
    const uint8_t *elem_end = skip_elem(elem_start, state.data + state.data_len);
    if (!elem_end) {
        sexp_read_free(&state);
        sqlite3_result_error(ctx, "Invalid element", -1);
        return;
    }
    
    /* Return native SQL type based on element type */
    sexp_result_native(ctx, &state, elem_start, elem_end);
    sexp_read_free(&state);
}

/*
 * sexp_cdr(blob) -> blob
 * Get rest of list (all but first element)
 */
static void sexp_cdr_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    if (argc != 1) {
        sqlite3_result_error(ctx, "sexp_cdr() requires 1 argument", -1);
        return;
    }
    
    if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_null(ctx);
        return;
    }
    
    const uint8_t *blob = sqlite3_value_blob(argv[0]);
    int blob_len = sqlite3_value_bytes(argv[0]);
    
    SexpReadState state;
    if (sexp_read_init(&state, blob, blob_len) < 0) {
        sqlite3_result_error(ctx, "Invalid sexp blob", -1);
        return;
    }
    
    size_t result_len;
    uint8_t *result = sexp_cdr(&state, state.root, &result_len);
    
    sexp_read_free(&state);
    
    if (!result) {
        if (state.error == SEXP_ERR_MEMORY) {
            sqlite3_result_error_nomem(ctx);
        } else {
            sqlite3_result_null(ctx);
        }
        return;
    }
    
    sqlite3_result_blob(ctx, result, result_len, free);
    sqlite3_result_subtype(ctx, SEXP_SUBTYPE);  /* Mark as sexp for fast path */
}

/*
 * sexp_nth(blob, n) -> native SQL type
 * Get nth element of list (0-indexed)
 * Returns: INTEGER, REAL, TEXT (for symbols/strings), BLOB (for lists), or NULL
 */
static void sexp_nth_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    if (argc != 2) {
        sqlite3_result_error(ctx, "sexp_nth() requires 2 arguments", -1);
        return;
    }
    
    if (sqlite3_value_type(argv[0]) == SQLITE_NULL ||
        sqlite3_value_type(argv[1]) == SQLITE_NULL) {
        sqlite3_result_null(ctx);
        return;
    }
    
    const uint8_t *blob = sqlite3_value_blob(argv[0]);
    int blob_len = sqlite3_value_bytes(argv[0]);
    int n = sqlite3_value_int(argv[1]);
    
    SexpReadState state;
    if (sexp_read_init(&state, blob, blob_len) < 0) {
        sqlite3_result_error(ctx, "Invalid sexp blob", -1);
        return;
    }
    
    /* Get nth element pointer */
    const uint8_t *elem_start = sexp_nth(&state, state.root, n);
    if (!elem_start) {
        sexp_read_free(&state);
        sqlite3_result_null(ctx);
        return;
    }
    
    /* Find element end */
    const uint8_t *elem_end = skip_elem(elem_start, state.data + state.data_len);
    if (!elem_end) {
        sexp_read_free(&state);
        sqlite3_result_error(ctx, "Invalid element", -1);
        return;
    }
    
    /* Return native SQL type based on element type */
    sexp_result_native(ctx, &state, elem_start, elem_end);
    sexp_read_free(&state);
}

/*
 * sexp_length(blob) -> integer
 * Get number of elements in list (O(1) operation)
 */
static void sexp_length_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    if (argc != 1) {
        sqlite3_result_error(ctx, "sexp_length() requires 1 argument", -1);
        return;
    }
    
    if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_null(ctx);
        return;
    }
    
    const uint8_t *blob = sqlite3_value_blob(argv[0]);
    int blob_len = sqlite3_value_bytes(argv[0]);
    
    SexpReadState state;
    if (sexp_read_init(&state, blob, blob_len) < 0) {
        sqlite3_result_error(ctx, "Invalid sexp blob", -1);
        return;
    }
    
    int len = sexp_length(&state, state.root);
    sexp_read_free(&state);
    
    if (len < 0) {
        sqlite3_result_null(ctx);
    } else {
        sqlite3_result_int(ctx, len);
    }
}

/*
 * sexp_contains(blob, blob) -> boolean
 * Check if needle is contained anywhere in haystack (structural containment)
 */
static void sexp_contains_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    if (argc != 2) {
        sqlite3_result_error(ctx, "sexp_contains() requires 2 arguments", -1);
        return;
    }
    
    if (sqlite3_value_type(argv[0]) == SQLITE_NULL ||
        sqlite3_value_type(argv[1]) == SQLITE_NULL) {
        sqlite3_result_null(ctx);
        return;
    }
    
    const uint8_t *haystack = sqlite3_value_blob(argv[0]);
    int h_len = sqlite3_value_bytes(argv[0]);
    const uint8_t *needle = sqlite3_value_blob(argv[1]);
    int n_len = sqlite3_value_bytes(argv[1]);
    
    int result = sexp_contains(haystack, h_len, needle, n_len);
    sqlite3_result_int(ctx, result);
}

/*
 * Cached needle state for sexp_contains_key
 * Used with sqlite3_get_auxdata/sqlite3_set_auxdata to avoid re-parsing
 * constant needle expressions on every row.
 */
typedef struct {
    SexpReadState state;
    uint8_t *data;        /* Copy of needle data (we own this) */
    size_t data_len;
} CachedNeedleState;

static void cached_needle_free(void *p) {
    CachedNeedleState *cached = (CachedNeedleState *)p;
    if (cached) {
        sexp_read_free(&cached->state);
        sqlite3_free(cached->data);
        sqlite3_free(cached);
    }
}

/*
 * sexp_contains_key(blob, blob) -> boolean
 * Key-based containment (like JSONB @> operator)
 * Treats list heads as keys, matches remaining elements in any order.
 *
 * Examples:
 *   sexp_contains_key((user (name "alice") (age 30)), (user (age 30))) -> 1
 *   sexp_contains_key((+ 1 2 3), (+ 2 1)) -> 1 (order independent)
 *
 * Performance optimization: The needle (second argument) is cached when it's
 * a constant expression, avoiding re-parsing on every row.
 */
static void sexp_contains_key_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    if (argc != 2) {
        sqlite3_result_error(ctx, "sexp_contains_key() requires 2 arguments", -1);
        return;
    }
    
    if (sqlite3_value_type(argv[0]) == SQLITE_NULL ||
        sqlite3_value_type(argv[1]) == SQLITE_NULL) {
        sqlite3_result_null(ctx);
        return;
    }
    
    const uint8_t *haystack = sqlite3_value_blob(argv[0]);
    int h_len = sqlite3_value_bytes(argv[0]);
    const uint8_t *needle = sqlite3_value_blob(argv[1]);
    int n_len = sqlite3_value_bytes(argv[1]);
    
    /* Try to get cached needle state */
    CachedNeedleState *cached = (CachedNeedleState *)sqlite3_get_auxdata(ctx, 1);
    
    if (cached) {
        /* Verify the cached data matches current needle (safety check) */
        if (cached->data_len == (size_t)n_len && 
            memcmp(cached->data, needle, n_len) == 0) {
            /* Use cached state */
            int result = sexp_contains_key_with_state(haystack, h_len, &cached->state);
            sqlite3_result_int(ctx, result);
            return;
        }
        /* Data changed, fall through to re-parse */
    }
    
    /* Parse needle and cache it */
    cached = sqlite3_malloc(sizeof(CachedNeedleState));
    if (!cached) {
        /* Fall back to uncached version */
        int result = sexp_contains_key(haystack, h_len, needle, n_len);
        sqlite3_result_int(ctx, result);
        return;
    }
    
    cached->data = sqlite3_malloc(n_len);
    if (!cached->data) {
        sqlite3_free(cached);
        int result = sexp_contains_key(haystack, h_len, needle, n_len);
        sqlite3_result_int(ctx, result);
        return;
    }
    memcpy(cached->data, needle, n_len);
    cached->data_len = n_len;
    
    if (sexp_read_init(&cached->state, cached->data, n_len) < 0) {
        sqlite3_free(cached->data);
        sqlite3_free(cached);
        sqlite3_result_int(ctx, 0);
        return;
    }
    
    /* Cache for future calls (SQLite will call cached_needle_free when done) */
    sqlite3_set_auxdata(ctx, 1, cached, cached_needle_free);
    
    /* Execute with newly cached state */
    int result = sexp_contains_key_with_state(haystack, h_len, &cached->state);
    sqlite3_result_int(ctx, result);
}

/*
 * sexp_string_value(blob) -> text
 * Return raw UTF-8 bytes from a string element without reformatting
 */
static void sexp_string_value_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    if (argc != 1) {
        sqlite3_result_error(ctx, "sexp_string_value() requires 1 argument", -1);
        return;
    }

    if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_null(ctx);
        return;
    }

    const uint8_t *blob = sqlite3_value_blob(argv[0]);
    int blob_len = sqlite3_value_bytes(argv[0]);

    SexpReadState state;
    if (sexp_read_init(&state, blob, blob_len) < 0) {
        sqlite3_result_error(ctx, "Invalid sexp blob", -1);
        return;
    }

    int len = 0;
    const char *str = sexp_get_string(&state, state.root, &len);
    if (!str) {
        sexp_read_free(&state);
        sqlite3_result_null(ctx);
        return;
    }

    sqlite3_result_text(ctx, str, len, SQLITE_TRANSIENT);
    sexp_read_free(&state);
}

/*
 * sexp_symbol_value(blob) -> text
 * Return symbol name as text (no quotes, no escaping)
 */
static void sexp_symbol_value_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    if (argc != 1) {
        sqlite3_result_error(ctx, "sexp_symbol_value() requires 1 argument", -1);
        return;
    }

    if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_null(ctx);
        return;
    }

    const uint8_t *blob = sqlite3_value_blob(argv[0]);
    int blob_len = sqlite3_value_bytes(argv[0]);

    SexpReadState state;
    if (sexp_read_init(&state, blob, blob_len) < 0) {
        sqlite3_result_error(ctx, "Invalid sexp blob", -1);
        return;
    }

    int len = 0;
    const char *sym = sexp_get_symbol(&state, state.root, &len);
    if (!sym) {
        sexp_read_free(&state);
        sqlite3_result_null(ctx);
        return;
    }

    sqlite3_result_text(ctx, sym, len, SQLITE_TRANSIENT);
    sexp_read_free(&state);
}

/*
 * sexp_int_value(blob) -> integer
 * Return integer value directly (no text conversion)
 */
static void sexp_int_value_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    if (argc != 1) {
        sqlite3_result_error(ctx, "sexp_int_value() requires 1 argument", -1);
        return;
    }

    if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_null(ctx);
        return;
    }

    const uint8_t *blob = sqlite3_value_blob(argv[0]);
    int blob_len = sqlite3_value_bytes(argv[0]);

    SexpReadState state;
    if (sexp_read_init(&state, blob, blob_len) < 0) {
        sqlite3_result_error(ctx, "Invalid sexp blob", -1);
        return;
    }

    /* Check type first */
    SexpType type = sexp_get_type(&state, state.root);
    if (type != SEXP_TYPE_INTEGER) {
        sexp_read_free(&state);
        sqlite3_result_null(ctx);
        return;
    }

    int64_t val = sexp_get_integer(&state, state.root);
    sexp_read_free(&state);
    
    sqlite3_result_int64(ctx, val);
}

/*
 * sexp_float_value(blob) -> real
 * Return float value directly (no text conversion)
 */
static void sexp_float_value_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    if (argc != 1) {
        sqlite3_result_error(ctx, "sexp_float_value() requires 1 argument", -1);
        return;
    }

    if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_null(ctx);
        return;
    }

    const uint8_t *blob = sqlite3_value_blob(argv[0]);
    int blob_len = sqlite3_value_bytes(argv[0]);

    SexpReadState state;
    if (sexp_read_init(&state, blob, blob_len) < 0) {
        sqlite3_result_error(ctx, "Invalid sexp blob", -1);
        return;
    }

    /* Check type first */
    SexpType type = sexp_get_type(&state, state.root);
    if (type != SEXP_TYPE_FLOAT) {
        sexp_read_free(&state);
        sqlite3_result_null(ctx);
        return;
    }

    double val = sexp_get_float(&state, state.root);
    sexp_read_free(&state);
    
    sqlite3_result_double(ctx, val);
}

/*
 * sexp_number_value(blob) -> integer or real
 * Return numeric value (works for both integers and floats)
 */
static void sexp_number_value_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    if (argc != 1) {
        sqlite3_result_error(ctx, "sexp_number_value() requires 1 argument", -1);
        return;
    }

    if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_null(ctx);
        return;
    }

    const uint8_t *blob = sqlite3_value_blob(argv[0]);
    int blob_len = sqlite3_value_bytes(argv[0]);

    SexpReadState state;
    if (sexp_read_init(&state, blob, blob_len) < 0) {
        sqlite3_result_error(ctx, "Invalid sexp blob", -1);
        return;
    }

    SexpType type = sexp_get_type(&state, state.root);
    
    if (type == SEXP_TYPE_INTEGER) {
        int64_t val = sexp_get_integer(&state, state.root);
        sexp_read_free(&state);
        sqlite3_result_int64(ctx, val);
    } else if (type == SEXP_TYPE_FLOAT) {
        double val = sexp_get_float(&state, state.root);
        sexp_read_free(&state);
        sqlite3_result_double(ctx, val);
    } else {
        sexp_read_free(&state);
        sqlite3_result_null(ctx);
    }
}

/*
 * sexp_path(blob, idx1, idx2, ...) -> native SQL type
 * Returns nested element by following fixed indices without intermediate extraction
 * Returns: INTEGER, REAL, TEXT (for symbols/strings), BLOB (for lists), or NULL
 */
static void sexp_path_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    if (argc < 2) {
        sqlite3_result_error(ctx, "sexp_path() requires at least 2 arguments", -1);
        return;
    }

    if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_null(ctx);
        return;
    }

    const uint8_t *blob = sqlite3_value_blob(argv[0]);
    int blob_len = sqlite3_value_bytes(argv[0]);

    SexpReadState state;
    if (sexp_read_init(&state, blob, blob_len) < 0) {
        sqlite3_result_error(ctx, "Invalid sexp blob", -1);
        return;
    }

    int depth = argc - 1;
    int indices[SEXP_MAX_DEPTH];
    if (depth > SEXP_MAX_DEPTH) {
        sexp_read_free(&state);
        sqlite3_result_error(ctx, "sexp_path depth exceeded", -1);
        return;
    }

    for (int i = 0; i < depth; i++) {
        if (sqlite3_value_type(argv[i + 1]) == SQLITE_NULL) {
            sexp_read_free(&state);
            sqlite3_result_null(ctx);
            return;
        }
        indices[i] = sqlite3_value_int(argv[i + 1]);
    }

    const uint8_t *pos = sexp_path_follow(&state, state.root, indices, depth);
    if (!pos) {
        sexp_read_free(&state);
        sqlite3_result_null(ctx);
        return;
    }

    const uint8_t *elem_end = skip_elem(pos, state.data + state.data_len);
    if (!elem_end) {
        sexp_read_free(&state);
        sqlite3_result_error(ctx, "Invalid element", -1);
        return;
    }

    /* Return native SQL type based on element type */
    sexp_result_native(ctx, &state, pos, elem_end);
    sexp_read_free(&state);
}

/*
 * sexp_get(blob, key1, key2, ...) -> native SQL type
 * Get value by key path from alist-style s-expression.
 * 
 * Supports multiple keys for nested access without re-parsing.
 * Works like json_extract() but for s-expressions.
 *
 * Example: (event (user (name "alice") (age 30)))
 *   sexp_get(data, 'user')           -> (name "alice") (age 30)  (as nested structure)
 *   sexp_get(data, 'user', 'name')   -> 'alice'  (TEXT)
 *   sexp_get(data, 'user', 'age')    -> 30       (INTEGER)
 *
 * Returns: INTEGER, REAL, TEXT (for symbols/strings/nested lists), or NULL
 */
static void sexp_get_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    if (argc < 2) {
        sqlite3_result_error(ctx, "sexp_get() requires at least 2 arguments", -1);
        return;
    }
    
    if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_null(ctx);
        return;
    }
    
    const uint8_t *blob = sqlite3_value_blob(argv[0]);
    int blob_len = sqlite3_value_bytes(argv[0]);
    
    SexpReadState state;
    if (sexp_read_init(&state, blob, blob_len) < 0) {
        sqlite3_result_error(ctx, "Invalid sexp blob", -1);
        return;
    }
    
    /* Navigate through key path */
    const uint8_t *current_pos = state.root;
    const uint8_t *value_end = NULL;
    
    for (int i = 1; i < argc; i++) {
        if (sqlite3_value_type(argv[i]) == SQLITE_NULL) {
            sexp_read_free(&state);
            sqlite3_result_null(ctx);
            return;
        }
        
        const char *key = (const char *)sqlite3_value_text(argv[i]);
        int key_len = sqlite3_value_bytes(argv[i]);
        
        value_end = NULL;
        current_pos = sexp_get_by_key(&state, current_pos, key, key_len, &value_end);
        
        if (!current_pos) {
            sexp_read_free(&state);
            sqlite3_result_null(ctx);
            return;
        }
    }
    
    /* If value_end wasn't set, calculate it */
    if (!value_end) {
        value_end = skip_elem(current_pos, state.data + state.data_len);
        if (!value_end) {
            sexp_read_free(&state);
            sqlite3_result_error(ctx, "Invalid element", -1);
            return;
        }
    }
    
    /* Return native SQL type based on element type */
    sexp_result_native(ctx, &state, current_pos, value_end);
    sexp_read_free(&state);
}

/*
 * OPTIMIZED TYPE PREDICATES
 *
 * These functions check the type tag directly from the blob without
 * full parsing. This is O(1) and avoids memory allocation.
 *
 * Fast path: Check version byte + skip symbol table to get root tag.
 * The root element tag byte tells us the type immediately.
 */

/*
 * Helper: Get root element tag byte directly from blob (O(1), no allocation)
 * Returns -1 on error, otherwise the tag byte
 */
static int get_root_tag_fast(const uint8_t *blob, int blob_len) {
    if (blob_len < 3) return -1;  /* Minimum: version + sym_count(1) + tag(1) */
    
    /* Check version */
    if (blob[0] != SEXP_FORMAT_VERSION) return -1;
    
    /* Skip symbol table: decode symbol count and skip all symbols */
    const uint8_t *p = blob + 1;
    const uint8_t *end = blob + blob_len;
    
    uint64_t sym_count;
    int n = sexp_decode_varint_fast(p, end - p, &sym_count);
    if (n == 0) return -1;
    p += n;
    
    /* Skip each symbol (varint length + bytes) */
    for (uint64_t i = 0; i < sym_count; i++) {
        uint64_t sym_len;
        n = sexp_decode_varint_fast(p, end - p, &sym_len);
        if (n == 0) return -1;
        p += n;
        if (p + sym_len > end) return -1;
        p += sym_len;
    }
    
    /* p now points to root element - return its tag byte */
    if (p >= end) return -1;
    return *p;
}

/*
 * Helper: Get SexpType from tag byte
 */
static SexpType tag_to_type(int tag_byte) {
    if (tag_byte < 0) return SEXP_TYPE_NIL;
    
    uint8_t tag = tag_byte & SEXP_TAG_MASK;
    uint8_t val = tag_byte & SEXP_VALUE_MASK;
    
    switch (tag) {
        case SEXP_TAG_NIL:
            /* Check for inline symbol */
            if (val == SEXP_TAG_INLINE_SYMBOL) return SEXP_TYPE_SYMBOL;
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

static void is_nil_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    if (argc != 1 || sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_null(ctx);
        return;
    }
    
    const uint8_t *blob = sqlite3_value_blob(argv[0]);
    int blob_len = sqlite3_value_bytes(argv[0]);
    
    int tag_byte = get_root_tag_fast(blob, blob_len);
    if (tag_byte < 0) {
        sqlite3_result_int(ctx, 0);
        return;
    }
    
    /* NIL tag with value 0 (not inline symbol) */
    sqlite3_result_int(ctx, (tag_byte & SEXP_TAG_MASK) == SEXP_TAG_NIL &&
                            (tag_byte & SEXP_VALUE_MASK) == 0);
}

static void is_list_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    if (argc != 1 || sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_null(ctx);
        return;
    }
    
    const uint8_t *blob = sqlite3_value_blob(argv[0]);
    int blob_len = sqlite3_value_bytes(argv[0]);
    
    int tag_byte = get_root_tag_fast(blob, blob_len);
    if (tag_byte < 0) {
        sqlite3_result_int(ctx, 0);
        return;
    }
    
    uint8_t tag = tag_byte & SEXP_TAG_MASK;
    /* NIL (empty list) or LIST tag */
    sqlite3_result_int(ctx, tag == SEXP_TAG_LIST || 
                           (tag == SEXP_TAG_NIL && (tag_byte & SEXP_VALUE_MASK) == 0));
}

static void is_atom_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    if (argc != 1 || sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_null(ctx);
        return;
    }
    
    const uint8_t *blob = sqlite3_value_blob(argv[0]);
    int blob_len = sqlite3_value_bytes(argv[0]);
    
    int tag_byte = get_root_tag_fast(blob, blob_len);
    SexpType type = tag_to_type(tag_byte);
    
    sqlite3_result_int(ctx, type == SEXP_TYPE_SYMBOL || 
                            type == SEXP_TYPE_STRING ||
                            type == SEXP_TYPE_INTEGER ||
                            type == SEXP_TYPE_FLOAT);
}

static void is_symbol_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    if (argc != 1 || sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_null(ctx);
        return;
    }
    
    const uint8_t *blob = sqlite3_value_blob(argv[0]);
    int blob_len = sqlite3_value_bytes(argv[0]);
    
    int tag_byte = get_root_tag_fast(blob, blob_len);
    SexpType type = tag_to_type(tag_byte);
    
    sqlite3_result_int(ctx, type == SEXP_TYPE_SYMBOL);
}

static void is_string_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    if (argc != 1 || sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_null(ctx);
        return;
    }
    
    const uint8_t *blob = sqlite3_value_blob(argv[0]);
    int blob_len = sqlite3_value_bytes(argv[0]);
    
    int tag_byte = get_root_tag_fast(blob, blob_len);
    SexpType type = tag_to_type(tag_byte);
    
    sqlite3_result_int(ctx, type == SEXP_TYPE_STRING);
}

static void is_number_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    if (argc != 1 || sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_null(ctx);
        return;
    }
    
    const uint8_t *blob = sqlite3_value_blob(argv[0]);
    int blob_len = sqlite3_value_bytes(argv[0]);
    
    int tag_byte = get_root_tag_fast(blob, blob_len);
    SexpType type = tag_to_type(tag_byte);
    
    sqlite3_result_int(ctx, type == SEXP_TYPE_INTEGER || type == SEXP_TYPE_FLOAT);
}

static void is_integer_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    if (argc != 1 || sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_null(ctx);
        return;
    }
    
    const uint8_t *blob = sqlite3_value_blob(argv[0]);
    int blob_len = sqlite3_value_bytes(argv[0]);
    
    int tag_byte = get_root_tag_fast(blob, blob_len);
    SexpType type = tag_to_type(tag_byte);
    
    sqlite3_result_int(ctx, type == SEXP_TYPE_INTEGER);
}

static void is_float_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    if (argc != 1 || sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_null(ctx);
        return;
    }
    
    const uint8_t *blob = sqlite3_value_blob(argv[0]);
    int blob_len = sqlite3_value_bytes(argv[0]);
    
    int tag_byte = get_root_tag_fast(blob, blob_len);
    SexpType type = tag_to_type(tag_byte);
    
    sqlite3_result_int(ctx, type == SEXP_TYPE_FLOAT);
}

/*
 * Extension entry point
 */
#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_sexp_init(sqlite3 *db, char **pzErrMsg, 
                      const sqlite3_api_routines *pApi);

#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_sexp_init(sqlite3 *db, char **pzErrMsg, 
                      const sqlite3_api_routines *pApi) {
    int rc;
    SQLITE_EXTENSION_INIT2(pApi);
    
    (void)pzErrMsg;  /* Unused */
    
    /* 
     * Register functions with SQLITE_SUBTYPE for consuming subtypes
     * and SQLITE_RESULT_SUBTYPE for producing them.
     * This enables fast-path processing for chained sexp function calls.
     */
    
    /* sexp() produces sexp blobs */
    rc = sqlite3_create_function(db, "sexp", 1, 
                                  SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_RESULT_SUBTYPE,
                                  NULL, sexp_func, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    /* sexp_text() consumes sexp blobs */
    rc = sqlite3_create_function(db, "sexp_text", 1,
                                  SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_SUBTYPE,
                                  NULL, sexp_text_func, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    /* sexp_typeof() consumes sexp blobs */
    rc = sqlite3_create_function(db, "sexp_typeof", 1,
                                  SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_SUBTYPE,
                                  NULL, sexp_typeof_func, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    /* sexp_car() consumes and produces sexp blobs */
    rc = sqlite3_create_function(db, "sexp_car", 1,
                                  SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_SUBTYPE | SQLITE_RESULT_SUBTYPE,
                                  NULL, sexp_car_func, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    /* Alias: head = car */
    rc = sqlite3_create_function(db, "sexp_head", 1,
                                  SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_SUBTYPE | SQLITE_RESULT_SUBTYPE,
                                  NULL, sexp_car_func, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    /* sexp_cdr() consumes and produces sexp blobs */
    rc = sqlite3_create_function(db, "sexp_cdr", 1,
                                  SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_SUBTYPE | SQLITE_RESULT_SUBTYPE,
                                  NULL, sexp_cdr_func, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    /* Alias: tail = cdr */
    rc = sqlite3_create_function(db, "sexp_tail", 1,
                                  SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_SUBTYPE | SQLITE_RESULT_SUBTYPE,
                                  NULL, sexp_cdr_func, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    /* sexp_nth() consumes and produces sexp blobs */
    rc = sqlite3_create_function(db, "sexp_nth", 2,
                                  SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_SUBTYPE | SQLITE_RESULT_SUBTYPE,
                                  NULL, sexp_nth_func, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    /* sexp_path() consumes sexp blobs and produces native types or blobs */
    rc = sqlite3_create_function(db, "sexp_path", -1,
                                  SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_SUBTYPE | SQLITE_RESULT_SUBTYPE,
                                  NULL, sexp_path_func, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    /* sexp_get() consumes sexp blobs - key-based value extraction (like json_extract) */
    /* Supports variadic args: sexp_get(data, 'key1', 'key2', ...) for nested access */
    rc = sqlite3_create_function(db, "sexp_get", -1,
                                  SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_SUBTYPE | SQLITE_RESULT_SUBTYPE,
                                  NULL, sexp_get_func, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    /* sexp_length() consumes sexp blobs */
    rc = sqlite3_create_function(db, "sexp_length", 1,
                                  SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_SUBTYPE,
                                  NULL, sexp_length_func, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    /* sexp_contains() consumes sexp blobs - structural containment */
    rc = sqlite3_create_function(db, "sexp_contains", 2,
                                  SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_SUBTYPE,
                                  NULL, sexp_contains_func, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    /* sexp_contains_key() consumes sexp blobs - key-based containment (like JSONB @>) */
    rc = sqlite3_create_function(db, "sexp_contains_key", 2,
                                  SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_SUBTYPE,
                                  NULL, sexp_contains_key_func, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    /* sexp_string_value() consumes sexp blobs */
    rc = sqlite3_create_function(db, "sexp_string_value", 1,
                                  SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_SUBTYPE,
                                  NULL, sexp_string_value_func, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    /* sexp_symbol_value() consumes sexp blobs - returns symbol name as text */
    rc = sqlite3_create_function(db, "sexp_symbol_value", 1,
                                  SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_SUBTYPE,
                                  NULL, sexp_symbol_value_func, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    /* sexp_int_value() consumes sexp blobs - returns integer directly */
    rc = sqlite3_create_function(db, "sexp_int_value", 1,
                                  SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_SUBTYPE,
                                  NULL, sexp_int_value_func, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    /* sexp_float_value() consumes sexp blobs - returns float directly */
    rc = sqlite3_create_function(db, "sexp_float_value", 1,
                                  SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_SUBTYPE,
                                  NULL, sexp_float_value_func, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    /* sexp_number_value() consumes sexp blobs - returns int or float */
    rc = sqlite3_create_function(db, "sexp_number_value", 1,
                                  SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_SUBTYPE,
                                  NULL, sexp_number_value_func, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    /* Type predicates - consume sexp blobs (O(1) optimized) */
    rc = sqlite3_create_function(db, "is_nil", 1,
                                  SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_SUBTYPE,
                                  NULL, is_nil_func, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = sqlite3_create_function(db, "is_list", 1,
                                  SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_SUBTYPE,
                                  NULL, is_list_func, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = sqlite3_create_function(db, "is_atom", 1,
                                  SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_SUBTYPE,
                                  NULL, is_atom_func, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = sqlite3_create_function(db, "is_symbol", 1,
                                  SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_SUBTYPE,
                                  NULL, is_symbol_func, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = sqlite3_create_function(db, "is_string", 1,
                                  SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_SUBTYPE,
                                  NULL, is_string_func, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = sqlite3_create_function(db, "is_number", 1,
                                  SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_SUBTYPE,
                                  NULL, is_number_func, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = sqlite3_create_function(db, "is_integer", 1,
                                  SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_SUBTYPE,
                                  NULL, is_integer_func, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = sqlite3_create_function(db, "is_float", 1,
                                  SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_SUBTYPE,
                                  NULL, is_float_func, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    return SQLITE_OK;
}
