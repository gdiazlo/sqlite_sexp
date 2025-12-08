/*
 * sexp_ops.c - S-expression operations (car, cdr, nth, contains, etc.)
 *
 * OPTIMIZATIONS (ported from pg_sexp):
 * - Element extraction reuses parent's symbol table header (zero-copy when possible)
 * - Pre-parsed symbol table with hashes for fast equality
 * - O(1) list length from header
 * - O(1) element access for large lists via SEntry table
 * - In-place containment comparison without allocation
 * - Small list size prefix enables O(1) skip
 * - Key index in list header for O(1) key-based containment
 */

#include <stdlib.h>
#include <string.h>
#include "sexp.h"

/* Read helpers */
static inline uint32_t read_uint32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | 
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
 * Read a 32-bit SEntry from byte array (alignment-safe)
 */
static inline uint32_t read_sentry(const uint8_t *sentries, int idx) {
    const uint8_t *p = sentries + idx * 4;
    return read_uint32_le(p);
}

/*
 * List header structure for decoded list info
 */
typedef struct {
    uint64_t count;
    const uint8_t *sentries;       /* SEntry table bytes (NULL for small lists) */
    const uint8_t *data_start;
    int is_large;
    /* Key index for O(1) key lookup (when HAS_KEYIDX flag is set) */
    int has_key_index;
    int key_count;
    const uint8_t *key_index;      /* Points to num_keys varint start */
} ListHeader;

/* Forward declarations for helper functions */
static int decode_list_header(const uint8_t **pp, const uint8_t *end, 
                               uint8_t tag_byte, ListHeader *hdr);
static const uint8_t *skip_element(const uint8_t *p, const uint8_t *end);
static int get_list_info(const uint8_t *p, const uint8_t *end, ListHeader *hdr);
static int elements_equal_inplace(SexpReadState *state_a, const uint8_t *a,
                                   SexpReadState *state_b, const uint8_t *b);
static int lookup_key_index(const ListHeader *hdr, int target_sym_idx);

/*
 * Decode list header after tag byte
 * Returns 0 on success, -1 on error
 */
static int decode_list_header(const uint8_t **pp, const uint8_t *end, 
                               uint8_t tag_byte, ListHeader *hdr) {
    const uint8_t *p = *pp;
    int has_key_idx = (tag_byte & SEXP_LIST_HAS_KEYIDX) != 0;
    uint64_t count = tag_byte & 0x0F;  /* Lower 4 bits for count (max 15) */
    
    /* Initialize key index fields */
    hdr->has_key_index = 0;
    hdr->key_count = 0;
    hdr->key_index = NULL;
    
    if (count == 0 && !has_key_idx) {
        /* Large list with SEntry table */
        if (p + 4 > end) return -1;
        hdr->count = read_uint32_le(p);
        p += 4;
        
        if (p + hdr->count * 4 > end) return -1;
        hdr->sentries = p;
        p += hdr->count * 4;
        hdr->data_start = p;
        hdr->is_large = 1;
    } else {
        /* Small list with size prefix */
        uint64_t payload_size;
        int n = sexp_decode_varint_fast(p, end - p, &payload_size);
        if (n == 0) return -1;
        p += n;
        
        hdr->count = count;
        hdr->sentries = NULL;
        hdr->is_large = 0;
        
        if (has_key_idx) {
            /* Parse key index */
            hdr->has_key_index = 1;
            hdr->key_index = p;
            
            uint64_t num_keys;
            n = sexp_decode_varint_fast(p, end - p, &num_keys);
            if (n == 0) return -1;
            p += n;
            hdr->key_count = (int)num_keys;
            
            /* Skip over key entries (sym_idx, elem_idx pairs) */
            for (int i = 0; i < (int)num_keys; i++) {
                uint64_t sym_idx, elem_idx;
                n = sexp_decode_varint_fast(p, end - p, &sym_idx);
                if (n == 0) return -1;
                p += n;
                n = sexp_decode_varint_fast(p, end - p, &elem_idx);
                if (n == 0) return -1;
                p += n;
            }
        }
        
        hdr->data_start = p;
    }
    
    *pp = p;
    return 0;
}

/*
 * Skip over an element - O(1) for atoms and small lists (with size prefix)
 */
static const uint8_t *skip_element(const uint8_t *p, const uint8_t *end) {
    if (p >= end) return NULL;
    
    uint8_t byte = *p++;
    uint8_t tag = byte & SEXP_TAG_MASK;
    uint8_t val = byte & SEXP_VALUE_MASK;
    
    switch (tag) {
        case SEXP_TAG_NIL:
            return p;
            
        case SEXP_TAG_SMALLINT:
            return p;
            
        case SEXP_TAG_LARGEINT: {
            /* Skip varint */
            while (p < end && (*p & 0x80)) p++;
            if (p < end) p++;
            return p;
        }
        
        case SEXP_TAG_FLOAT:
            return (p + 8 <= end) ? p + 8 : NULL;
            
        case SEXP_TAG_SYMBOL: {
            /* Skip varint index */
            while (p < end && (*p & 0x80)) p++;
            if (p < end) p++;
            return p;
        }
        
        case SEXP_TAG_SHORTSTR:
            return (p + val <= end) ? p + val : NULL;
            
        case SEXP_TAG_LONGSTR: {
            uint64_t len;
            int n = sexp_decode_varint_fast(p, end - p, &len);
            if (n == 0) return NULL;
            p += n;
            return (p + len <= end) ? p + len : NULL;
        }
        
        case SEXP_TAG_LIST: {
            if (val == 0) {
                /* Large list - must skip elements manually */
                if (p + 4 > end) return NULL;
                uint32_t count = read_uint32_le(p);
                p += 4;
                
                /* Skip SEntry table */
                if (p + count * 4 > end) return NULL;
                p += count * 4;
                
                /* Skip each element */
                for (uint32_t i = 0; i < count; i++) {
                    p = skip_element(p, end);
                    if (!p) return NULL;
                }
                return p;
            } else {
                /* Small list - O(1) skip using size prefix! */
                uint64_t payload_size;
                int n = sexp_decode_varint_fast(p, end - p, &payload_size);
                if (n == 0) return NULL;
                p += n;
                return (p + payload_size <= end) ? p + payload_size : NULL;
            }
        }
        
        default:
            return NULL;
    }
}

/*
 * Get bounds of element at index in list
 * O(1) for large lists (SEntry), O(n) for small lists (max 4 elements)
 */
static void get_element_bounds(const ListHeader *hdr, int idx, const uint8_t *end,
                                const uint8_t **elem_start, const uint8_t **elem_end) {
    if (hdr->sentries) {
        /* Large list: O(1) via SEntry - use alignment-safe reader */
        uint32_t entry = read_sentry(hdr->sentries, idx);
        *elem_start = hdr->data_start + SENTRY_OFFSET(entry);
        if (idx + 1 < (int)hdr->count) {
            uint32_t next_entry = read_sentry(hdr->sentries, idx + 1);
            *elem_end = hdr->data_start + SENTRY_OFFSET(next_entry);
        } else {
            *elem_end = end;
        }
    } else {
        /* Small list: scan to element (at most 4 elements) */
        const uint8_t *p = hdr->data_start;
        for (int j = 0; j < idx; j++) {
            p = skip_element(p, end);
        }
        *elem_start = p;
        *elem_end = skip_element(p, end);
    }
}

/*
 * Look up element index by symbol index in key index.
 * Returns element index (0-based), or -1 if not found.
 */
static int lookup_key_index(const ListHeader *hdr, int target_sym_idx) {
    if (!hdr->has_key_index || hdr->key_count == 0) return -1;
    
    const uint8_t *p = hdr->key_index;
    const uint8_t *key_end = hdr->data_start;  /* Key index ends where data starts */
    
    /* Skip num_keys */
    uint64_t num_keys;
    int n = sexp_decode_varint_fast(p, key_end - p, &num_keys);
    if (n == 0) return -1;
    p += n;
    
    /* Search for matching sym_idx */
    for (int i = 0; i < (int)num_keys; i++) {
        uint64_t sym_idx, elem_idx;
        n = sexp_decode_varint_fast(p, key_end - p, &sym_idx);
        if (n == 0) return -1;
        p += n;
        n = sexp_decode_varint_fast(p, key_end - p, &elem_idx);
        if (n == 0) return -1;
        p += n;
        
        if ((int)sym_idx == target_sym_idx) {
            return (int)elem_idx;
        }
    }
    
    return -1;
}

/*
 * FAST PATH: Extract element by reusing parent's header
 *
 * Instead of converting to text and re-parsing, we copy the parent's
 * entire header (version + symbol table) and append the element data.
 * This is O(header_size + element_size) instead of O(text_conversion).
 */
uint8_t *sexp_extract_element(SexpReadState *state, const uint8_t *elem_start, 
                               const uint8_t *elem_end, size_t *out_len) {
    size_t header_len = state->header_len;
    size_t elem_size = elem_end - elem_start;
    size_t total_size = header_len + elem_size;
    
    uint8_t *result = malloc(total_size);
    if (!result) return NULL;
    
    /* Copy header from parent */
    memcpy(result, state->data, header_len);
    
    /* Copy element data */
    memcpy(result + header_len, elem_start, elem_size);
    
    *out_len = total_size;
    return result;
}

/*
 * Path traversal helper: follow a sequence of indices.
 * Returns pointer of element reached, NULL on failure.
 */
const uint8_t *sexp_path_follow(SexpReadState *state, const uint8_t *start,
                                const int *indices, int depth) {
    const uint8_t *pos = start;
    for (int i = 0; i < depth; i++) {
        if (!pos) {
            state->error = SEXP_ERR_INVALID;
            return NULL;
        }
        pos = sexp_nth(state, pos, indices[i]);
        if (!pos) {
            return NULL;
        }
    }
    return pos;
}

/* Get list info: returns count, sets pointers */
static int get_list_info(const uint8_t *p, const uint8_t *end,
                         ListHeader *hdr) {
    if (p >= end) return -1;
    
    uint8_t byte = *p++;
    uint8_t tag = byte & SEXP_TAG_MASK;
    
    if (tag == SEXP_TAG_NIL) {
        hdr->count = 0;
        hdr->sentries = NULL;
        hdr->data_start = p;
        hdr->is_large = 0;
        return 0;
    }
    
    if (tag != SEXP_TAG_LIST) return -1;
    
    return decode_list_header(&p, end, byte, hdr);
}

/*
 * sexp_car - Get first element of list
 * Returns pointer within parent data (no allocation)
 */
const uint8_t *sexp_car(SexpReadState *state, const uint8_t *list_pos) {
    ListHeader hdr;
    const uint8_t *end = state->data + state->data_len;
    
    if (get_list_info(list_pos, end, &hdr) < 0) {
        state->error = SEXP_ERR_INVALID;
        return NULL;
    }
    
    if (hdr.count == 0) {
        return NULL;
    }
    
    return hdr.data_start;
}

/*
 * sexp_nth - Get nth element (0-indexed)
 * Returns pointer within parent data (no allocation)
 */
const uint8_t *sexp_nth(SexpReadState *state, const uint8_t *list_pos, int n) {
    ListHeader hdr;
    const uint8_t *end = state->data + state->data_len;
    
    if (n < 0) {
        state->error = SEXP_ERR_BOUNDS;
        return NULL;
    }
    
    if (get_list_info(list_pos, end, &hdr) < 0) {
        state->error = SEXP_ERR_INVALID;
        return NULL;
    }
    
    if ((uint64_t)n >= hdr.count) {
        state->error = SEXP_ERR_BOUNDS;
        return NULL;
    }
    
    const uint8_t *elem_start, *elem_end;
    get_element_bounds(&hdr, n, end, &elem_start, &elem_end);
    
    return elem_start;
}

/*
 * sexp_length - Get number of elements in list
 * O(1) operation - count stored in header
 */
int sexp_length(SexpReadState *state, const uint8_t *list_pos) {
    ListHeader hdr;
    
    if (get_list_info(list_pos, state->data + state->data_len, &hdr) < 0) {
        state->error = SEXP_ERR_INVALID;
        return -1;
    }
    
    return (int)hdr.count;
}

/*
 * sexp_cdr - Get rest of list (all but first element)
 * Returns new malloc'd sexp blob
 */
uint8_t *sexp_cdr(SexpReadState *state, const uint8_t *list_pos, size_t *out_len) {
    ListHeader hdr;
    const uint8_t *end = state->data + state->data_len;
    
    if (get_list_info(list_pos, end, &hdr) < 0) {
        state->error = SEXP_ERR_INVALID;
        return NULL;
    }
    
    if (hdr.count <= 1) {
        /* Return nil */
        uint8_t *result = malloc(3);
        if (!result) {
            state->error = SEXP_ERR_MEMORY;
            return NULL;
        }
        result[0] = SEXP_FORMAT_VERSION;
        result[1] = 0;  /* No symbols */
        result[2] = SEXP_TAG_NIL;
        *out_len = 3;
        return result;
    }
    
    /* Find second element */
    const uint8_t *second, *dummy;
    get_element_bounds(&hdr, 1, end, &second, &dummy);
    
    /* Find end of all remaining elements */
    const uint8_t *elem_end = second;
    for (int i = 1; i < (int)hdr.count; i++) {
        elem_end = skip_element(elem_end, end);
        if (!elem_end) {
            state->error = SEXP_ERR_INVALID;
            return NULL;
        }
    }
    
    /* Calculate sizes */
    size_t header_len = state->header_len;
    size_t elem_data_len = elem_end - second;
    int new_count = (int)hdr.count - 1;
    
    size_t list_overhead;
    if (new_count <= SEXP_SMALLLIST_MAX) {
        /* Small list: tag + varint size */
        uint8_t tmp[10];
        int size_bytes = sexp_encode_varint(tmp, sizeof(tmp), elem_data_len);
        list_overhead = 1 + size_bytes;
    } else {
        /* Large list: tag + count + SEntry table */
        list_overhead = 1 + 4 + new_count * 4;
    }
    
    size_t total_size = header_len + list_overhead + elem_data_len;
    uint8_t *result = malloc(total_size);
    if (!result) {
        state->error = SEXP_ERR_MEMORY;
        return NULL;
    }
    
    /* Copy header */
    memcpy(result, state->data, header_len);
    
    size_t pos = header_len;
    
    /* Write list */
    if (new_count <= SEXP_SMALLLIST_MAX) {
        result[pos++] = SEXP_TAG_LIST | new_count;
        pos += sexp_encode_varint(result + pos, 10, elem_data_len);
    } else {
        result[pos++] = SEXP_TAG_LIST;
        result[pos++] = new_count & 0xFF;
        result[pos++] = (new_count >> 8) & 0xFF;
        result[pos++] = (new_count >> 16) & 0xFF;
        result[pos++] = (new_count >> 24) & 0xFF;
        
        /* Build SEntry table */
        const uint8_t *p = second;
        for (int i = 0; i < new_count; i++) {
            uint8_t elem_tag = (*p & SEXP_TAG_MASK) >> 5;
            uint32_t offset = (uint32_t)(p - second);
            uint32_t sentry = SENTRY_MAKE(elem_tag, offset);
            result[pos++] = sentry & 0xFF;
            result[pos++] = (sentry >> 8) & 0xFF;
            result[pos++] = (sentry >> 16) & 0xFF;
            result[pos++] = (sentry >> 24) & 0xFF;
            p = skip_element(p, end);
        }
    }
    
    /* Copy element data */
    memcpy(result + pos, second, elem_data_len);
    pos += elem_data_len;
    
    *out_len = pos;
    return result;
}

/*
 * Get integer value
 */
int64_t sexp_get_integer(SexpReadState *state, const uint8_t *pos) {
    if (!pos || pos >= state->data + state->data_len) {
        state->error = SEXP_ERR_BOUNDS;
        return 0;
    }
    
    uint8_t byte = *pos++;
    uint8_t tag = byte & SEXP_TAG_MASK;
    uint8_t val = byte & SEXP_VALUE_MASK;
    
    if (tag == SEXP_TAG_SMALLINT) {
        return (int64_t)val - SEXP_SMALLINT_BIAS;
    } else if (tag == SEXP_TAG_LARGEINT) {
        uint64_t zz;
        int n = sexp_decode_varint_fast(pos, state->data + state->data_len - pos, &zz);
        if (n == 0) {
            state->error = SEXP_ERR_INVALID;
            return 0;
        }
        return sexp_zigzag_decode(zz);
    }
    
    state->error = SEXP_ERR_INVALID;
    return 0;
}

/*
 * Get float value
 */
double sexp_get_float(SexpReadState *state, const uint8_t *pos) {
    if (!pos || pos + 9 > state->data + state->data_len) {
        state->error = SEXP_ERR_BOUNDS;
        return 0.0;
    }
    
    uint8_t tag = *pos & SEXP_TAG_MASK;
    if (tag != SEXP_TAG_FLOAT) {
        state->error = SEXP_ERR_INVALID;
        return 0.0;
    }
    
    double d;
    memcpy(&d, pos + 1, 8);
    return d;
}

/*
 * Get symbol name - uses pre-parsed symbol table for O(1) lookup
 */
const char *sexp_get_symbol(SexpReadState *state, const uint8_t *pos, int *len) {
    if (!pos || pos >= state->data + state->data_len) {
        state->error = SEXP_ERR_BOUNDS;
        return NULL;
    }
    
    uint8_t tag = *pos & SEXP_TAG_MASK;
    
    /* Handle standard symbol ref */
    if (tag == SEXP_TAG_SYMBOL) {
        uint64_t idx;
        int n = sexp_decode_varint_fast(pos + 1, state->data + state->data_len - pos - 1, &idx);
        if (n == 0 || (int)idx >= state->sym_count) {
            state->error = SEXP_ERR_INVALID;
            return NULL;
        }
        
        /* Ensure symbols are parsed (lazy initialization) */
        if (sexp_ensure_symbols_parsed(state) < 0) {
            return NULL;
        }
        
        /* O(1) lookup using pre-parsed symbol table */
        *len = state->sym_lens[idx];
        return state->sym_strs[idx];
    }
    
    /* Handle inline symbol (masked as NIL) */
    if (tag == SEXP_TAG_NIL && (*pos & 0x1F) == SEXP_TAG_INLINE_SYMBOL) {
        /* Inline symbol: 0x10 + varint len + bytes */
        uint64_t slen;
        int n = sexp_decode_varint_fast(pos + 1, state->data + state->data_len - pos - 1, &slen);
        if (n == 0) {
            state->error = SEXP_ERR_INVALID;
            return NULL;
        }
        *len = (int)slen;
        return (const char*)(pos + 1 + n);
    }
    
    state->error = SEXP_ERR_INVALID;
    return NULL;
}

/*
 * Get string value
 */
const char *sexp_get_string(SexpReadState *state, const uint8_t *pos, int *len) {
    if (!pos || pos >= state->data + state->data_len) {
        state->error = SEXP_ERR_BOUNDS;
        return NULL;
    }
    
    uint8_t byte = *pos++;
    uint8_t tag = byte & SEXP_TAG_MASK;
    uint8_t val = byte & SEXP_VALUE_MASK;
    
    if (tag == SEXP_TAG_SHORTSTR) {
        *len = val;
        return (const char*)pos;
    } else if (tag == SEXP_TAG_LONGSTR) {
        uint64_t slen;
        int n = sexp_decode_varint_fast(pos, state->data + state->data_len - pos, &slen);
        if (n == 0) {
            state->error = SEXP_ERR_INVALID;
            return NULL;
        }
        *len = (int)slen;
        return (const char*)(pos + n);
    }
    
    state->error = SEXP_ERR_INVALID;
    return NULL;
}

/*
 * sexp_equal - Check equality of two s-expressions
 * OPTIMIZED: Uses in-place comparison without text conversion
 */
int sexp_equal(const uint8_t *a, size_t a_len, const uint8_t *b, size_t b_len) {
    /* Quick check: binary equal = definitely equal */
    if (a_len == b_len && memcmp(a, b, a_len) == 0)
        return 1;
    
    /* Semantic comparison using in-place comparison (no allocation) */
    SexpReadState state_a, state_b;
    
    if (sexp_read_init(&state_a, a, a_len) < 0) return 0;
    if (sexp_read_init(&state_b, b, b_len) < 0) {
        sexp_read_free(&state_a);
        return 0;
    }
    
    int result = elements_equal_inplace(&state_a, state_a.root,
                                        &state_b, state_b.root);
    
    sexp_read_free(&state_a);
    sexp_read_free(&state_b);
    return result;
}

/*
 * In-place element comparison without allocation
 * Both pointers must be positioned at element start within their respective states
 */
static int elements_equal_inplace(
    SexpReadState *state_a, const uint8_t *a,
    SexpReadState *state_b, const uint8_t *b) {
    
    const uint8_t *end_a = state_a->data + state_a->data_len;
    const uint8_t *end_b = state_b->data + state_b->data_len;
    
    if (a >= end_a || b >= end_b)
        return (a >= end_a) && (b >= end_b);
    
    uint8_t byte_a = *a++;
    uint8_t byte_b = *b++;
    uint8_t tag_a = byte_a & SEXP_TAG_MASK;
    uint8_t tag_b = byte_b & SEXP_TAG_MASK;
    
    /* Different tags (with integer exception) */
    if (tag_a != tag_b) {
        /* Allow smallint/largeint cross-comparison */
        if (!((tag_a == SEXP_TAG_SMALLINT || tag_a == SEXP_TAG_LARGEINT) &&
              (tag_b == SEXP_TAG_SMALLINT || tag_b == SEXP_TAG_LARGEINT)))
            return 0;
    }
    
    switch (tag_a) {
        case SEXP_TAG_NIL:
            return tag_b == SEXP_TAG_NIL;
            
        case SEXP_TAG_SMALLINT:
            if (tag_b == SEXP_TAG_SMALLINT) {
                return (byte_a & SEXP_VALUE_MASK) == (byte_b & SEXP_VALUE_MASK);
            } else {
                /* Compare with large int */
                int64_t val_a = (byte_a & SEXP_VALUE_MASK) - SEXP_SMALLINT_BIAS;
                uint64_t zz = 0;
                sexp_decode_varint_fast(b, end_b - b, &zz);
                return val_a == sexp_zigzag_decode(zz);
            }
            
        case SEXP_TAG_LARGEINT: {
            uint64_t zz_a = 0;
            sexp_decode_varint_fast(a, end_a - a, &zz_a);
            
            if (tag_b == SEXP_TAG_SMALLINT) {
                int64_t val_b = (byte_b & SEXP_VALUE_MASK) - SEXP_SMALLINT_BIAS;
                return sexp_zigzag_decode(zz_a) == val_b;
            } else {
                uint64_t zz_b = 0;
                sexp_decode_varint_fast(b, end_b - b, &zz_b);
                return zz_a == zz_b;
            }
        }
        
        case SEXP_TAG_FLOAT:
            return memcmp(a, b, 8) == 0;
            
        case SEXP_TAG_SYMBOL: {
            uint64_t idx_a = 0, idx_b = 0;
            sexp_decode_varint_fast(a, end_a - a, &idx_a);
            sexp_decode_varint_fast(b, end_b - b, &idx_b);
            
            if ((int)idx_a >= state_a->sym_count || (int)idx_b >= state_b->sym_count)
                return 0;
            
            /* Ensure symbol tables are parsed (lazy initialization) */
            if (sexp_ensure_symbols_parsed(state_a) < 0 ||
                sexp_ensure_symbols_parsed(state_b) < 0)
                return 0;
            
            /* Fast path: compare hashes first */
            if (state_a->sym_hashes && state_b->sym_hashes) {
                if (state_a->sym_hashes[idx_a] != state_b->sym_hashes[idx_b])
                    return 0;
            }
            
            /* Compare lengths */
            if (state_a->sym_lens[idx_a] != state_b->sym_lens[idx_b])
                return 0;
            
            /* Full string comparison */
            return memcmp(state_a->sym_strs[idx_a], state_b->sym_strs[idx_b],
                         state_a->sym_lens[idx_a]) == 0;
        }
        
        case SEXP_TAG_SHORTSTR: {
            int len_a = byte_a & SEXP_VALUE_MASK;
            int len_b = byte_b & SEXP_VALUE_MASK;
            if (len_a != len_b) return 0;
            return memcmp(a, b, len_a) == 0;
        }
        
        case SEXP_TAG_LONGSTR: {
            uint64_t len_a = 0, len_b = 0;
            int n_a = sexp_decode_varint_fast(a, end_a - a, &len_a);
            int n_b = sexp_decode_varint_fast(b, end_b - b, &len_b);
            if (len_a != len_b) return 0;
            return memcmp(a + n_a, b + n_b, len_a) == 0;
        }
        
        case SEXP_TAG_LIST: {
            ListHeader hdr_a, hdr_b;
            const uint8_t *p_a = a - 1, *p_b = b - 1;  /* Include tag byte */
            
            if (get_list_info(p_a, end_a, &hdr_a) < 0) return 0;
            if (get_list_info(p_b, end_b, &hdr_b) < 0) return 0;
            
            if (hdr_a.count != hdr_b.count) return 0;
            
            /* Compare each element */
            for (uint64_t i = 0; i < hdr_a.count; i++) {
                const uint8_t *ea_start, *ea_end, *eb_start, *eb_end;
                get_element_bounds(&hdr_a, (int)i, end_a, &ea_start, &ea_end);
                get_element_bounds(&hdr_b, (int)i, end_b, &eb_start, &eb_end);
                
                if (!elements_equal_inplace(state_a, ea_start, state_b, eb_start))
                    return 0;
            }
            return 1;
        }
        
        default:
            return 0;
    }
}

/*
 * Recursive containment check - no allocation in hot path
 */
static int contains_recursive(
    SexpReadState *hay_state, const uint8_t *hay_pos,
    SexpReadState *needle_state, const uint8_t *needle_pos) {
    
    const uint8_t *hay_end = hay_state->data + hay_state->data_len;
    
    /* Check if needle matches at current position */
    if (elements_equal_inplace(hay_state, hay_pos, needle_state, needle_pos))
        return 1;
    
    /* If haystack is a list, recurse into children */
    uint8_t tag = *hay_pos & SEXP_TAG_MASK;
    if (tag == SEXP_TAG_LIST) {
        ListHeader hdr;
        if (get_list_info(hay_pos, hay_end, &hdr) < 0)
            return 0;
        
        for (uint64_t i = 0; i < hdr.count; i++) {
            const uint8_t *child_start, *child_end;
            get_element_bounds(&hdr, (int)i, hay_end, &child_start, &child_end);
            
            if (contains_recursive(hay_state, child_start, needle_state, needle_pos))
                return 1;
        }
    }
    
    return 0;
}

/*
 * sexp_contains - Check if needle is contained in haystack
 */
int sexp_contains(const uint8_t *haystack, size_t h_len,
                  const uint8_t *needle, size_t n_len) {
    SexpReadState hay_state, needle_state;
    
    if (sexp_read_init(&hay_state, haystack, h_len) < 0) return 0;
    if (sexp_read_init(&needle_state, needle, n_len) < 0) {
        sexp_read_free(&hay_state);
        return 0;
    }
    
    /* BLOOM FILTER FAST REJECTION */
    BloomSig container_bloom = sexp_element_bloom(&hay_state, hay_state.root);
    BloomSig elem_bloom = sexp_element_bloom(&needle_state, needle_state.root);
    
    if (!bloom_may_contain(container_bloom, elem_bloom)) {
        sexp_read_free(&hay_state);
        sexp_read_free(&needle_state);
        return 0;  /* Bloom says NO - definitely not contained */
    }
    
    int result = contains_recursive(&hay_state, hay_state.root,
                                    &needle_state, needle_state.root);
    
    sexp_read_free(&hay_state);
    sexp_read_free(&needle_state);
    
    return result;
}

/*
 * BLOOM SIGNATURE COMPUTATION
 *
 * Each element contributes its semantic hash to the signature.
 * Lists combine all descendant signatures (union), so checking the list's bloom
 * against the element's bloom gives a fast NO answer.
 */

/*
 * Compute semantic hash of an element (must be independent of symbol table indices)
 */
static uint32_t element_semantic_hash(SexpReadState *state, const uint8_t *pos) {
    const uint8_t *end = state->data + state->data_len;
    
    if (pos >= end) return 0;
    
    uint8_t byte = *pos++;
    uint8_t tag = byte & SEXP_TAG_MASK;
    uint8_t val = byte & SEXP_VALUE_MASK;
    
    switch (tag) {
        case SEXP_TAG_NIL:
            return sexp_hash_uint32(SEXP_TAG_NIL);
            
        case SEXP_TAG_SMALLINT: {
            int64_t ival = (int64_t)val - SEXP_SMALLINT_BIAS;
            return sexp_hash_combine(SEXP_TAG_SMALLINT, sexp_hash_uint32((uint32_t)ival));
        }
            
        case SEXP_TAG_LARGEINT: {
            uint64_t zz = 0;
            sexp_decode_varint_fast(pos, end - pos, &zz);
            int64_t ival = sexp_zigzag_decode(zz);
            return sexp_hash_combine(SEXP_TAG_LARGEINT, 
                                     sexp_hash_uint32((uint32_t)ival) ^ 
                                     sexp_hash_uint32((uint32_t)(ival >> 32)));
        }
            
        case SEXP_TAG_FLOAT: {
            double d;
            memcpy(&d, pos, 8);
            uint64_t bits;
            memcpy(&bits, &d, 8);
            return sexp_hash_combine(SEXP_TAG_FLOAT,
                                     sexp_hash_uint32((uint32_t)bits) ^
                                     sexp_hash_uint32((uint32_t)(bits >> 32)));
        }
            
        case SEXP_TAG_SYMBOL: {
            /* Must hash the actual symbol string, not the index! */
            uint64_t idx = 0;
            sexp_decode_varint_fast(pos, end - pos, &idx);
            if ((int)idx < state->sym_count) {
                if (sexp_ensure_symbols_parsed(state) == 0) {
                    return sexp_hash_string_with_tag(SEXP_TAG_SYMBOL, 
                                                     state->sym_strs[idx], 
                                                     state->sym_lens[idx]);
                }
            }
            return sexp_hash_uint32(SEXP_TAG_SYMBOL);
        }
            
        case SEXP_TAG_SHORTSTR:
            return sexp_hash_string_with_tag(SEXP_TAG_SHORTSTR, (const char*)pos, val);
            
        case SEXP_TAG_LONGSTR: {
            uint64_t len = 0;
            int n = sexp_decode_varint_fast(pos, end - pos, &len);
            return sexp_hash_string_with_tag(SEXP_TAG_LONGSTR, (const char*)(pos + n), (int)len);
        }
            
        case SEXP_TAG_LIST: {
            /* For lists, combine hashes of all elements */
            ListHeader hdr;
            const uint8_t *list_start = pos - 1;
            if (get_list_info(list_start, end, &hdr) < 0) {
                return sexp_hash_uint32(SEXP_TAG_LIST);
            }
            
            uint32_t list_hash = sexp_hash_uint32(SEXP_TAG_LIST);
            for (uint64_t i = 0; i < hdr.count; i++) {
                const uint8_t *elem_start, *elem_end;
                get_element_bounds(&hdr, (int)i, end, &elem_start, &elem_end);
                list_hash = sexp_hash_combine(list_hash, element_semantic_hash(state, elem_start));
            }
            return list_hash;
        }
            
        default:
            return 0;
    }
}

/*
 * sexp_element_bloom - Compute Bloom signature for an element
 */
BloomSig sexp_element_bloom(SexpReadState *state, const uint8_t *pos) {
    const uint8_t *end = state->data + state->data_len;
    
    if (pos >= end) return 0;
    
    uint8_t byte = *pos;
    uint8_t tag = byte & SEXP_TAG_MASK;
    
    /* For atoms, just compute signature from semantic hash */
    if (tag != SEXP_TAG_LIST) {
        return bloom_compute_sig(element_semantic_hash(state, pos));
    }
    
    /* For lists, combine all descendant signatures */
    ListHeader hdr;
    if (get_list_info(pos, end, &hdr) < 0) {
        return bloom_compute_sig(sexp_hash_uint32(SEXP_TAG_LIST));
    }
    
    /* Start with the list's own hash */
    BloomSig list_bloom = bloom_compute_sig(element_semantic_hash(state, pos));
    
    /* Add signatures of all children (recursively) */
    for (uint64_t i = 0; i < hdr.count; i++) {
        const uint8_t *elem_start, *elem_end;
        get_element_bounds(&hdr, (int)i, end, &elem_start, &elem_end);
        list_bloom = bloom_combine(list_bloom, sexp_element_bloom(state, elem_start));
    }
    
    return list_bloom;
}

/*
 * KEY-BASED CONTAINMENT (like JSONB @> operator)
 *
 * Treats list heads as "keys" and remaining elements as "values".
 * Order-independent matching for value elements.
 *
 * Example:
 *   (user (name "alice") (age 30)) contains_key (user (age 30))  -> TRUE
 *   (user (name "alice") (age 30)) contains_key (user (name "bob")) -> FALSE
 *   (+ 1 2 3) contains_key (+ 2 1) -> TRUE (order independent!)
 */

/* Forward declarations */
static int key_contains_recursive(
    SexpReadState *container_state, const uint8_t *container_pos,
    SexpReadState *needle_state, const uint8_t *needle_pos);

static int element_key_matches(
    SexpReadState *container_state, const uint8_t *container_pos,
    SexpReadState *needle_state, const uint8_t *needle_pos);

/*
 * Check if needle element matches container element using key semantics.
 * - For atoms: exact equality
 * - For lists: head must match, tail elements matched in any order using key semantics
 */
static int element_key_matches(
    SexpReadState *container_state, const uint8_t *container_pos,
    SexpReadState *needle_state, const uint8_t *needle_pos) {
    
    const uint8_t *c_end = container_state->data + container_state->data_len;
    const uint8_t *n_end = needle_state->data + needle_state->data_len;
    
    if (container_pos >= c_end || needle_pos >= n_end) {
        return (container_pos >= c_end) && (needle_pos >= n_end);
    }
    
    uint8_t c_tag = *container_pos & SEXP_TAG_MASK;
    uint8_t n_tag = *needle_pos & SEXP_TAG_MASK;
    
    /* Handle NIL (empty list) needle specially - matches any list or nil */
    if (n_tag == SEXP_TAG_NIL && (*needle_pos & SEXP_VALUE_MASK) == 0) {
        /* Empty list matches nil or any list */
        return (c_tag == SEXP_TAG_NIL && (*container_pos & SEXP_VALUE_MASK) == 0) ||
               (c_tag == SEXP_TAG_LIST);
    }
    
    /* Non-list atoms must be exactly equal */
    if (n_tag != SEXP_TAG_LIST) {
        return elements_equal_inplace(container_state, container_pos,
                                      needle_state, needle_pos);
    }
    
    /* Needle is a list - container must also be a list */
    if (c_tag != SEXP_TAG_LIST) {
        return 0;
    }
    
    ListHeader c_hdr, n_hdr;
    if (get_list_info(container_pos, c_end, &c_hdr) < 0) return 0;
    if (get_list_info(needle_pos, n_end, &n_hdr) < 0) return 0;
    
    /* Empty needle list matches any list */
    if (n_hdr.count == 0) {
        return 1;
    }
    
    /* Container must have at least as many elements */
    if (c_hdr.count < n_hdr.count) {
        return 0;
    }
    
    /* First element (head) must match exactly */
    const uint8_t *c_head_start, *c_head_end;
    const uint8_t *n_head_start, *n_head_end;
    get_element_bounds(&c_hdr, 0, c_end, &c_head_start, &c_head_end);
    get_element_bounds(&n_hdr, 0, n_end, &n_head_start, &n_head_end);
    
    if (!elements_equal_inplace(container_state, c_head_start,
                                needle_state, n_head_start)) {
        return 0;
    }
    
    /* For each tail element in needle, find matching element in container tail */
    /* Use a simple O(n*m) algorithm with a "used" bitmask for container elements */
    uint64_t used_mask = 1;  /* Bit 0 = head, already matched */
    
    for (uint64_t ni = 1; ni < n_hdr.count; ni++) {
        const uint8_t *n_elem_start, *n_elem_end;
        get_element_bounds(&n_hdr, (int)ni, n_end, &n_elem_start, &n_elem_end);
        
        int found = 0;
        for (uint64_t ci = 1; ci < c_hdr.count && !found; ci++) {
            if (used_mask & ((uint64_t)1 << ci)) continue;  /* Already used */
            
            const uint8_t *c_elem_start, *c_elem_end;
            get_element_bounds(&c_hdr, (int)ci, c_end, &c_elem_start, &c_elem_end);
            
            /* Recursive key match for nested structures */
            if (element_key_matches(container_state, c_elem_start,
                                    needle_state, n_elem_start)) {
                used_mask |= ((uint64_t)1 << ci);
                found = 1;
            }
        }
        
        if (!found) {
            return 0;  /* Needle element not found in container */
        }
    }
    
    return 1;
}

/*
 * Get the head element of a list (for fast head comparison).
 * Returns NULL if not a list or empty.
 */
static const uint8_t *get_list_head(const uint8_t *pos, const uint8_t *end) {
    if (pos >= end) return NULL;
    uint8_t tag = *pos & SEXP_TAG_MASK;
    if (tag != SEXP_TAG_LIST) return NULL;
    
    ListHeader hdr;
    if (get_list_info(pos, end, &hdr) < 0 || hdr.count == 0) return NULL;
    
    return hdr.data_start;
}

/*
 * Recursive key-based containment search
 * Returns 1 if needle is key-contained anywhere in container
 * 
 * OPTIMIZATION: If needle is a list starting with a symbol, we only need
 * to recurse into container sublists that also start with that symbol.
 * This avoids full tree traversal for queries like (user (type premium)).
 */
static int key_contains_recursive(
    SexpReadState *container_state, const uint8_t *container_pos,
    SexpReadState *needle_state, const uint8_t *needle_pos) {
    
    const uint8_t *c_end = container_state->data + container_state->data_len;
    const uint8_t *n_end = needle_state->data + needle_state->data_len;
    
    /* Check if needle key-matches at current position */
    if (element_key_matches(container_state, container_pos,
                            needle_state, needle_pos)) {
        return 1;
    }
    
    /* If container is not a list, we're done (no children to check) */
    uint8_t c_tag = *container_pos & SEXP_TAG_MASK;
    if (c_tag != SEXP_TAG_LIST) {
        return 0;
    }
    
    ListHeader c_hdr;
    if (get_list_info(container_pos, c_end, &c_hdr) < 0) {
        return 0;
    }
    
    /* OPTIMIZATION: If needle is a list starting with a symbol,
     * we can skip container children that don't start with that symbol. */
    uint8_t n_tag = *needle_pos & SEXP_TAG_MASK;
    const uint8_t *needle_head = NULL;
    int needle_head_is_symbol = 0;
    
    if (n_tag == SEXP_TAG_LIST) {
        needle_head = get_list_head(needle_pos, n_end);
        if (needle_head && (*needle_head & SEXP_TAG_MASK) == SEXP_TAG_SYMBOL) {
            needle_head_is_symbol = 1;
        }
    }
    
    for (uint64_t i = 0; i < c_hdr.count; i++) {
        const uint8_t *child_start, *child_end;
        get_element_bounds(&c_hdr, (int)i, c_end, &child_start, &child_end);
        
        /* Fast path: if needle starts with a symbol, skip children that
         * are lists not starting with that same symbol */
        if (needle_head_is_symbol) {
            uint8_t child_tag = *child_start & SEXP_TAG_MASK;
            if (child_tag == SEXP_TAG_LIST) {
                const uint8_t *child_head = get_list_head(child_start, c_end);
                if (child_head) {
                    uint8_t child_head_tag = *child_head & SEXP_TAG_MASK;
                    /* Only recurse if child head is also a symbol (for potential match)
                     * or if child head is a list (child might contain matching subtree) */
                    if (child_head_tag == SEXP_TAG_SYMBOL) {
                        /* Quick symbol comparison */
                        if (!elements_equal_inplace(container_state, child_head,
                                                    needle_state, needle_head)) {
                            /* Head symbols don't match - but still need to check 
                             * grandchildren for the needle */
                            /* Recurse only into sublists, not this child */
                            ListHeader child_hdr;
                            if (get_list_info(child_start, c_end, &child_hdr) >= 0) {
                                for (uint64_t j = 0; j < child_hdr.count; j++) {
                                    const uint8_t *gc_start, *gc_end;
                                    get_element_bounds(&child_hdr, (int)j, c_end, &gc_start, &gc_end);
                                    if (key_contains_recursive(container_state, gc_start,
                                                               needle_state, needle_pos)) {
                                        return 1;
                                    }
                                }
                            }
                            continue;
                        }
                    }
                }
            }
        }
        
        if (key_contains_recursive(container_state, child_start,
                                    needle_state, needle_pos)) {
            return 1;
        }
    }
    
    return 0;
}

/*
 * Get the symbol index of the head of a list (for index lookup).
 * Returns -1 if not a list or head is not a symbol.
 */
static int get_list_head_sym_index(SexpReadState *state, const uint8_t *pos) {
    const uint8_t *end = state->data + state->data_len;
    if (pos >= end) return -1;
    
    uint8_t tag = *pos & SEXP_TAG_MASK;
    if (tag != SEXP_TAG_LIST) return -1;
    
    ListHeader hdr;
    if (get_list_info(pos, end, &hdr) < 0 || hdr.count == 0) return -1;
    
    const uint8_t *head_start, *head_end;
    get_element_bounds(&hdr, 0, end, &head_start, &head_end);
    
    uint8_t head_tag = *head_start & SEXP_TAG_MASK;
    if (head_tag != SEXP_TAG_SYMBOL) return -1;
    
    uint64_t sym_idx = 0;
    sexp_decode_varint_fast(head_start + 1, end - head_start - 1, &sym_idx);
    return (int)sym_idx;
}

/*
 * Fast symbol name lookup for cross-state comparison.
 * Returns symbol string, length, and hash if available.
 */
static const char *get_symbol_info(SexpReadState *state, int sym_idx, 
                                    int *len_out, uint32_t *hash_out) {
    if (sym_idx < 0 || sym_idx >= state->sym_count) return NULL;
    if (sexp_ensure_symbols_parsed(state) < 0) return NULL;
    
    *len_out = state->sym_lens[sym_idx];
    if (hash_out && state->sym_hashes) {
        *hash_out = state->sym_hashes[sym_idx];
    }
    return state->sym_strs[sym_idx];
}

/*
 * Check if haystack symbol matches needle symbol (cross-state comparison).
 */
static int symbols_match_cross(SexpReadState *hay_state, int hay_sym_idx,
                                SexpReadState *needle_state, int needle_sym_idx) {
    int hay_len, needle_len;
    uint32_t hay_hash = 0, needle_hash = 0;
    
    const char *hay_str = get_symbol_info(hay_state, hay_sym_idx, &hay_len, &hay_hash);
    const char *needle_str = get_symbol_info(needle_state, needle_sym_idx, &needle_len, &needle_hash);
    
    if (!hay_str || !needle_str) return 0;
    
    /* Fast path: compare hashes first */
    if (hay_hash && needle_hash && hay_hash != needle_hash) return 0;
    if (hay_len != needle_len) return 0;
    
    return memcmp(hay_str, needle_str, hay_len) == 0;
}

/*
 * Optimized key-based containment search.
 * 
 * When needle is a list starting with a symbol (the common case),
 * we use an optimized search that:
 * 1. Uses key index for O(1) lookup when available
 * 2. Skips branches that don't match the needle's head symbol
 * 3. Uses symbol index comparison instead of full element comparison
 */
static int key_contains_optimized(
    SexpReadState *haystack_state, const uint8_t *hay_pos,
    SexpReadState *needle_state, const uint8_t *needle_pos,
    int needle_head_sym_idx, const char *needle_sym_str, int needle_sym_len,
    uint32_t needle_sym_hash) {
    
    const uint8_t *h_end = haystack_state->data + haystack_state->data_len;
    if (hay_pos >= h_end) return 0;
    
    uint8_t hay_tag = *hay_pos & SEXP_TAG_MASK;
    
    /* If haystack position is a list starting with a symbol */
    if (hay_tag == SEXP_TAG_LIST) {
        int hay_head_sym = get_list_head_sym_index(haystack_state, hay_pos);
        
        if (hay_head_sym >= 0) {
            /* Compare head symbols */
            if (symbols_match_cross(haystack_state, hay_head_sym,
                                    needle_state, needle_head_sym_idx)) {
                /* Head symbols match - check if full needle matches */
                if (element_key_matches(haystack_state, hay_pos,
                                        needle_state, needle_pos)) {
                    return 1;
                }
            }
        }
        
        /* Get list header to access children */
        ListHeader hdr;
        memset(&hdr, 0, sizeof(hdr));
        if (get_list_info(hay_pos, h_end, &hdr) >= 0) {
            /* OPTIMIZATION: If this list has a key index, use it for O(1) lookup */
            if (hdr.has_key_index) {
                /* Find the matching symbol in haystack's symbol table */
                int hay_sym_idx = -1;
                for (int i = 0; i < haystack_state->sym_count; i++) {
                    if (haystack_state->sym_hashes && 
                        haystack_state->sym_hashes[i] == needle_sym_hash &&
                        haystack_state->sym_lens[i] == needle_sym_len &&
                        memcmp(haystack_state->sym_strs[i], needle_sym_str, needle_sym_len) == 0) {
                        hay_sym_idx = i;
                        break;
                    }
                }
                
                if (hay_sym_idx >= 0) {
                    /* Look up in key index */
                    int elem_idx = lookup_key_index(&hdr, hay_sym_idx);
                    if (elem_idx >= 0 && elem_idx < (int)hdr.count) {
                        /* Found! Check if it matches */
                        const uint8_t *child_start, *child_end;
                        get_element_bounds(&hdr, elem_idx, h_end, &child_start, &child_end);
                        
                        if (element_key_matches(haystack_state, child_start,
                                                needle_state, needle_pos)) {
                            return 1;
                        }
                        /* Also recurse into that child for deeper matches */
                        if (key_contains_optimized(haystack_state, child_start,
                                                   needle_state, needle_pos,
                                                   needle_head_sym_idx, needle_sym_str,
                                                   needle_sym_len, needle_sym_hash)) {
                            return 1;
                        }
                    }
                }
                /* Key not in index, but still recurse into non-keyed children */
            }
            
            /* Recurse into all children (fallback or for lists without key index) */
            for (uint64_t i = 0; i < hdr.count; i++) {
                const uint8_t *child_start, *child_end;
                get_element_bounds(&hdr, (int)i, h_end, &child_start, &child_end);
                
                if (key_contains_optimized(haystack_state, child_start,
                                           needle_state, needle_pos,
                                           needle_head_sym_idx, needle_sym_str,
                                           needle_sym_len, needle_sym_hash)) {
                    return 1;
                }
            }
        }
    }
    
    return 0;
}

/*
 * sexp_contains_key_indexed - Key-based containment with optimized lookup
 *
 * Uses key index for O(1) lookup when available.
 */
int sexp_contains_key_indexed(SexpReadState *haystack_state,
                               SexpReadState *needle_state) {
    /* Get needle's head symbol (if it's a list starting with a symbol) */
    int needle_sym_idx = get_list_head_sym_index(needle_state, needle_state->root);
    
    if (needle_sym_idx >= 0) {
        /* Get needle's symbol info for key index lookups */
        if (sexp_ensure_symbols_parsed(needle_state) < 0 ||
            sexp_ensure_symbols_parsed(haystack_state) < 0) {
            /* Fall back to recursive search */
            return key_contains_recursive(haystack_state, haystack_state->root,
                                          needle_state, needle_state->root);
        }
        
        const char *needle_sym_str = needle_state->sym_strs[needle_sym_idx];
        int needle_sym_len = needle_state->sym_lens[needle_sym_idx];
        uint32_t needle_sym_hash = needle_state->sym_hashes ? 
                                    needle_state->sym_hashes[needle_sym_idx] : 0;
        
        /* Use optimized search with key index support */
        return key_contains_optimized(haystack_state, haystack_state->root,
                                      needle_state, needle_state->root,
                                      needle_sym_idx, needle_sym_str,
                                      needle_sym_len, needle_sym_hash);
    }
    
    /* Fall back to recursive search */
    return key_contains_recursive(haystack_state, haystack_state->root,
                                  needle_state, needle_state->root);
}

/*
 * sexp_contains_key_with_state - Key-based containment with pre-parsed needle
 *
 * This is the optimized version that takes a pre-parsed needle state.
 * Use this when the needle is constant across many calls (e.g., in SQL queries).
 *
 * Uses symbol comparison optimization when needle starts with a symbol.
 */
int sexp_contains_key_with_state(const uint8_t *haystack, size_t h_len,
                                  SexpReadState *needle_state) {
    SexpReadState hay_state;
    
    if (sexp_read_init(&hay_state, haystack, h_len) < 0) return 0;
    
    int result = sexp_contains_key_indexed(&hay_state, needle_state);
    
    sexp_read_free(&hay_state);
    
    return result;
}

/*
 * sexp_contains_key - Key-based containment check (like JSONB @>)
 *
 * Convenience wrapper that parses both haystack and needle.
 * For better performance with constant needles, use sexp_contains_key_with_state().
 */
int sexp_contains_key(const uint8_t *haystack, size_t h_len,
                      const uint8_t *needle, size_t n_len) {
    SexpReadState needle_state;
    
    if (sexp_read_init(&needle_state, needle, n_len) < 0) return 0;
    
    int result = sexp_contains_key_with_state(haystack, h_len, &needle_state);
    
    sexp_read_free(&needle_state);
    
    return result;
}

/*
 * sexp_get_by_key - Find value by key in alist-style s-expression
 *
 * Searches for a sublist whose head matches the given key (symbol or string),
 * and returns a pointer to the second element (the value).
 *
 * Alist structure: ((key1 value1) (key2 value2) ...)
 * or: (tag (key1 value1) (key2 value2) ...)
 *
 * For lists like: (user (name "alice") (age 30))
 *   - sexp_get_by_key(..., "name", 4) returns pointer to "alice"
 *   - sexp_get_by_key(..., "age", 3) returns pointer to 30
 *
 * Returns pointer to value element, or NULL if not found.
 */
const uint8_t *sexp_get_by_key(SexpReadState *state, const uint8_t *list_pos,
                                const char *key, int key_len,
                                const uint8_t **value_end) {
    const uint8_t *end = state->data + state->data_len;
    
    if (!list_pos || list_pos >= end) {
        state->error = SEXP_ERR_BOUNDS;
        return NULL;
    }
    
    /* Must be a list */
    uint8_t tag = *list_pos & SEXP_TAG_MASK;
    if (tag != SEXP_TAG_LIST) {
        /* Could also be nil (empty list) */
        if (tag == SEXP_TAG_NIL) {
            return NULL;  /* Empty, key not found */
        }
        state->error = SEXP_ERR_INVALID;
        return NULL;
    }
    
    ListHeader hdr;
    if (get_list_info(list_pos, end, &hdr) < 0) {
        state->error = SEXP_ERR_INVALID;
        return NULL;
    }
    
    /* Pre-compute key hash for fast rejection */
    uint32_t key_hash = sexp_hash_bytes(key, key_len);
    
    /* Ensure symbols are parsed for fast lookup */
    if (sexp_ensure_symbols_parsed(state) < 0) {
        return NULL;
    }
    
    /* Search through elements looking for a sublist whose head matches key */
    for (uint64_t i = 0; i < hdr.count; i++) {
        const uint8_t *elem_start, *elem_end;
        get_element_bounds(&hdr, (int)i, end, &elem_start, &elem_end);
        
        /* Element must be a list */
        uint8_t elem_tag = *elem_start & SEXP_TAG_MASK;
        if (elem_tag != SEXP_TAG_LIST) continue;
        
        ListHeader sub_hdr;
        if (get_list_info(elem_start, end, &sub_hdr) < 0) continue;
        
        /* Must have at least 2 elements: (key value ...) */
        if (sub_hdr.count < 2) continue;
        
        /* Get head (the key) */
        const uint8_t *head_start = sub_hdr.data_start;
        uint8_t head_tag = *head_start & SEXP_TAG_MASK;
        
        int matches = 0;
        
        /* Check if head is a symbol matching our key */
        if (head_tag == SEXP_TAG_SYMBOL) {
            uint64_t idx = 0;
            sexp_decode_varint_fast(head_start + 1, end - head_start - 1, &idx);
            
            if ((int)idx < state->sym_count) {
                /* Fast path: compare hash first */
                if (state->sym_hashes && state->sym_hashes[idx] == key_hash) {
                    /* Hash matches, verify full string */
                    if (state->sym_lens[idx] == key_len &&
                        memcmp(state->sym_strs[idx], key, key_len) == 0) {
                        matches = 1;
                    }
                } else if (!state->sym_hashes) {
                    /* No hashes, direct compare */
                    if (state->sym_lens[idx] == key_len &&
                        memcmp(state->sym_strs[idx], key, key_len) == 0) {
                        matches = 1;
                    }
                }
            }
        }
        /* Also check for inline symbol */
        else if (head_tag == SEXP_TAG_NIL && 
                 (*head_start & SEXP_VALUE_MASK) == SEXP_TAG_INLINE_SYMBOL) {
            uint64_t slen = 0;
            int n = sexp_decode_varint_fast(head_start + 1, end - head_start - 1, &slen);
            if (n > 0 && (int)slen == key_len) {
                const char *sym_str = (const char*)(head_start + 1 + n);
                if (memcmp(sym_str, key, key_len) == 0) {
                    matches = 1;
                }
            }
        }
        /* Also check for string keys */
        else if (head_tag == SEXP_TAG_SHORTSTR) {
            int slen = *head_start & SEXP_VALUE_MASK;
            if (slen == key_len && memcmp(head_start + 1, key, key_len) == 0) {
                matches = 1;
            }
        }
        else if (head_tag == SEXP_TAG_LONGSTR) {
            uint64_t slen = 0;
            int n = sexp_decode_varint_fast(head_start + 1, end - head_start - 1, &slen);
            if (n > 0 && (int)slen == key_len) {
                if (memcmp(head_start + 1 + n, key, key_len) == 0) {
                    matches = 1;
                }
            }
        }
        
        if (matches) {
            /* Found! Determine what to return based on sublist structure */
            if (sub_hdr.count == 2) {
                /* Simple key-value pair: (key value) -> return value */
                const uint8_t *val_start, *val_end_ptr;
                get_element_bounds(&sub_hdr, 1, end, &val_start, &val_end_ptr);
                
                if (value_end) {
                    *value_end = val_end_ptr;
                }
                return val_start;
            } else {
                /* Nested object: (key prop1 prop2 ...) -> return whole sublist
                 * The caller will use sexp_cdr or handle the structure.
                 * We return the whole sublist so caller can process it as an object. */
                if (value_end) {
                    /* Find end of the sublist */
                    const uint8_t *last_start, *last_end;
                    get_element_bounds(&sub_hdr, (int)sub_hdr.count - 1, end, &last_start, &last_end);
                    *value_end = last_end;
                }
                return elem_start;  /* Return the whole (key prop1 prop2 ...) sublist */
            }
        }
    }
    
    /* Key not found */
    return NULL;
}
