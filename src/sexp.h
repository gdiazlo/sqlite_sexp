/*
 * sexp.h - S-expression data type for SQLite
 *
 * Binary format specification (version 1):
 *
 * Root structure:
 *   [1 byte: version]           - SEXP_FORMAT_VERSION (1)
 *   [varint: symbol_count]      - Number of unique symbols
 *   [symbol_table entries...]   - Each: [varint: length][bytes: symbol chars]
 *   [root_element]              - The actual s-expression data
 *
 * Type tags (3-bit, stored in bits 7-5):
 *   000 (0x00) = NIL
 *   001 (0x20) = Small integer (-16 to 15, value in bits 4-0)
 *   010 (0x40) = Large integer (zigzag-encoded varint follows)
 *   011 (0x60) = Float (8-byte IEEE 754 double follows)
 *   100 (0x80) = Symbol reference (varint symbol index follows)
 *   101 (0xA0) = Short string (length 0-31 in bits 4-0, then bytes)
 *   110 (0xC0) = Long string (varint length follows, then bytes)
 *   111 (0xE0) = List (format depends on element count)
 *
 * List formats:
 *   Small lists (1-4 elements):
 *     [0xE0 | count]              - Tag with count in lower 5 bits
 *     [varint: payload_size]      - Total size of element data
 *     [element_0]...[element_N-1] - Sequential element data
 *
 *   Large lists (5+ elements):
 *     [0xE0]                      - Tag byte with count=0
 *     [uint32: count]             - 4 bytes: element count
 *     [SEntry[0]..SEntry[count-1]] - 4*count bytes: type+offset table
 *     [element_0]...[element_N-1] - Element data
 *
 *   Keyed lists (lists with symbol-headed sublists, for fast key lookup):
 *     [0xE0 | SEXP_LIST_HAS_KEYIDX | count] - Tag with key index flag
 *     [varint: payload_size]      - Total size including key index
 *     [varint: num_keys]          - Number of key entries
 *     [key_entry...]              - Each: [varint: sym_idx][varint: elem_idx]
 *     [element_0]...[element_N-1] - Sequential element data
 *
 *   The key index maps symbol indices to element indices for sublists
 *   that start with that symbol. This enables O(1) key lookup.
 *
 * SEntry format (32-bit):
 *   Bits 31-29: Type tag (3 bits)
 *   Bits 28-0:  Offset from data start (29 bits)
 */

#ifndef SEXP_H
#define SEXP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Format version */
#define SEXP_FORMAT_VERSION 1

/* Type tags (bits 7-5) */
#define SEXP_TAG_NIL         0x00
#define SEXP_TAG_INLINE_SYMBOL 0x10 /* Stored in unused bits of NIL tag space */
#define SEXP_TAG_SMALLINT    0x20
#define SEXP_TAG_LARGEINT    0x40
#define SEXP_TAG_FLOAT       0x60
#define SEXP_TAG_SYMBOL      0x80
#define SEXP_TAG_SHORTSTR    0xA0
#define SEXP_TAG_LONGSTR     0xC0
#define SEXP_TAG_LIST        0xE0

/* Masks */
#define SEXP_TAG_MASK        0xE0   /* Top 3 bits */
#define SEXP_VALUE_MASK      0x1F   /* Bottom 5 bits */

/* Small integer range: -16 to 15 (5-bit signed) */
#define SEXP_SMALLINT_MIN    (-16)
#define SEXP_SMALLINT_MAX    15
#define SEXP_SMALLINT_BIAS   16     /* Stored as value + 16 */

/* Short string max length */
#define SEXP_SHORTSTR_MAX    31

/* Small list max elements */
#define SEXP_SMALLLIST_MAX   4

/* Large list threshold */
#define SEXP_LARGELIST_MIN   5

/* List flag: has key index (bit 4 in list tag byte) */
#define SEXP_LIST_HAS_KEYIDX 0x10

/* SEntry field extraction */
#define SENTRY_TAG(e)        (((e) >> 29) & 0x7)
#define SENTRY_OFFSET(e)     ((e) & 0x1FFFFFFF)
#define SENTRY_MAKE(tag, off) ((((uint32_t)(tag) & 0x7) << 29) | ((off) & 0x1FFFFFFF))

/* Limits */
#define SEXP_MAX_DEPTH       1000
#define SEXP_MAX_SYMBOLS     65536
#define SEXP_MAX_STRING      (100 * 1024 * 1024)  /* 100MB */

/* SQLite subtype for sexp blobs (enables fast path in chained calls) */
#define SEXP_SUBTYPE         0x73  /* 's' for sexp */

/* Type enumeration for API use */
typedef enum {
    SEXP_TYPE_NIL = 0,
    SEXP_TYPE_INTEGER,
    SEXP_TYPE_FLOAT,
    SEXP_TYPE_SYMBOL,
    SEXP_TYPE_STRING,
    SEXP_TYPE_LIST
} SexpType;

/* Error codes */
typedef enum {
    SEXP_OK = 0,
    SEXP_ERR_SYNTAX,
    SEXP_ERR_OVERFLOW,
    SEXP_ERR_MEMORY,
    SEXP_ERR_DEPTH,
    SEXP_ERR_INVALID,
    SEXP_ERR_BOUNDS
} SexpError;

/* Symbol table for parsing */
typedef struct {
    const char **symbols;   /* Array of pointers into input (zero-copy) */
    int       *lengths;     /* Length of each symbol */
    uint32_t  *hashes;      /* Pre-computed hash of each symbol */
    int        count;       /* Number of symbols */
    int        capacity;    /* Allocated capacity */
    int       *hash_table;  /* Maps hash slot -> symbol index (-1 if empty) */
    int        hash_size;   /* Size of hash table (power of 2) */
} SexpSymbolTable;

/* Parse state */
typedef struct {
    const char     *input;      /* Input string */
    const char     *pos;        /* Current position */
    const char     *end;        /* End of input */
    SexpSymbolTable symtab;     /* Symbol table */
    uint8_t        *output;     /* Output buffer */
    size_t          output_len; /* Output length */
    size_t          output_cap; /* Output capacity */
    int             depth;      /* Current nesting depth */
    SexpError       error;      /* Last error */
    const char     *error_pos;  /* Position of error */
    char            error_msg[256]; /* Error message */
} SexpParseState;

/*
 * Key index entry - position where a keyed list (key ...) appears
 */
typedef struct KeyIndexEntry {
    const uint8_t *pos;          /* Position of the list (key ...) */
    struct KeyIndexEntry *next;  /* Next entry with same key (linked list) */
} KeyIndexEntry;

/*
 * Key index - maps symbol indices to positions where they appear as list heads
 * This enables O(1) lookup for key-based containment queries.
 */
typedef struct {
    KeyIndexEntry **buckets;     /* Hash buckets: sym_index -> entry list */
    int             bucket_count;/* Number of buckets (= sym_count) */
    KeyIndexEntry  *entries;     /* Pre-allocated entry pool */
    int             entry_count; /* Number of entries used */
    int             entry_cap;   /* Capacity of entry pool */
} KeyIndex;

/* Read state for traversing binary sexp */
typedef struct {
    const uint8_t  *data;       /* Binary data */
    size_t          data_len;   /* Data length */
    const uint8_t  *symbols;    /* Start of symbol table data */
    int             sym_count;  /* Number of symbols */
    const uint8_t  *root;       /* Start of root element */
    SexpError       error;      /* Last error */
    
    /* Pre-parsed symbol table for O(1) access (lazy-allocated) */
    const char    **sym_strs;   /* Array of symbol string pointers */
    int            *sym_lens;   /* Array of symbol lengths */
    uint32_t       *sym_hashes; /* Pre-computed hashes for fast compare */
    size_t          header_len; /* Length of version + symbol table */
    int             sym_parsed; /* 0 = not parsed, 1 = parsed */
    
    /* Key index for O(1) key-based containment (lazy-built) */
    KeyIndex       *key_index;  /* NULL until first key lookup */
} SexpReadState;

/*
 * Varint encoding (protobuf-style, little-endian)
 * Returns number of bytes written/read, or 0 on error
 */
static inline int sexp_encode_varint(uint8_t *buf, size_t buf_len, uint64_t value) {
    int n = 0;
    do {
        if ((size_t)n >= buf_len) return 0;
        buf[n] = (value & 0x7F) | ((value > 0x7F) ? 0x80 : 0);
        value >>= 7;
        n++;
    } while (value > 0);
    return n;
}

static inline int sexp_decode_varint(const uint8_t *buf, size_t buf_len, uint64_t *value) {
    *value = 0;
    int shift = 0;
    int n = 0;
    do {
        if ((size_t)n >= buf_len || shift >= 64) return 0;
        *value |= (uint64_t)(buf[n] & 0x7F) << shift;
        shift += 7;
    } while (buf[n++] & 0x80);
    return n;
}

/*
 * Fast path varint decoder - inline for 1-2 byte values (covers 99% of cases)
 * Returns number of bytes consumed, or falls back to full decoder
 */
static inline int sexp_decode_varint_fast(const uint8_t *buf, size_t buf_len, uint64_t *value) {
    if (buf_len == 0) return 0;
    if (!(buf[0] & 0x80)) {
        *value = buf[0];
        return 1;
    }
    if (buf_len >= 2 && !(buf[1] & 0x80)) {
        *value = (buf[0] & 0x7F) | ((uint64_t)buf[1] << 7);
        return 2;
    }
    /* Fall back to full decoder for larger values */
    return sexp_decode_varint(buf, buf_len, value);
}

/* Zigzag encoding for signed integers */
static inline uint64_t sexp_zigzag_encode(int64_t value) {
    return (uint64_t)((value << 1) ^ (value >> 63));
}

static inline int64_t sexp_zigzag_decode(uint64_t value) {
    return (int64_t)((value >> 1) ^ -(int64_t)(value & 1));
}

/*
 * Parser API
 */

/* Initialize parse state */
void sexp_parse_init(SexpParseState *state, const char *input, size_t len);

/* Free parse state resources */
void sexp_parse_free(SexpParseState *state);

/* Parse input and return binary blob (caller must free with sqlite3_free) */
uint8_t *sexp_parse(SexpParseState *state, size_t *out_len);

/*
 * Reader API
 */

/* Initialize read state from binary data */
int sexp_read_init(SexpReadState *state, const uint8_t *data, size_t len);

/* Free read state resources */
void sexp_read_free(SexpReadState *state);

/* Ensure symbol table is parsed (lazy initialization) */
int sexp_ensure_symbols_parsed(SexpReadState *state);

/* Get type of element at position */
SexpType sexp_get_type(SexpReadState *state, const uint8_t *pos);

/* Get type name as string */
const char *sexp_type_name(SexpType type);

/* Convert to text (returns malloc'd string, caller must free) */
char *sexp_to_text(SexpReadState *state);

/*
 * Operations API
 */

/* Get first element of list (car) */
const uint8_t *sexp_car(SexpReadState *state, const uint8_t *list_pos);

/* Build new sexp containing rest of list (cdr) - returns malloc'd blob */
uint8_t *sexp_cdr(SexpReadState *state, const uint8_t *list_pos, size_t *out_len);

/* Get nth element of list (0-indexed) */
const uint8_t *sexp_nth(SexpReadState *state, const uint8_t *list_pos, int n);

/* Follow a fixed sequence of indices relative to start */
const uint8_t *sexp_path_follow(SexpReadState *state, const uint8_t *start,
                                const int *indices, int depth);

/* Get length of list */
int sexp_length(SexpReadState *state, const uint8_t *list_pos);

/* Check equality of two sexps */
int sexp_equal(const uint8_t *a, size_t a_len, const uint8_t *b, size_t b_len);

/* Check if needle is contained in haystack */
int sexp_contains(const uint8_t *haystack, size_t h_len,
                  const uint8_t *needle, size_t n_len);

/* Get integer value (returns 0 and sets error if not integer) */
int64_t sexp_get_integer(SexpReadState *state, const uint8_t *pos);

/* Get float value (returns 0.0 and sets error if not float) */
double sexp_get_float(SexpReadState *state, const uint8_t *pos);

/* Get symbol name (returns NULL if not symbol) */
const char *sexp_get_symbol(SexpReadState *state, const uint8_t *pos, int *len);

/* Get string value (returns NULL if not string) */
const char *sexp_get_string(SexpReadState *state, const uint8_t *pos, int *len);

/*
 * Fast element extraction - reuses parent's symbol table header
 * Returns malloc'd blob, caller must free
 */
uint8_t *sexp_extract_element(SexpReadState *state, const uint8_t *elem_start, 
                               const uint8_t *elem_end, size_t *out_len);

/*
 * Simple FNV-1a hash for symbol comparison
 */
static inline uint32_t sexp_hash_bytes(const char *data, int len) {
    uint32_t hash = 2166136261u;
    for (int i = 0; i < len; i++) {
        hash ^= (uint8_t)data[i];
        hash *= 16777619u;
    }
    return hash;
}

/*
 * Hash combination (from pg_sexp)
 */
static inline uint32_t sexp_hash_combine(uint32_t h1, uint32_t h2) {
    return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
}

static inline uint32_t sexp_hash_uint32(uint32_t val) {
    uint32_t h = 2166136261u;
    h = (h ^ (val & 0xFF)) * 16777619u;
    h = (h ^ ((val >> 8) & 0xFF)) * 16777619u;
    h = (h ^ ((val >> 16) & 0xFF)) * 16777619u;
    h = (h ^ ((val >> 24) & 0xFF)) * 16777619u;
    return h;
}

static inline uint32_t sexp_hash_string_with_tag(uint8_t tag, const char *str, int len) {
    uint32_t h = sexp_hash_bytes(str, len);
    return sexp_hash_combine(tag, h);
}

/*
 * BLOOM FILTER for Containment Fast Rejection
 * 
 * 64-bit signature computed from element hashes. If the needle's bloom
 * bits are not a subset of the container's bits, containment is impossible.
 * 
 * Uses 4 hash functions (BLOOM_K) to set bits.
 */
typedef uint64_t BloomSig;
#define BLOOM_K 4

/* Rotate left for hash function variation */
static inline uint32_t rotl32(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

/* Compute bloom signature for a single element hash */
static inline BloomSig bloom_compute_sig(uint32_t elem_hash) {
    BloomSig sig = 0;
    for (int i = 0; i < BLOOM_K; i++) {
        uint32_t rotated = rotl32(elem_hash, i * 8);
        int bit_pos = rotated & 63;  /* 0-63 for 64-bit bloom */
        sig |= ((BloomSig)1 << bit_pos);
    }
    return sig;
}

/* Combine bloom signatures (union of bits) */
static inline BloomSig bloom_combine(BloomSig parent, BloomSig child) {
    return parent | child;
}

/*
 * Fast rejection check:
 *   if (needle_bloom & ~container_bloom) != 0:
 *       needle has bits not present in container -> NOT contained
 */
static inline int bloom_may_contain(BloomSig container_bloom, BloomSig needle_bloom) {
    return (needle_bloom & ~container_bloom) == 0;
}

/*
 * Compute bloom signature of an element
 */
BloomSig sexp_element_bloom(SexpReadState *state, const uint8_t *pos);

/*
 * Build key index for fast key-based lookups (lazy, cached in state)
 * Returns 0 on success, -1 on error.
 */
int sexp_ensure_key_index(SexpReadState *state);

/*
 * Key-based containment (like JSONB @> operator)
 * Treats list heads as keys, matches remaining elements in any order.
 * 
 * Example: (user (name "alice") (age 30)) contains_key (user (age 30)) = true
 */
int sexp_contains_key(const uint8_t *haystack, size_t h_len,
                      const uint8_t *needle, size_t n_len);

/*
 * Key-based containment with pre-parsed states for both haystack and needle.
 * Use this for maximum performance when both are accessed multiple times.
 * The haystack will have its key index built on first call.
 */
int sexp_contains_key_indexed(SexpReadState *haystack_state,
                               SexpReadState *needle_state);

/*
 * Key-based containment with pre-parsed needle state.
 * Use this version when the needle is constant across many calls (e.g., in SQL queries).
 * The needle_state must remain valid for the duration of the call.
 */
int sexp_contains_key_with_state(const uint8_t *haystack, size_t h_len,
                                  SexpReadState *needle_state);

/*
 * Find value by key in alist-style s-expression.
 * Searches for a sublist whose head matches the given key (symbol or string),
 * and returns a pointer to the second element (the value).
 *
 * Example: sexp_get_by_key((user (name "alice") (age 30)), "name") -> pointer to "alice"
 *
 * Returns pointer to value element, or NULL if not found.
 * Also sets *value_end to the end of the value element.
 */
const uint8_t *sexp_get_by_key(SexpReadState *state, const uint8_t *list_pos,
                                const char *key, int key_len,
                                const uint8_t **value_end);

#endif /* SEXP_H */
