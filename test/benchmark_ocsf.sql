-- Benchmark: OCSF Simulation (Deep Nested Objects, 100k rows)
-- Run with: sqlite3 < test/benchmark_ocsf.sql

.load ./sexp
.mode list
.headers off

SELECT '=== SQLite JSONB vs SEXP: OCSF Simulation (100k rows) ===' as info;
SELECT '';

DROP TABLE IF EXISTS ocsf_json;
DROP TABLE IF EXISTS ocsf_sexp;

CREATE TABLE ocsf_json (id INTEGER PRIMARY KEY, data JSONB);
CREATE TABLE ocsf_sexp (id INTEGER PRIMARY KEY, data BLOB);

SELECT '--- Generating 100,000 OCSF-like events ---';
SELECT 'Structure: Metadata, Device, File, User objects with nested fields.';

-- Generate data
-- We use a fixed seed-like generation for reproducibility
.timer on

INSERT INTO ocsf_json (data)
WITH RECURSIVE cnt(x) AS (
    SELECT 1
    UNION ALL
    SELECT x+1 FROM cnt WHERE x < 100000
)
SELECT jsonb_object(
    'class_uid', 1001,
    'category_uid', 1,
    'activity_id', 1,
    'metadata', jsonb_object(
        'product', jsonb_object(
            'name', 'Sensor',
            'vendor_name', 'SecurityCorp',
            'version', '1.0.' || (x % 10)
        ),
        'time', 1678900000 + x,
        'log_level', CASE (x % 3) WHEN 0 THEN 'Info' WHEN 1 THEN 'Warning' ELSE 'Error' END
    ),
    'device', jsonb_object(
        'hostname', 'laptop-' || (x % 1000),
        'ip', '192.168.' || ((x / 256) % 256) || '.' || (x % 256),
        'mac', '00:11:22:33:44:' || printf('%02X', x % 256),
        'agent_id', 'agt_' || x
    ),
    'file', jsonb_object(
        'name', 'file_' || (x % 500) || '.exe',
        'path', 'C:\\Windows\\Temp\\file_' || (x % 500) || '.exe',
        'size', x * 1024,
        'hashes', jsonb_array(
            jsonb_object('algorithm', 'SHA256', 'value', 'hash_' || x),
            jsonb_object('algorithm', 'MD5', 'value', 'md5_' || x)
        )
    ),
    'user', jsonb_object(
        'name', 'user_' || (x % 50),
        'uid', 'u_' || (x % 50),
        'domain', 'CORP',
        'groups', jsonb_array('admin', 'users', 'remote')
    )
) FROM cnt;

.timer off
SELECT '  JSONB insert (100k): see timer above';

.timer on

INSERT INTO ocsf_sexp (data)
WITH RECURSIVE cnt(x) AS (
    SELECT 1
    UNION ALL
    SELECT x+1 FROM cnt WHERE x < 100000
)
SELECT sexp(
    '(event ' ||
    '(class_uid 1001) ' ||
    '(category_uid 1) ' ||
    '(activity_id 1) ' ||
    '(metadata ' ||
        '(product (name "Sensor") (vendor_name "SecurityCorp") (version "1.0.' || (x % 10) || '")) ' ||
        '(time ' || (1678900000 + x) || ') ' ||
        '(log_level "' || CASE (x % 3) WHEN 0 THEN 'Info' WHEN 1 THEN 'Warning' ELSE 'Error' END || '")' ||
    ') ' ||
    '(device ' ||
        '(hostname "laptop-' || (x % 1000) || '") ' ||
        '(ip "192.168.' || ((x / 256) % 256) || '.' || (x % 256) || '") ' ||
        '(mac "00:11:22:33:44:' || printf('%02X', x % 256) || '") ' ||
        '(agent_id "agt_' || x || '")' ||
    ') ' ||
    '(file ' ||
        '(name "file_' || (x % 500) || '.exe") ' ||
        '(path "C:\\Windows\\Temp\\file_' || (x % 500) || '.exe") ' ||
        '(size ' || (x * 1024) || ') ' ||
        '(hashes ' ||
            '((algorithm "SHA256") (value "hash_' || x || '")) ' ||
            '((algorithm "MD5") (value "md5_' || x || '"))' ||
        ')' ||
    ') ' ||
    '(user ' ||
        '(name "user_' || (x % 50) || '") ' ||
        '(uid "u_' || (x % 50) || '") ' ||
        '(domain "CORP") ' ||
        '(groups "admin" "users" "remote")' ||
    ')' ||
    ')'
) FROM cnt;

.timer off
SELECT '  SEXP insert (100k): see timer above';
SELECT '';

SELECT '--- Test 2: Storage Size ---';
SELECT '  JSONB total: ' || SUM(LENGTH(data))/1024/1024 || ' MB' FROM ocsf_json;
SELECT '  SEXP total:  ' || SUM(LENGTH(data))/1024/1024 || ' MB' FROM ocsf_sexp;
SELECT '  JSONB avg:   ' || CAST(AVG(LENGTH(data)) AS INTEGER) || ' bytes' FROM ocsf_json;
SELECT '  SEXP avg:    ' || CAST(AVG(LENGTH(data)) AS INTEGER) || ' bytes' FROM ocsf_sexp;
SELECT '';

SELECT '--- Test 3: Query by Device IP (Deep Scan) ---';
SELECT 'Target IP: 192.168.0.100 (Expected ~1 hit)';

.timer on
SELECT COUNT(*) FROM ocsf_json 
WHERE json_extract(data, '$.device.ip') = '192.168.0.100';
.timer off
SELECT '  JSONB scan: see timer above';

-- SEXP structure: (event (class_uid ...) (category_uid ...) (activity_id ...) (metadata ...) (device ...) ...)
-- Use variadic sexp_get for direct nested access (no re-parsing!)

.timer on
SELECT COUNT(*) FROM ocsf_sexp 
WHERE sexp_get(data, 'device', 'ip') = '192.168.0.100';
.timer off
SELECT '  SEXP scan (sexp_get variadic): see timer above';
SELECT '';

SELECT '--- Test 4: Query by File Name (Deep Scan) ---';
SELECT 'Target: file_100.exe (Expected ~200 hits)';

.timer on
SELECT COUNT(*) FROM ocsf_json
WHERE json_extract(data, '$.file.name') = 'file_100.exe';
.timer off
SELECT '  JSONB scan: see timer above';

-- SEXP: Use variadic sexp_get for direct nested access (no re-parsing!)

.timer on
SELECT COUNT(*) FROM ocsf_sexp
WHERE sexp_get(data, 'file', 'name') = 'file_100.exe';
.timer off
SELECT '  SEXP scan (sexp_get variadic): see timer above';
SELECT '';

SELECT '=== Benchmark Complete ===';
