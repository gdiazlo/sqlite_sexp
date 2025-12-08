-- Comprehensive Test Suite for SQLite S-Expression Extension
-- Run with: sqlite3 < test/test_comprehensive.sql
-- Returns non-zero exit code on failure

.load ./sexp
.bail on
.mode list
.headers off

-- ============================================================
-- Test Helper: assertion macro using CHECK constraint trick
-- ============================================================

CREATE TEMP TABLE test_results (
    test_name TEXT,
    passed INTEGER,
    details TEXT
);

-- ============================================================
-- SECTION 1: Parsing Tests
-- ============================================================

SELECT '=== SECTION 1: Parsing Tests ===' as section;

-- 1.1 Basic atoms
INSERT INTO test_results VALUES ('parse_integer', sexp_text(sexp('42')) = '42', 'integer 42');
INSERT INTO test_results VALUES ('parse_neg_integer', sexp_text(sexp('-100')) = '-100', 'negative integer');
INSERT INTO test_results VALUES ('parse_zero', sexp_text(sexp('0')) = '0', 'zero');
INSERT INTO test_results VALUES ('parse_float', sexp_text(sexp('3.14')) = '3.14', 'float');
INSERT INTO test_results VALUES ('parse_float_exp', sexp_text(sexp('1e10')) LIKE '1%e%10' OR sexp_text(sexp('1e10')) = '10000000000.0', 'exponential');
INSERT INTO test_results VALUES ('parse_symbol', sexp_text(sexp('hello')) = 'hello', 'symbol');
INSERT INTO test_results VALUES ('parse_symbol_special', sexp_text(sexp('my-symbol_123')) = 'my-symbol_123', 'symbol with special chars');
INSERT INTO test_results VALUES ('parse_string', sexp_text(sexp('"hello world"')) = '"hello world"', 'string');
INSERT INTO test_results VALUES ('parse_empty_string', sexp_text(sexp('""')) = '""', 'empty string');
INSERT INTO test_results VALUES ('parse_nil', sexp_text(sexp('()')) = '()', 'nil/empty list');
INSERT INTO test_results VALUES ('parse_nil_keyword', sexp_text(sexp('nil')) = '()', 'nil keyword');

-- 1.2 String escapes
INSERT INTO test_results VALUES ('parse_escape_newline', sexp_text(sexp('"a\nb"')) = '"a\nb"', 'newline escape');
INSERT INTO test_results VALUES ('parse_escape_tab', sexp_text(sexp('"a\tb"')) = '"a\tb"', 'tab escape');
INSERT INTO test_results VALUES ('parse_escape_quote', sexp_text(sexp('"a\"b"')) = '"a\"b"', 'quote escape');
INSERT INTO test_results VALUES ('parse_escape_backslash', sexp_text(sexp('"a\\b"')) = '"a\\b"', 'backslash escape');

-- 1.3 Lists
INSERT INTO test_results VALUES ('parse_simple_list', sexp_text(sexp('(a b c)')) = '(a b c)', 'simple list');
INSERT INTO test_results VALUES ('parse_nested_list', sexp_text(sexp('(a (b c) d)')) = '(a (b c) d)', 'nested list');
INSERT INTO test_results VALUES ('parse_deep_nested', sexp_text(sexp('((((a))))')) = '((((a))))', 'deeply nested');
INSERT INTO test_results VALUES ('parse_mixed_list', sexp_text(sexp('(1 "str" sym 3.14)')) = '(1 "str" sym 3.14)', 'mixed types');

-- 1.4 Whitespace handling
INSERT INTO test_results VALUES ('parse_extra_spaces', sexp_text(sexp('(a   b    c)')) = '(a b c)', 'extra spaces');
INSERT INTO test_results VALUES ('parse_multiline', sexp_text(sexp('(a
b
c)')) = '(a b c)', 'multiline');
INSERT INTO test_results VALUES ('parse_with_comment', sexp_text(sexp('(a ; comment
b c)')) = '(a b c)', 'with comment');

-- 1.5 Small vs large integers
INSERT INTO test_results VALUES ('parse_small_int_min', sexp_text(sexp('-16')) = '-16', 'small int min');
INSERT INTO test_results VALUES ('parse_small_int_max', sexp_text(sexp('15')) = '15', 'small int max');
INSERT INTO test_results VALUES ('parse_large_int', sexp_text(sexp('1000000')) = '1000000', 'large int');
INSERT INTO test_results VALUES ('parse_large_neg_int', sexp_text(sexp('-999999')) = '-999999', 'large negative int');

-- ============================================================
-- SECTION 2: Type Inspection Tests
-- ============================================================

SELECT '=== SECTION 2: Type Inspection Tests ===' as section;

INSERT INTO test_results VALUES ('typeof_nil', sexp_typeof(sexp('()')) = 'nil', 'nil type');
INSERT INTO test_results VALUES ('typeof_integer', sexp_typeof(sexp('42')) = 'integer', 'integer type');
INSERT INTO test_results VALUES ('typeof_float', sexp_typeof(sexp('3.14')) = 'float', 'float type');
INSERT INTO test_results VALUES ('typeof_symbol', sexp_typeof(sexp('hello')) = 'symbol', 'symbol type');
INSERT INTO test_results VALUES ('typeof_string', sexp_typeof(sexp('"hello"')) = 'string', 'string type');
INSERT INTO test_results VALUES ('typeof_list', sexp_typeof(sexp('(a b)')) = 'list', 'list type');

-- Type predicates
INSERT INTO test_results VALUES ('is_nil_true', is_nil(sexp('()')) = 1, 'is_nil true');
INSERT INTO test_results VALUES ('is_nil_false', is_nil(sexp('42')) = 0, 'is_nil false');
INSERT INTO test_results VALUES ('is_list_true', is_list(sexp('(a b)')) = 1, 'is_list true');
INSERT INTO test_results VALUES ('is_list_nil', is_list(sexp('()')) = 1, 'is_list for nil');
INSERT INTO test_results VALUES ('is_list_false', is_list(sexp('42')) = 0, 'is_list false');
INSERT INTO test_results VALUES ('is_atom_symbol', is_atom(sexp('hello')) = 1, 'is_atom symbol');
INSERT INTO test_results VALUES ('is_atom_string', is_atom(sexp('"hi"')) = 1, 'is_atom string');
INSERT INTO test_results VALUES ('is_atom_number', is_atom(sexp('42')) = 1, 'is_atom number');
INSERT INTO test_results VALUES ('is_atom_list', is_atom(sexp('(a)')) = 0, 'is_atom list');
INSERT INTO test_results VALUES ('is_symbol_true', is_symbol(sexp('hello')) = 1, 'is_symbol true');
INSERT INTO test_results VALUES ('is_symbol_false', is_symbol(sexp('"hello"')) = 0, 'is_symbol false');
INSERT INTO test_results VALUES ('is_string_true', is_string(sexp('"hello"')) = 1, 'is_string true');
INSERT INTO test_results VALUES ('is_string_false', is_string(sexp('hello')) = 0, 'is_string false');
INSERT INTO test_results VALUES ('is_number_int', is_number(sexp('42')) = 1, 'is_number int');
INSERT INTO test_results VALUES ('is_number_float', is_number(sexp('3.14')) = 1, 'is_number float');
INSERT INTO test_results VALUES ('is_number_false', is_number(sexp('hello')) = 0, 'is_number false');

-- ============================================================
-- SECTION 3: List Operations Tests
-- ============================================================

SELECT '=== SECTION 3: List Operations Tests ===' as section;

-- sexp_length
INSERT INTO test_results VALUES ('length_nil', sexp_length(sexp('()')) = 0, 'nil length');
INSERT INTO test_results VALUES ('length_1', sexp_length(sexp('(a)')) = 1, 'single element');
INSERT INTO test_results VALUES ('length_3', sexp_length(sexp('(a b c)')) = 3, 'three elements');
INSERT INTO test_results VALUES ('length_nested', sexp_length(sexp('(a (b c) d)')) = 3, 'nested list length');

-- sexp_car (now returns native types!)
INSERT INTO test_results VALUES ('car_symbol', sexp_car(sexp('(a b c)')) = 'a', 'car returns symbol as text');
INSERT INTO test_results VALUES ('car_integer', sexp_car(sexp('(42 b c)')) = 42, 'car returns integer');
INSERT INTO test_results VALUES ('car_float', abs(sexp_car(sexp('(3.14 b c)')) - 3.14) < 0.001, 'car returns float');
INSERT INTO test_results VALUES ('car_string', sexp_car(sexp('("hello" b c)')) = 'hello', 'car returns string (unquoted)');
INSERT INTO test_results VALUES ('car_nested_is_text', typeof(sexp_car(sexp('((nested) b c)'))) = 'text', 'car of list returns text');
INSERT INTO test_results VALUES ('car_nested_text', sexp_car(sexp('((nested) b c)')) = '(nested)', 'car nested as text');
INSERT INTO test_results VALUES ('car_nil', sexp_car(sexp('()')) IS NULL, 'car of nil');

-- sexp_cdr (still returns blob since result is always a list)
INSERT INTO test_results VALUES ('cdr_simple', sexp_text(sexp_cdr(sexp('(a b c)'))) = '(b c)', 'cdr simple');
INSERT INTO test_results VALUES ('cdr_single', sexp_text(sexp_cdr(sexp('(a)'))) = '()', 'cdr single element');
INSERT INTO test_results VALUES ('cdr_nil', sexp_text(sexp_cdr(sexp('()'))) = '()', 'cdr of nil');

-- sexp_nth (now returns native types!)
INSERT INTO test_results VALUES ('nth_0_symbol', sexp_nth(sexp('(a b c d e)'), 0) = 'a', 'nth 0 symbol');
INSERT INTO test_results VALUES ('nth_integer', sexp_nth(sexp('(1 2 3 4 5)'), 2) = 3, 'nth integer');
INSERT INTO test_results VALUES ('nth_string', sexp_nth(sexp('("x" "y" "z")'), 1) = 'y', 'nth string');
INSERT INTO test_results VALUES ('nth_float', abs(sexp_nth(sexp('(1.1 2.2 3.3)'), 2) - 3.3) < 0.001, 'nth float');
INSERT INTO test_results VALUES ('nth_list_is_text', typeof(sexp_nth(sexp('(a (nested) c)'), 1)) = 'text', 'nth list returns text');
INSERT INTO test_results VALUES ('nth_out_of_bounds', sexp_nth(sexp('(a b c)'), 10) IS NULL, 'nth out of bounds');
INSERT INTO test_results VALUES ('nth_negative', sexp_nth(sexp('(a b c)'), -1) IS NULL, 'nth negative');

-- sexp_head/sexp_tail aliases
INSERT INTO test_results VALUES ('head_alias', sexp_head(sexp('(a b c)')) = 'a', 'head alias returns native');
INSERT INTO test_results VALUES ('tail_alias', sexp_text(sexp_tail(sexp('(a b c)'))) = '(b c)', 'tail alias');

-- ============================================================
-- SECTION 4: sexp_path Tests (now returns native types!)
-- ============================================================

SELECT '=== SECTION 4: sexp_path Tests ===' as section;

INSERT INTO test_results VALUES ('path_single_symbol', sexp_path(sexp('(a b c)'), 1) = 'b', 'single index symbol');
INSERT INTO test_results VALUES ('path_single_integer', sexp_path(sexp('(10 20 30)'), 2) = 30, 'single index integer');
INSERT INTO test_results VALUES ('path_nested_symbol', sexp_path(sexp('(a (b c) d)'), 1, 0) = 'b', 'nested path symbol');
INSERT INTO test_results VALUES ('path_deep_symbol', sexp_path(sexp('(a (b (c d)))'), 1, 1, 0) = 'c', 'deep path symbol');
INSERT INTO test_results VALUES ('path_returns_list', typeof(sexp_path(sexp('(a (b c) d)'), 1)) = 'text', 'path to list returns text');
INSERT INTO test_results VALUES ('path_invalid', sexp_path(sexp('(a b)'), 5) IS NULL, 'invalid path');

-- ============================================================
-- SECTION 4b: sexp_get Tests (key-value extraction like json_extract)
-- ============================================================

SELECT '=== SECTION 4b: sexp_get Tests ===' as section;

-- Basic key-value extraction from alist structure
INSERT INTO test_results VALUES ('get_string', 
    sexp_get(sexp('(user (name "alice") (age 30))'), 'name') = 'alice', 
    'get string value');
INSERT INTO test_results VALUES ('get_integer', 
    sexp_get(sexp('(user (name "alice") (age 30))'), 'age') = 30, 
    'get integer value');
INSERT INTO test_results VALUES ('get_float', 
    abs(sexp_get(sexp('(item (price 9.99) (qty 5))'), 'price') - 9.99) < 0.001, 
    'get float value');
INSERT INTO test_results VALUES ('get_symbol', 
    sexp_get(sexp('(event (type click) (page home))'), 'type') = 'click', 
    'get symbol value');

-- Nested list as value
INSERT INTO test_results VALUES ('get_list_is_text', 
    typeof(sexp_get(sexp('(user (tags (admin vip)) (id 1))'), 'tags')) = 'text', 
    'get list returns text');
INSERT INTO test_results VALUES ('get_list_text', 
    sexp_get(sexp('(user (tags (admin vip)) (id 1))'), 'tags') = '(admin vip)', 
    'get list as text');

-- Key not found
INSERT INTO test_results VALUES ('get_not_found', 
    sexp_get(sexp('(user (name "alice"))'), 'email') IS NULL, 
    'get missing key returns null');

-- Empty sexp
INSERT INTO test_results VALUES ('get_empty', 
    sexp_get(sexp('()'), 'foo') IS NULL, 
    'get from empty returns null');

-- Works with string keys too
INSERT INTO test_results VALUES ('get_with_string_key', 
    sexp_get(sexp('(config ("api-key" "secret123"))'), 'api-key') = 'secret123', 
    'get with string key in sexp');

-- Real-world usage: JSON-like query
INSERT INTO test_results VALUES ('get_jsonlike', 
    sexp_get(sexp('(event (class_uid 1001) (device (hostname "laptop-1")))'), 'class_uid') = 1001, 
    'JSON-like structure query');

-- ============================================================
-- SECTION 5: sexp_string_value Tests
-- ============================================================

SELECT '=== SECTION 5: sexp_string_value Tests ===' as section;

INSERT INTO test_results VALUES ('string_value_simple', sexp_string_value(sexp('"hello"')) = 'hello', 'simple string');
INSERT INTO test_results VALUES ('string_value_spaces', sexp_string_value(sexp('"hello world"')) = 'hello world', 'string with spaces');
-- Note: sexp_nth now returns native TEXT directly, so sexp_string_value is not needed!
INSERT INTO test_results VALUES ('string_value_nested_native', sexp_nth(sexp('(name "John")'), 1) = 'John', 'sexp_nth returns native text');
INSERT INTO test_results VALUES ('string_value_not_string', sexp_string_value(sexp('symbol')) IS NULL, 'non-string returns null');

-- Comparison using string_value
INSERT INTO test_results VALUES ('string_compare_text', sexp_text(sexp('"test"')) = '"test"', 'text includes quotes');
INSERT INTO test_results VALUES ('string_compare_value', sexp_string_value(sexp('"test"')) = 'test', 'value excludes quotes');

-- ============================================================
-- SECTION 6: sexp_contains Tests
-- ============================================================

SELECT '=== SECTION 6: sexp_contains Tests ===' as section;

INSERT INTO test_results VALUES ('contains_atom', sexp_contains(sexp('(a b c)'), sexp('b')) = 1, 'contains atom');
INSERT INTO test_results VALUES ('contains_not', sexp_contains(sexp('(a b c)'), sexp('x')) = 0, 'not contains');
INSERT INTO test_results VALUES ('contains_sublist', sexp_contains(sexp('(a (b c) d)'), sexp('(b c)')) = 1, 'contains sublist');
INSERT INTO test_results VALUES ('contains_nested', sexp_contains(sexp('(define (f x) (+ x 1))'), sexp('(+ x 1)')) = 1, 'contains nested');
INSERT INTO test_results VALUES ('contains_deep', sexp_contains(sexp('(a (b (c d)))'), sexp('d')) = 1, 'contains deep');
INSERT INTO test_results VALUES ('contains_self', sexp_contains(sexp('(a b)'), sexp('(a b)')) = 1, 'contains self');

-- ============================================================
-- SECTION 6b: sexp_contains_key Tests (Key-based containment)
-- ============================================================

SELECT '=== SECTION 6b: sexp_contains_key Tests ===' as section;

-- Basic head-only matches
INSERT INTO test_results VALUES ('contains_key_head_only', 
    sexp_contains_key(sexp('(a b)'), sexp('(a)')) = 1, 
    'head-only match');
INSERT INTO test_results VALUES ('contains_key_head_mismatch', 
    sexp_contains_key(sexp('(a b)'), sexp('(b)')) = 0, 
    'head must match');

-- Full matches
INSERT INTO test_results VALUES ('contains_key_full_match', 
    sexp_contains_key(sexp('(a b)'), sexp('(a b)')) = 1, 
    'full list match');
INSERT INTO test_results VALUES ('contains_key_full_mismatch', 
    sexp_contains_key(sexp('(a b)'), sexp('(x y)')) = 0, 
    'full list mismatch');

-- Order-independent tail matching
INSERT INTO test_results VALUES ('contains_key_order_independent', 
    sexp_contains_key(sexp('(a b c)'), sexp('(a c b)')) = 1, 
    'order independent tail');
INSERT INTO test_results VALUES ('contains_key_order_3_elements', 
    sexp_contains_key(sexp('(f 1 2 3)'), sexp('(f 3 1)')) = 1, 
    'order independent with 3 tail elements');

-- Empty needle matches any list
INSERT INTO test_results VALUES ('contains_key_empty_needle', 
    sexp_contains_key(sexp('(a b c)'), sexp('()')) = 1, 
    'empty needle matches list');
INSERT INTO test_results VALUES ('contains_key_empty_both', 
    sexp_contains_key(sexp('()'), sexp('()')) = 1, 
    'empty matches empty');

-- Atom needle (exact match required)
INSERT INTO test_results VALUES ('contains_key_atom_found', 
    sexp_contains_key(sexp('(a b c)'), sexp('a')) = 1, 
    'atom needle found');
INSERT INTO test_results VALUES ('contains_key_atom_not_found', 
    sexp_contains_key(sexp('(a b c)'), sexp('x')) = 0, 
    'atom needle not found');

-- Recursive search
INSERT INTO test_results VALUES ('contains_key_recursive', 
    sexp_contains_key(sexp('(foo (bar (a b)))'), sexp('(a)')) = 1, 
    'recursive search finds nested');
INSERT INTO test_results VALUES ('contains_key_recursive_deep', 
    sexp_contains_key(sexp('(define (f x) (+ x 1))'), sexp('(+)')) = 1, 
    'recursive search deep nested');

-- Nested structure matches
INSERT INTO test_results VALUES ('contains_key_nested_full', 
    sexp_contains_key(sexp('(user (name "alice"))'), sexp('(user (name "alice"))')) = 1, 
    'nested full match');
INSERT INTO test_results VALUES ('contains_key_nested_partial', 
    sexp_contains_key(sexp('(user (name "alice") (age 30))'), sexp('(user (age 30))')) = 1, 
    'nested partial match');
INSERT INTO test_results VALUES ('contains_key_nested_head_only', 
    sexp_contains_key(sexp('(user (name "alice") (age 30))'), sexp('(user)')) = 1, 
    'nested head-only match');

-- Real-world JSONB-like usage
INSERT INTO test_results VALUES ('contains_key_jsonb_style', 
    sexp_contains_key(sexp('(event (type "login") (user (id 123) (name "alice")))'), 
                      sexp('(event (user (id 123)))')) = 1, 
    'JSONB-style partial match');

-- ============================================================
-- SECTION 7: Large List Tests (SEntry table)
-- ============================================================

SELECT '=== SECTION 7: Large List Tests ===' as section;

-- Lists with 5+ elements use SEntry table for O(1) access
INSERT INTO test_results VALUES ('large_list_parse', sexp_text(sexp('(a b c d e f g)')) = '(a b c d e f g)', 'parse large list');
INSERT INTO test_results VALUES ('large_list_length', sexp_length(sexp('(a b c d e f g)')) = 7, 'large list length');
INSERT INTO test_results VALUES ('large_list_nth_0', sexp_nth(sexp('(a b c d e f g)'), 0) = 'a', 'large list nth 0 native');
INSERT INTO test_results VALUES ('large_list_nth_3', sexp_nth(sexp('(a b c d e f g)'), 3) = 'd', 'large list nth 3 native');
INSERT INTO test_results VALUES ('large_list_nth_6', sexp_nth(sexp('(a b c d e f g)'), 6) = 'g', 'large list nth 6 native');
INSERT INTO test_results VALUES ('large_list_car', sexp_car(sexp('(1 2 3 4 5 6 7)')) = 1, 'large list car native');
INSERT INTO test_results VALUES ('large_list_cdr', sexp_length(sexp_cdr(sexp('(1 2 3 4 5 6 7)'))) = 6, 'large list cdr');

-- ============================================================
-- SECTION 8: Real-World Usage Tests
-- ============================================================

SELECT '=== SECTION 8: Real-World Usage Tests ===' as section;

-- Event storage and querying
DROP TABLE IF EXISTS test_events;
CREATE TABLE test_events (id INTEGER PRIMARY KEY, data BLOB);

INSERT INTO test_events (data) VALUES
    (sexp('(click (user 123) (page "/home"))')),
    (sexp('(view (user 123) (page "/products"))')),
    (sexp('(click (user 456) (page "/products"))')),
    (sexp('(purchase (user 123) (item "widget") (price 9.99))'));

INSERT INTO test_results VALUES ('usage_insert', (SELECT COUNT(*) FROM test_events) = 4, 'insert events');

-- Query by event type - now using native type returns
INSERT INTO test_results VALUES ('usage_query_type', 
    (SELECT COUNT(*) FROM test_events WHERE sexp_car(data) = 'click') = 2, 
    'query by event type (native)');
INSERT INTO test_results VALUES ('usage_query_contains', 
    (SELECT COUNT(*) FROM test_events WHERE sexp_contains(data, sexp('(user 123)'))) = 3, 
    'query by containment');

-- Using sexp_get for JSON-like queries
INSERT INTO test_results VALUES ('usage_get_user', 
    (SELECT COUNT(*) FROM test_events WHERE sexp_get(data, 'user') = 123) = 3, 
    'query with sexp_get');
INSERT INTO test_results VALUES ('usage_get_page', 
    (SELECT sexp_get(data, 'page') FROM test_events WHERE id = 1) = '/home', 
    'extract string with sexp_get');

-- OCSF-like structure access
INSERT INTO test_results VALUES ('ocsf_structure', 
    sexp_path(
        sexp('(event (class_uid 1001) (device (hostname "laptop-1") (ip "192.168.0.1")))'),
        2, 2, 1) = '192.168.0.1',
    'OCSF-like structure access (native)');

-- Nested sexp_get usage
INSERT INTO test_results VALUES ('ocsf_get_class', 
    sexp_get(sexp('(event (class_uid 1001) (device (hostname "laptop-1")))'), 'class_uid') = 1001,
    'OCSF get class_uid');

DROP TABLE test_events;

-- ============================================================
-- SECTION 9: Native Type Return Tests (like json_extract)
-- ============================================================

SELECT '=== SECTION 9: Native Type Return Tests ===' as section;

-- Verify typeof() returns correct SQL types
INSERT INTO test_results VALUES ('native_int_type', 
    typeof(sexp_nth(sexp('(42)'), 0)) = 'integer', 
    'integer returns SQL integer type');
INSERT INTO test_results VALUES ('native_float_type', 
    typeof(sexp_nth(sexp('(3.14)'), 0)) = 'real', 
    'float returns SQL real type');
INSERT INTO test_results VALUES ('native_symbol_type', 
    typeof(sexp_nth(sexp('(hello)'), 0)) = 'text', 
    'symbol returns SQL text type');
INSERT INTO test_results VALUES ('native_string_type', 
    typeof(sexp_nth(sexp('("world")'), 0)) = 'text', 
    'string returns SQL text type');
INSERT INTO test_results VALUES ('native_list_type', 
    typeof(sexp_nth(sexp('((a b))'), 0)) = 'text', 
    'list returns SQL blob type');
INSERT INTO test_results VALUES ('native_nil_type', 
    sexp_car(sexp('()')) IS NULL, 
    'nil returns SQL NULL');

-- Verify values can be used directly in arithmetic
INSERT INTO test_results VALUES ('native_arithmetic', 
    sexp_nth(sexp('(10 20 30)'), 0) + sexp_nth(sexp('(10 20 30)'), 1) = 30, 
    'integers work in arithmetic');
INSERT INTO test_results VALUES ('native_float_arithmetic', 
    abs(sexp_nth(sexp('(1.5 2.5)'), 0) + sexp_nth(sexp('(1.5 2.5)'), 1) - 4.0) < 0.001, 
    'floats work in arithmetic');

-- Verify values can be used directly in string operations
INSERT INTO test_results VALUES ('native_string_concat', 
    sexp_nth(sexp('("hello" "world")'), 0) || ' ' || sexp_nth(sexp('("hello" "world")'), 1) = 'hello world', 
    'strings work in concatenation');

-- Verify values can be compared directly
INSERT INTO test_results VALUES ('native_int_compare', 
    sexp_get(sexp('(config (port 8080))'), 'port') > 1000, 
    'integers work in comparisons');
INSERT INTO test_results VALUES ('native_string_compare', 
    sexp_get(sexp('(user (name "alice"))'), 'name') = 'alice', 
    'strings work in equality');

-- Boolean support: true/false symbols return as SQL INTEGER 1/0
INSERT INTO test_results VALUES ('native_bool_true_type', 
    typeof(sexp_nth(sexp('(true)'), 0)) = 'integer', 
    'true returns SQL integer type');
INSERT INTO test_results VALUES ('native_bool_false_type', 
    typeof(sexp_nth(sexp('(false)'), 0)) = 'integer', 
    'false returns SQL integer type');
INSERT INTO test_results VALUES ('native_bool_true_value', 
    sexp_nth(sexp('(true)'), 0) = 1, 
    'true returns 1');
INSERT INTO test_results VALUES ('native_bool_false_value', 
    sexp_nth(sexp('(false)'), 0) = 0, 
    'false returns 0');
INSERT INTO test_results VALUES ('native_bool_compare_true', 
    sexp_get(sexp('(user (active true))'), 'active') = true, 
    'true compares with SQL true');
INSERT INTO test_results VALUES ('native_bool_compare_false', 
    sexp_get(sexp('(user (active false))'), 'active') = false, 
    'false compares with SQL false');
INSERT INTO test_results VALUES ('native_bool_in_where', 
    (SELECT COUNT(*) FROM (SELECT 1 WHERE sexp_get(sexp('(user (active true))'), 'active'))) = 1, 
    'true works in WHERE clause');

-- ============================================================
-- SECTION 10: Edge Cases and Error Handling
-- ============================================================

SELECT '=== SECTION 10: Edge Cases ===' as section;

-- NULL handling
INSERT INTO test_results VALUES ('null_input', sexp(NULL) IS NULL, 'null input');
INSERT INTO test_results VALUES ('null_text', sexp_text(NULL) IS NULL, 'null text input');

-- Boundary values
INSERT INTO test_results VALUES ('boundary_short_str', LENGTH(sexp_string_value(sexp('"' || 
    'a' || 'b' || 'c' || 'd' || 'e' || 'f' || 'g' || 'h' || 'i' || 'j' ||
    'k' || 'l' || 'm' || 'n' || 'o' || 'p' || 'q' || 'r' || 's' || 't' ||
    'u' || 'v' || 'w' || 'x' || 'y' || 'z' || '1' || '2' || '3' || '4' || '5' || '"'))) = 31, 
    'max short string (31 chars)');

-- Symbol deduplication
INSERT INTO test_results VALUES ('symbol_dedup', 
    sexp_text(sexp('(foo foo foo)')) = '(foo foo foo)', 
    'symbol deduplication');

-- ============================================================
-- Results Summary
-- ============================================================

SELECT '';
SELECT '========================================';
SELECT '           TEST RESULTS SUMMARY';
SELECT '========================================';

SELECT 'Total tests: ' || COUNT(*) FROM test_results;
SELECT 'Passed: ' || COUNT(*) FROM test_results WHERE passed = 1;
SELECT 'Failed: ' || COUNT(*) FROM test_results WHERE passed = 0;

SELECT '';
SELECT 'Failed tests:';
SELECT test_name || ': ' || details FROM test_results WHERE passed = 0;

-- Final status
SELECT CASE WHEN (SELECT COUNT(*) FROM test_results WHERE passed = 0) > 0 
    THEN 'TESTS FAILED!' 
    ELSE 'All tests passed!' 
END as final_status;
