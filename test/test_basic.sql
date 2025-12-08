-- SQLite S-Expression Extension Tests
-- Run with: sqlite3 < test/test_basic.sql

.load ./sexp

-- Enable headers and column mode for readable output
.mode column
.headers on

-- ============================================================
-- Basic Parsing and Text Conversion
-- ============================================================

SELECT '=== Basic Parsing ===' as test_section;

-- Parse and convert back to text
SELECT sexp_text(sexp('42')) as int_val;
SELECT sexp_text(sexp('-100')) as neg_int;
SELECT sexp_text(sexp('3.14')) as float_val;
SELECT sexp_text(sexp('hello')) as symbol_val;
SELECT sexp_text(sexp('"hello world"')) as string_val;
SELECT sexp_text(sexp('()')) as nil_val;
SELECT sexp_text(sexp('(a b c)')) as simple_list;
SELECT sexp_text(sexp('(+ 1 2)')) as expr;
SELECT sexp_text(sexp('(define (square x) (* x x))')) as nested;

-- ============================================================
-- Type Inspection
-- ============================================================

SELECT '=== Type Inspection ===' as test_section;

SELECT sexp_typeof(sexp('42')) as type_int;
SELECT sexp_typeof(sexp('3.14')) as type_float;
SELECT sexp_typeof(sexp('hello')) as type_symbol;
SELECT sexp_typeof(sexp('"hello"')) as type_string;
SELECT sexp_typeof(sexp('()')) as type_nil;
SELECT sexp_typeof(sexp('(a b)')) as type_list;

-- ============================================================
-- Type Predicates
-- ============================================================

SELECT '=== Type Predicates ===' as test_section;

SELECT is_nil(sexp('()')) as is_nil_true;
SELECT is_nil(sexp('42')) as is_nil_false;
SELECT is_list(sexp('(a b c)')) as is_list_true;
SELECT is_list(sexp('()')) as is_list_nil;  -- nil is also a list
SELECT is_list(sexp('42')) as is_list_false;
SELECT is_atom(sexp('hello')) as is_atom_symbol;
SELECT is_atom(sexp('"str"')) as is_atom_string;
SELECT is_atom(sexp('42')) as is_atom_int;
SELECT is_atom(sexp('(a)')) as is_atom_false;
SELECT is_symbol(sexp('hello')) as is_symbol_true;
SELECT is_symbol(sexp('"hello"')) as is_symbol_false;
SELECT is_string(sexp('"hello"')) as is_string_true;
SELECT is_string(sexp('hello')) as is_string_false;
SELECT is_number(sexp('42')) as is_number_int;
SELECT is_number(sexp('3.14')) as is_number_float;
SELECT is_number(sexp('hello')) as is_number_false;

-- ============================================================
-- List Operations
-- ============================================================

SELECT '=== List Operations ===' as test_section;

-- Length
SELECT sexp_length(sexp('()')) as len_nil;
SELECT sexp_length(sexp('(a)')) as len_1;
SELECT sexp_length(sexp('(a b c)')) as len_3;
SELECT sexp_length(sexp('(1 2 3 4 5 6)')) as len_6;

-- Car (first element) - NOW RETURNS NATIVE TYPES!
SELECT sexp_car(sexp('(a b c)')) as car_abc;  -- returns 'a' as TEXT
SELECT sexp_car(sexp('((nested) b c)')) as car_nested;  -- list now returns text representation

-- Cdr (rest of list) - still returns blob
SELECT sexp_text(sexp_cdr(sexp('(a b c)'))) as cdr_abc;
SELECT sexp_text(sexp_cdr(sexp('(a)'))) as cdr_single;
SELECT sexp_text(sexp_cdr(sexp('()'))) as cdr_nil;

-- Nth element - NOW RETURNS NATIVE TYPES!
SELECT sexp_nth(sexp('(a b c d e)'), 0) as nth_0;  -- returns 'a' as TEXT
SELECT sexp_nth(sexp('(a b c d e)'), 2) as nth_2;  -- returns 'c' as TEXT
SELECT sexp_nth(sexp('(a b c d e)'), 4) as nth_4;  -- returns 'e' as TEXT

-- ============================================================
-- Containment
-- ============================================================

SELECT '=== Containment ===' as test_section;

-- Simple containment
SELECT sexp_contains(sexp('(a b c)'), sexp('b')) as contains_atom;
SELECT sexp_contains(sexp('(a b c)'), sexp('x')) as not_contains;

-- Nested containment
SELECT sexp_contains(sexp('(a (b c) d)'), sexp('(b c)')) as contains_sublist;
SELECT sexp_contains(sexp('(define (f x) (+ x 1))'), sexp('(+ x 1)')) as contains_nested;

-- ============================================================
-- Comments and Whitespace
-- ============================================================

SELECT '=== Comments and Whitespace ===' as test_section;

SELECT sexp_text(sexp('(a  b   c)')) as whitespace;
SELECT sexp_text(sexp('
    (a 
     b 
     c)
')) as multiline;

-- Comments
SELECT sexp_text(sexp('(a ; this is a comment
b c)')) as with_comment;

-- ============================================================
-- Edge Cases
-- ============================================================

SELECT '=== Edge Cases ===' as test_section;

-- Small integers (-16 to 15)
SELECT sexp_text(sexp('-16')) as small_neg;
SELECT sexp_text(sexp('15')) as small_pos;

-- Large integers
SELECT sexp_text(sexp('1000000')) as large_int;
SELECT sexp_text(sexp('-999999')) as large_neg;

-- Strings with escapes
SELECT sexp_text(sexp('"hello\nworld"')) as newline_str;
SELECT sexp_text(sexp('"tab\there"')) as tab_str;
SELECT sexp_text(sexp('"quote\"here"')) as quote_str;

-- Deeply nested
SELECT sexp_text(sexp('((((a))))')) as deep_nested;

-- Empty string
SELECT sexp_text(sexp('""')) as empty_string;

-- ============================================================
-- Table Usage Example
-- ============================================================

SELECT '=== Table Usage ===' as test_section;

-- Create a table with sexp data
CREATE TABLE IF NOT EXISTS events (
    id INTEGER PRIMARY KEY,
    data BLOB
);

DELETE FROM events;

INSERT INTO events (data) VALUES
    (sexp('(click (user 123) (page "/home"))')),
    (sexp('(view (user 123) (page "/products"))')),
    (sexp('(click (user 456) (page "/products"))')),
    (sexp('(purchase (user 123) (item "widget") (price 9.99))'));

-- Query events
SELECT id, sexp_text(data) as event FROM events;

-- Get event types - NOW sexp_car RETURNS NATIVE TEXT!
SELECT id, sexp_car(data) as event_type FROM events;

-- Find click events - DIRECT COMPARISON NOW WORKS!
SELECT id, sexp_text(data) as event 
FROM events 
WHERE sexp_car(data) = 'click';

-- Find events for user 123
SELECT id, sexp_text(data) as event
FROM events
WHERE sexp_contains(data, sexp('(user 123)'));

-- Using sexp_get for key-based access
SELECT id, sexp_get(data, 'user') as user_id FROM events;

-- Cleanup
DROP TABLE events;

SELECT '=== All Tests Complete ===' as result;
