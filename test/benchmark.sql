-- Benchmark: JSONB vs SEXP in SQLite
-- Run with: sqlite3 < test/benchmark.sql
--
-- This benchmark compares performance of SQLite's built-in JSONB
-- with our SEXP extension for various operations.

.load ./sexp

-- Disable output during data generation
.mode list
.headers off

-- ============================================================
-- Setup: Create test tables
-- ============================================================

SELECT '=== SQLite JSONB vs SEXP Benchmark ===' as info;
SELECT '';

-- Drop existing tables
DROP TABLE IF EXISTS json_data;
DROP TABLE IF EXISTS sexp_data;
DROP TABLE IF EXISTS json_nested;
DROP TABLE IF EXISTS sexp_nested;
DROP TABLE IF EXISTS bench_results;

-- Results table
CREATE TABLE bench_results (
    test_name TEXT,
    data_type TEXT,
    rows_affected INTEGER,
    time_ms REAL
);

-- ============================================================
-- Test 1: Insert Performance (Simple Records)
-- ============================================================

SELECT '--- Test 1: Insert Performance (10000 simple records) ---';

-- Create tables
CREATE TABLE json_data (
    id INTEGER PRIMARY KEY,
    data JSONB
);

CREATE TABLE sexp_data (
    id INTEGER PRIMARY KEY,
    data BLOB
);

-- Time JSON inserts
SELECT datetime('now') as start_time;

.timer on

INSERT INTO json_data (data)
WITH RECURSIVE cnt(x) AS (
    SELECT 1
    UNION ALL
    SELECT x+1 FROM cnt WHERE x < 10000
)
SELECT jsonb_object(
    'id', x,
    'name', 'user_' || x,
    'email', 'user' || x || '@example.com',
    'age', 20 + (x % 50),
    'active', CASE WHEN x % 2 = 0 THEN json('true') ELSE json('false') END
) FROM cnt;

.timer off

SELECT '  JSONB insert: see timer above';

-- Time SEXP inserts
.timer on

INSERT INTO sexp_data (data)
WITH RECURSIVE cnt(x) AS (
    SELECT 1
    UNION ALL
    SELECT x+1 FROM cnt WHERE x < 10000
)
SELECT sexp(
    '(user (id ' || x || ') ' ||
    '(name "user_' || x || '") ' ||
    '(email "user' || x || '@example.com") ' ||
    '(age ' || (20 + (x % 50)) || ') ' ||
    '(active ' || CASE WHEN x % 2 = 0 THEN 'true' ELSE 'false' END || '))'
) FROM cnt;

.timer off

SELECT '  SEXP insert: see timer above';
SELECT '';

-- ============================================================
-- Test 2: Storage Size Comparison
-- ============================================================

SELECT '--- Test 2: Storage Size Comparison ---';

SELECT '  JSONB total size: ' || SUM(LENGTH(data)) || ' bytes' FROM json_data;
SELECT '  SEXP total size:  ' || SUM(LENGTH(data)) || ' bytes' FROM sexp_data;
SELECT '  JSONB avg size:   ' || ROUND(AVG(LENGTH(data)), 2) || ' bytes' FROM json_data;
SELECT '  SEXP avg size:    ' || ROUND(AVG(LENGTH(data)), 2) || ' bytes' FROM sexp_data;
SELECT '';

-- ============================================================
-- Test 3: Field Extraction Performance
-- ============================================================

SELECT '--- Test 3: Field Extraction (10000 rows) ---';

-- JSON field extraction
.timer on
SELECT COUNT(*) FROM json_data WHERE json_extract(data, '$.age') > 40;
.timer off
SELECT '  JSONB json_extract: see timer above';

-- SEXP field extraction (using sexp_get for key-based access)
.timer on
SELECT COUNT(*) FROM sexp_data WHERE sexp_get(data, 'age') > 40;
.timer off
SELECT '  SEXP sexp_get: see timer above';
SELECT '';

-- ============================================================
-- Test 4: Full Scan with Type Check
-- ============================================================

SELECT '--- Test 4: Full Table Scan with Filtering ---';

-- JSON: find all active users
.timer on
SELECT COUNT(*) FROM json_data WHERE json_extract(data, '$.active') = 1;
.timer off
SELECT '  JSONB filter active: see timer above';

-- SEXP: find all active users (using sexp_get for key-based access)
-- Note: true/false return as SQL INTEGER 1/0, just like SQLite's JSON
.timer on
SELECT COUNT(*) FROM sexp_data WHERE sexp_get(data, 'active') = true;
.timer off
SELECT '  SEXP sexp_get filter: see timer above';
SELECT '';

-- ============================================================
-- Test 5: Containment Check Performance
-- ============================================================

SELECT '--- Test 5: Containment Check (10000 rows) ---';

-- Create tables with nested data for containment tests
CREATE TABLE json_nested (
    id INTEGER PRIMARY KEY,
    data JSONB
);

CREATE TABLE sexp_nested (
    id INTEGER PRIMARY KEY,
    data BLOB
);

-- Insert nested data
INSERT INTO json_nested (data)
WITH RECURSIVE cnt(x) AS (
    SELECT 1
    UNION ALL
    SELECT x+1 FROM cnt WHERE x < 10000
)
SELECT jsonb_object(
    'event', CASE x % 4 
        WHEN 0 THEN 'click'
        WHEN 1 THEN 'view'
        WHEN 2 THEN 'purchase'
        ELSE 'scroll'
    END,
    'user', jsonb_object('id', x % 100, 'type', CASE WHEN x % 3 = 0 THEN 'premium' ELSE 'free' END),
    'metadata', jsonb_object('page', '/page/' || (x % 20), 'timestamp', 1700000000 + x)
) FROM cnt;

INSERT INTO sexp_nested (data)
WITH RECURSIVE cnt(x) AS (
    SELECT 1
    UNION ALL
    SELECT x+1 FROM cnt WHERE x < 10000
)
SELECT sexp(
    '(' || CASE x % 4 
        WHEN 0 THEN 'click'
        WHEN 1 THEN 'view'
        WHEN 2 THEN 'purchase'
        ELSE 'scroll'
    END || 
    ' (user (id ' || (x % 100) || ') (type ' || CASE WHEN x % 3 = 0 THEN 'premium' ELSE 'free' END || '))' ||
    ' (metadata (page "/page/' || (x % 20) || '") (timestamp ' || (1700000000 + x) || ')))'
) FROM cnt;

SELECT '  Nested data created';

-- Test 5a: JSON path query vs SEXP structural containment
SELECT '';
SELECT '  Test 5a: Path query vs structural containment';

.timer on
SELECT COUNT(*) FROM json_nested 
WHERE json_extract(data, '$.user.type') = 'premium';
.timer off
SELECT '  JSONB path query: see timer above';

-- SEXP structural containment (searches for exact subtree anywhere)
.timer on
SELECT COUNT(*) FROM sexp_nested 
WHERE sexp_contains(data, sexp('(type premium)'));
.timer off
SELECT '  SEXP sexp_contains (structural): see timer above';

-- Test 5b: SEXP key-based containment (like JSONB @> operator)
-- This is the proper equivalent to JSONB containment
SELECT '';
SELECT '  Test 5b: Key-based containment (like JSONB @>)';

-- SEXP key-based containment: find records where user object has type=premium
-- sexp_contains_key matches by keys, ignoring extra keys in haystack
.timer on
SELECT COUNT(*) FROM sexp_nested 
WHERE sexp_contains_key(data, sexp('(user (type premium))'));
.timer off
SELECT '  SEXP sexp_contains_key (key-based): see timer above';

-- Test 5c: Multiple key match
SELECT '';
SELECT '  Test 5c: Multiple key containment';

-- Find click events from premium users
.timer on
SELECT COUNT(*) FROM json_nested 
WHERE json_extract(data, '$.event') = 'click' 
  AND json_extract(data, '$.user.type') = 'premium';
.timer off
SELECT '  JSONB multi-path query: see timer above';

.timer on
SELECT COUNT(*) FROM sexp_nested 
WHERE sexp_contains_key(data, sexp('(click (user (type premium)))'));
.timer off
SELECT '  SEXP sexp_contains_key (multi-key): see timer above';
SELECT '';

-- ============================================================
-- Test 6: Serialization/Deserialization Round-trip
-- ============================================================

SELECT '--- Test 6: Round-trip Conversion (1000 iterations) ---';

-- JSON round-trip
.timer on
WITH RECURSIVE cnt(x) AS (
    SELECT 1 UNION ALL SELECT x+1 FROM cnt WHERE x < 1000
)
SELECT COUNT(*) FROM cnt, json_data 
WHERE json_data.id <= 10 
AND json(jsonb(json(data))) IS NOT NULL;
.timer off
SELECT '  JSONB round-trip: see timer above';

-- SEXP round-trip  
.timer on
WITH RECURSIVE cnt(x) AS (
    SELECT 1 UNION ALL SELECT x+1 FROM cnt WHERE x < 1000
)
SELECT COUNT(*) FROM cnt, sexp_data 
WHERE sexp_data.id <= 10 
AND sexp(sexp_text(data)) IS NOT NULL;
.timer off
SELECT '  SEXP round-trip: see timer above';
SELECT '';

-- ============================================================
-- Test 7: Complex Nested Structure
-- ============================================================

SELECT '--- Test 7: Deep Nesting Performance ---';

-- Create deeply nested structures
DROP TABLE IF EXISTS json_deep;
DROP TABLE IF EXISTS sexp_deep;

CREATE TABLE json_deep (id INTEGER PRIMARY KEY, data JSONB);
CREATE TABLE sexp_deep (id INTEGER PRIMARY KEY, data BLOB);

-- Insert deeply nested JSON
.timer on
INSERT INTO json_deep (data)
WITH RECURSIVE cnt(x) AS (
    SELECT 1 UNION ALL SELECT x+1 FROM cnt WHERE x < 1000
)
SELECT jsonb_object(
    'level1', jsonb_object(
        'level2', jsonb_object(
            'level3', jsonb_object(
                'level4', jsonb_object(
                    'value', x,
                    'name', 'item_' || x
                )
            )
        )
    )
) FROM cnt;
.timer off
SELECT '  JSONB deep insert: see timer above';

-- Insert deeply nested SEXP
.timer on
INSERT INTO sexp_deep (data)
WITH RECURSIVE cnt(x) AS (
    SELECT 1 UNION ALL SELECT x+1 FROM cnt WHERE x < 1000
)
SELECT sexp(
    '(level1 (level2 (level3 (level4 (value ' || x || ') (name "item_' || x || '")))))'
) FROM cnt;
.timer off
SELECT '  SEXP deep insert: see timer above';

-- Query deep nested value using sexp_get for cleaner access
-- Structure: (level1 (level2 (level3 (level4 (value N) (name "...")))))
-- Navigate through nested lists, then use sexp_get at the alist level
-- Each level returns TEXT, so we re-parse with sexp() for chaining
.timer on
SELECT COUNT(*) FROM json_deep 
WHERE json_extract(data, '$.level1.level2.level3.level4.value') > 500;
.timer off
SELECT '  JSONB deep extract: see timer above';

.timer on
SELECT COUNT(*) FROM sexp_deep 
WHERE sexp_get(sexp(sexp_nth(sexp(sexp_nth(sexp(sexp_nth(data, 1)), 1)), 1)), 'value') > 500;
.timer off
SELECT '  SEXP deep extract (sexp_get): see timer above';
SELECT '';

-- ============================================================
-- Test 8: Array/List Operations
-- ============================================================

SELECT '--- Test 8: Array/List Operations ---';

DROP TABLE IF EXISTS json_arrays;
DROP TABLE IF EXISTS sexp_lists;

CREATE TABLE json_arrays (id INTEGER PRIMARY KEY, data JSONB);
CREATE TABLE sexp_lists (id INTEGER PRIMARY KEY, data BLOB);

-- Insert arrays/lists
.timer on
INSERT INTO json_arrays (data)
WITH RECURSIVE cnt(x) AS (
    SELECT 1 UNION ALL SELECT x+1 FROM cnt WHERE x < 5000
)
SELECT jsonb_array(x, x+1, x+2, x+3, x+4, x+5, x+6, x+7, x+8, x+9) FROM cnt;
.timer off
SELECT '  JSONB array insert: see timer above';

.timer on
INSERT INTO sexp_lists (data)
WITH RECURSIVE cnt(x) AS (
    SELECT 1 UNION ALL SELECT x+1 FROM cnt WHERE x < 5000
)
SELECT sexp('(' || x || ' ' || (x+1) || ' ' || (x+2) || ' ' || (x+3) || ' ' || (x+4) || 
            ' ' || (x+5) || ' ' || (x+6) || ' ' || (x+7) || ' ' || (x+8) || ' ' || (x+9) || ')') FROM cnt;
.timer off
SELECT '  SEXP list insert: see timer above';

-- Array length
.timer on
SELECT SUM(json_array_length(data)) FROM json_arrays;
.timer off
SELECT '  JSONB array_length: see timer above';

.timer on
SELECT SUM(sexp_length(data)) FROM sexp_lists;
.timer off
SELECT '  SEXP sexp_length: see timer above';

-- Array element access (sexp_nth now returns native INTEGER)
.timer on
SELECT SUM(json_extract(data, '$[4]')) FROM json_arrays;
.timer off
SELECT '  JSONB element access: see timer above';

.timer on
SELECT SUM(sexp_nth(data, 4)) FROM sexp_lists;
.timer off
SELECT '  SEXP element access: see timer above';
SELECT '';

-- ============================================================
-- Test 9: String Content Performance
-- ============================================================

SELECT '--- Test 9: String-heavy Data ---';

DROP TABLE IF EXISTS json_strings;
DROP TABLE IF EXISTS sexp_strings;

CREATE TABLE json_strings (id INTEGER PRIMARY KEY, data JSONB);
CREATE TABLE sexp_strings (id INTEGER PRIMARY KEY, data BLOB);

-- Insert string-heavy data
.timer on
INSERT INTO json_strings (data)
WITH RECURSIVE cnt(x) AS (
    SELECT 1 UNION ALL SELECT x+1 FROM cnt WHERE x < 5000
)
SELECT jsonb_object(
    'title', 'This is a longer title for item number ' || x || ' in our test dataset',
    'description', 'This is an even longer description that contains more text to test string handling performance. Item ' || x,
    'tags', jsonb_array('tag1', 'tag2', 'tag3', 'category_' || (x % 10))
) FROM cnt;
.timer off
SELECT '  JSONB string insert: see timer above';

.timer on
INSERT INTO sexp_strings (data)
WITH RECURSIVE cnt(x) AS (
    SELECT 1 UNION ALL SELECT x+1 FROM cnt WHERE x < 5000
)
SELECT sexp(
    '(item (title "This is a longer title for item number ' || x || ' in our test dataset") ' ||
    '(description "This is an even longer description that contains more text to test string handling performance. Item ' || x || '") ' ||
    '(tags "tag1" "tag2" "tag3" "category_' || (x % 10) || '"))'
) FROM cnt;
.timer off
SELECT '  SEXP string insert: see timer above';

-- Storage comparison
SELECT '  JSONB string storage: ' || SUM(LENGTH(data)) || ' bytes' FROM json_strings;
SELECT '  SEXP string storage:  ' || SUM(LENGTH(data)) || ' bytes' FROM sexp_strings;
SELECT '';

-- ============================================================
-- Summary
-- ============================================================

SELECT '=== Benchmark Complete ===';
SELECT '';
SELECT 'Notes:';
SELECT '- JSONB is SQLite built-in, highly optimized';
SELECT '- SEXP uses symbol deduplication for repeated symbols';
SELECT '- SEXP containment is recursive structural matching';
SELECT '- JSONB has path-based extraction ($.foo.bar)';
SELECT '- SEXP uses positional access (nth) or car/cdr';
SELECT '';

-- Cleanup
DROP TABLE IF EXISTS json_data;
DROP TABLE IF EXISTS sexp_data;
DROP TABLE IF EXISTS json_nested;
DROP TABLE IF EXISTS sexp_nested;
DROP TABLE IF EXISTS json_deep;
DROP TABLE IF EXISTS sexp_deep;
DROP TABLE IF EXISTS json_arrays;
DROP TABLE IF EXISTS sexp_lists;
DROP TABLE IF EXISTS json_strings;
DROP TABLE IF EXISTS sexp_strings;
DROP TABLE IF EXISTS bench_results;
