# sqlite_sexp

A SQLite extension for storing and querying S-expressions as a native data type.

Works like SQLite's JSON functions: extraction functions return native SQL types directly.

## Quick Start

```sh
# Build
make

# Test
make test

# Use
sqlite3 mydb.db
```

```sql
.load ./sexp

-- Parse and store
CREATE TABLE config (id INTEGER PRIMARY KEY, data BLOB);
INSERT INTO config (data) VALUES (sexp('(server (host "localhost") (port 8080))'));

-- Query with native type returns (like json_extract)
SELECT sexp_get(data, 'host') FROM config;
-- localhost

SELECT sexp_get(data, 'port') + 1000 FROM config;
-- 9080  (integer arithmetic works directly!)

-- Nested access (like json_extract with $.user.name)
INSERT INTO config (data) VALUES (sexp('(app (db (host "db.local") (port 5432))))'));
SELECT sexp_get(data, 'db', 'host') FROM config WHERE id = 2;
-- db.local

-- Access by index
SELECT sexp_nth(sexp('(10 20 30)'), 1);
-- 20  (returns INTEGER, not blob)
```

## Installation

### From Source

Requirements: C99 compiler, SQLite development headers.

```sh
# Linux (Debian/Ubuntu)
sudo apt install build-essential libsqlite3-dev

# macOS
xcode-select --install
brew install sqlite

# Build and install
make
make install  # installs to /usr/local/lib/sqlite3
```

### Loading in SQLite

```sql
-- From current directory
.load ./sexp

-- From install location
.load /usr/local/lib/sqlite3/sexp

-- In application code
SELECT load_extension('/path/to/sexp');
```

Note: SQLite must be compiled with extension loading enabled. The `sqlite3` CLI has it enabled by default.

## Usage

### Storing S-expressions

```sql
-- Create a table with sexp data
CREATE TABLE events (
    id INTEGER PRIMARY KEY,
    timestamp TEXT DEFAULT CURRENT_TIMESTAMP,
    data BLOB
);

-- Insert s-expression data
INSERT INTO events (data) VALUES
    (sexp('(login (user "alice") (ip "192.168.1.1"))')),
    (sexp('(purchase (user "bob") (item "widget") (price 29.99))')),
    (sexp('(logout (user "alice"))'));
```

### Native Type Returns (like json_extract)

Extraction functions return native SQL types, not blobs:

| S-expression Type | SQL Type Returned |
|-------------------|-------------------|
| integer | INTEGER |
| float | REAL |
| symbol | TEXT |
| string | TEXT (unquoted) |
| list | BLOB (for further processing) |
| nil | NULL |

```sql
-- Direct value extraction - no conversion functions needed!
SELECT sexp_car(sexp('(click user123)'));      -- 'click' (TEXT)
SELECT sexp_nth(sexp('(10 20 30)'), 1);        -- 20 (INTEGER)
SELECT sexp_path(sexp('((a 1.5))'), 0, 1);     -- 1.5 (REAL)

-- Works in expressions
SELECT sexp_nth(sexp('(10 20 30)'), 0) + sexp_nth(sexp('(10 20 30)'), 1);  -- 30

-- String comparison works directly
SELECT * FROM events WHERE sexp_car(data) = 'login';
```

### Key-Based Extraction with sexp_get

`sexp_get(blob, key1, key2, ...)` extracts values by key path from alist-style structures (like `json_extract`):

```sql
-- Structure: (tag (key1 value1) (key2 value2) ...)
SELECT sexp_get(sexp('(user (name "alice") (age 30))'), 'name');  -- 'alice'
SELECT sexp_get(sexp('(user (name "alice") (age 30))'), 'age');   -- 30

-- Use in queries
SELECT * FROM events WHERE sexp_get(data, 'user') = 'alice';

-- Arithmetic on extracted values
SELECT * FROM events WHERE sexp_get(data, 'price') > 20.00;

-- NESTED ACCESS: Multiple keys navigate through nested structures
-- Structure: (event (user (name "alice") (type "premium")) (metadata ...))
SELECT sexp_get(data, 'user', 'name');   -- 'alice' (no re-parsing needed!)
SELECT sexp_get(data, 'user', 'type');   -- 'premium'

-- Equivalent to JSONB: json_extract(data, '$.user.name')
-- Three levels deep:
SELECT sexp_get(data, 'metadata', 'product', 'version');
```

### Querying Data

```sql
-- Get event type (first element) - returns TEXT directly
SELECT id, sexp_car(data) as event_type FROM events;
-- 1|login
-- 2|purchase
-- 3|logout

-- Filter by event type - direct comparison
SELECT * FROM events WHERE sexp_car(data) = 'login';

-- Extract nested values using sexp_get
SELECT sexp_get(data, 'user') as user FROM events;
-- alice
-- bob
-- alice

-- Or using path-based access
SELECT sexp_path(data, 1, 1) as user FROM events;

-- Find events containing specific data
SELECT * FROM events WHERE sexp_contains(data, sexp('(user "alice")'));

-- Key-based containment (order-independent matching)
SELECT * FROM events 
WHERE sexp_contains_key(data, sexp('(purchase (price 29.99))'));
```

### Working with Lists

```sql
-- Access list elements (scalars return native types)
SELECT sexp_car(sexp('(a b c)'));              -- 'a' (TEXT)
SELECT sexp_text(sexp_cdr(sexp('(a b c)')));   -- '(b c)' (cdr always returns list)
SELECT sexp_nth(sexp('(a b c)'), 1);           -- 'b' (TEXT)
SELECT sexp_length(sexp('(a b c)'));           -- 3

-- When the element is a list, you get a blob back
SELECT typeof(sexp_nth(sexp('((nested) b c)'), 0));  -- 'blob'
SELECT sexp_text(sexp_nth(sexp('((nested) b c)'), 0));  -- '(nested)'

-- Deep access with sexp_path
SELECT sexp_path(sexp('((a 1) (b 2) (c 3))'), 1, 0);   -- 'b'
SELECT sexp_path(sexp('((a 1) (b 2) (c 3))'), 2, 1);   -- 3
```

### Explicit Type Conversion (when needed)

For cases where you have a sexp blob and need a specific type:

```sql
-- These work on sexp blobs (e.g., from stored data or sexp() function)
SELECT sexp_int_value(sexp('42'));           -- 42 (INTEGER)
SELECT sexp_float_value(sexp('3.14'));       -- 3.14 (REAL)
SELECT sexp_string_value(sexp('"hello"'));   -- hello (TEXT)
SELECT sexp_symbol_value(sexp('foo'));       -- foo (TEXT)
SELECT sexp_number_value(sexp('42'));        -- 42 (auto: INTEGER or REAL)
```

### Type Checking

```sql
-- Type predicates return 1 (true) or 0 (false)
-- O(1) performance - no full parsing needed
SELECT is_list(sexp('(a b)'));     -- 1
SELECT is_symbol(sexp('foo'));     -- 1
SELECT is_string(sexp('"foo"'));   -- 1
SELECT is_integer(sexp('42'));     -- 1
SELECT is_float(sexp('3.14'));     -- 1
SELECT is_number(sexp('42'));      -- 1
SELECT is_nil(sexp('()'));         -- 1
SELECT is_atom(sexp('foo'));       -- 1

-- Get type name
SELECT sexp_typeof(sexp('42'));      -- integer
SELECT sexp_typeof(sexp('(a b)'));   -- list
```

## Function Reference

| Function | Returns | Description |
|----------|---------|-------------|
| `sexp(text)` | blob | Parse text to sexp |
| `sexp_text(blob)` | text | Convert sexp to text |
| `sexp_get(blob, key, ...)` | native | Get value by key path (like json_extract) |
| `sexp_car(blob)` | native | First element (native type or blob) |
| `sexp_cdr(blob)` | blob | All but first element |
| `sexp_nth(blob, n)` | native | Element at index n (native type or blob) |
| `sexp_path(blob, i, ...)` | native | Nested element access (native type or blob) |
| `sexp_length(blob)` | int | Number of elements |
| `sexp_typeof(blob)` | text | Type name |
| `sexp_string_value(blob)` | text | String content (explicit) |
| `sexp_symbol_value(blob)` | text | Symbol name (explicit) |
| `sexp_int_value(blob)` | int | Integer value (explicit) |
| `sexp_float_value(blob)` | real | Float value (explicit) |
| `sexp_number_value(blob)` | int/real | Numeric value (explicit) |
| `sexp_contains(a, b)` | int | Does a contain b? |
| `sexp_contains_key(a, b)` | int | Key-based containment |
| `is_nil(blob)` | int | Is empty list? |
| `is_list(blob)` | int | Is list? |
| `is_atom(blob)` | int | Is atom? |
| `is_symbol(blob)` | int | Is symbol? |
| `is_string(blob)` | int | Is string? |
| `is_number(blob)` | int | Is number? |
| `is_integer(blob)` | int | Is integer? |
| `is_float(blob)` | int | Is float? |

Aliases: `sexp_head` = `sexp_car`, `sexp_tail` = `sexp_cdr`

**Native type returns**: `sexp_get`, `sexp_car`, `sexp_nth`, and `sexp_path` return:
- INTEGER for sexp integers
- REAL for sexp floats  
- TEXT for sexp symbols and strings
- BLOB for sexp lists (for chaining)
- NULL for nil

## S-expression Syntax

| Type | Syntax | Examples |
|------|--------|----------|
| Symbol | identifier | `foo`, `my-var`, `+`, `null?` |
| Integer | digits | `42`, `-100`, `0` |
| Float | decimal | `3.14`, `-1.5e10` |
| String | quoted | `"hello"`, `"with \"escapes\""` |
| List | parentheses | `(a b c)`, `(+ 1 2)` |
| Empty list | `()` or `nil` | `()` |

Comments start with `;` and continue to end of line.

## Building

```sh
make                 # Build for current platform
make test            # Run tests
make debug           # Build with debug symbols
make install         # Install to /usr/local/lib/sqlite3
make clean           # Remove build artifacts

# Cross-compilation
make TARGET_OS=linux
make TARGET_OS=macos  
make TARGET_OS=windows  # requires mingw-w64
```

## License

MIT
