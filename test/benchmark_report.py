#!/usr/bin/env python3
"""
Benchmark Report Generator for SQLite JSONB vs SEXP

Run the benchmark and generate a formatted report:
    python3 test/benchmark_report.py
"""

import subprocess
import re
import sys
from dataclasses import dataclass
from typing import List, Tuple


@dataclass
class BenchResult:
    test_name: str
    jsonb_time: float
    sexp_time: float
    jsonb_size: int = 0
    sexp_size: int = 0
    notes: str = ""


def run_benchmark() -> str:
    """Run the SQL benchmark and capture output."""
    result = subprocess.run(
        ["sqlite3"],
        input=open("test/benchmark.sql").read(),
        capture_output=True,
        text=True,
        cwd="/home/gdiazlo/data/src/sqlite_sexp",
    )
    return result.stdout + result.stderr


def parse_time(text: str) -> float:
    """Extract time in ms from 'Run Time: real X.XXX' format."""
    match = re.search(r"real (\d+\.\d+)", text)
    if match:
        return float(match.group(1)) * 1000  # Convert to ms
    return 0.0


def parse_size(text: str) -> int:
    """Extract size in bytes."""
    match = re.search(r"(\d+) bytes", text)
    if match:
        return int(match.group(1))
    return 0


def generate_report():
    """Generate formatted benchmark report."""

    print("=" * 70)
    print("       SQLite JSONB vs SEXP Extension Benchmark Report")
    print("=" * 70)
    print()

    # Run benchmark
    print("Running benchmark...")
    output = run_benchmark()
    lines = output.split("\n")

    results = []

    # Parse results
    i = 0
    while i < len(lines):
        line = lines[i]

        # Test 1: Insert Performance
        if "Test 1:" in line:
            jsonb_time = sexp_time = 0
            while i < len(lines) and "Test 2:" not in lines[i]:
                if "JSONB insert" in lines[i] and i > 0:
                    jsonb_time = parse_time(lines[i - 1])
                elif "SEXP insert" in lines[i] and i > 0:
                    sexp_time = parse_time(lines[i - 1])
                i += 1
            results.append(
                BenchResult(
                    "Insert 10K records",
                    jsonb_time,
                    sexp_time,
                    notes="Simple user records with 5 fields",
                )
            )
            continue

        # Test 2: Storage Size
        if "Test 2:" in line:
            jsonb_size = sexp_size = 0
            while i < len(lines) and "Test 3:" not in lines[i]:
                if "JSONB total size" in lines[i]:
                    jsonb_size = parse_size(lines[i])
                elif "SEXP total size" in lines[i]:
                    sexp_size = parse_size(lines[i])
                i += 1
            results.append(
                BenchResult(
                    "Storage Size (10K records)",
                    0,
                    0,
                    jsonb_size,
                    sexp_size,
                    notes="Total bytes for 10K records",
                )
            )
            continue

        # Test 3: Field Extraction
        if "Test 3:" in line:
            jsonb_time = sexp_time = 0
            while i < len(lines) and "Test 4:" not in lines[i]:
                if "json_extract" in lines[i] and i > 0:
                    jsonb_time = parse_time(lines[i - 1])
                elif "sexp_nth" in lines[i] and i > 0:
                    sexp_time = parse_time(lines[i - 1])
                i += 1
            results.append(
                BenchResult(
                    "Field Extraction (10K rows)",
                    jsonb_time,
                    sexp_time,
                    notes="Extract and filter on numeric field",
                )
            )
            continue

        # Test 4: Full Scan Filter
        if "Test 4:" in line:
            jsonb_time = sexp_time = 0
            while i < len(lines) and "Test 5:" not in lines[i]:
                if "JSONB filter" in lines[i] and i > 0:
                    jsonb_time = parse_time(lines[i - 1])
                elif "SEXP filter" in lines[i] and i > 0:
                    sexp_time = parse_time(lines[i - 1])
                i += 1
            results.append(
                BenchResult(
                    "Full Scan Filter (10K rows)",
                    jsonb_time,
                    sexp_time,
                    notes="Filter on boolean field",
                )
            )
            continue

        # Test 5: Containment
        if "Test 5:" in line:
            jsonb_time = sexp_time = 0
            while i < len(lines) and "Test 6:" not in lines[i]:
                if "path query" in lines[i] and i > 0:
                    jsonb_time = parse_time(lines[i - 1])
                elif "sexp_contains" in lines[i] and i > 0:
                    sexp_time = parse_time(lines[i - 1])
                i += 1
            results.append(
                BenchResult(
                    "Containment Query (10K rows)",
                    jsonb_time,
                    sexp_time,
                    notes="Find nested key-value pair",
                )
            )
            continue

        # Test 6: Round-trip
        if "Test 6:" in line:
            jsonb_time = sexp_time = 0
            while i < len(lines) and "Test 7:" not in lines[i]:
                if "JSONB round-trip" in lines[i] and i > 0:
                    jsonb_time = parse_time(lines[i - 1])
                elif "SEXP round-trip" in lines[i] and i > 0:
                    sexp_time = parse_time(lines[i - 1])
                i += 1
            results.append(
                BenchResult(
                    "Round-trip Conversion",
                    jsonb_time,
                    sexp_time,
                    notes="Parse -> serialize -> parse (1K iter)",
                )
            )
            continue

        # Test 7: Deep Nesting
        if "Test 7:" in line:
            jsonb_time = sexp_time = 0
            while i < len(lines) and "Test 8:" not in lines[i]:
                if "JSONB deep extract" in lines[i] and i > 0:
                    jsonb_time = parse_time(lines[i - 1])
                elif "SEXP deep extract" in lines[i] and i > 0:
                    sexp_time = parse_time(lines[i - 1])
                i += 1
            results.append(
                BenchResult(
                    "Deep Nesting Extract (1K rows)",
                    jsonb_time,
                    sexp_time,
                    notes="4-level deep field extraction",
                )
            )
            continue

        # Test 8: Array/List Length
        if "Test 8:" in line:
            jsonb_time = sexp_time = 0
            jsonb_elem = sexp_elem = 0
            while i < len(lines) and "Test 9:" not in lines[i]:
                if "array_length" in lines[i] and i > 0:
                    jsonb_time = parse_time(lines[i - 1])
                elif "sexp_length" in lines[i] and i > 0:
                    sexp_time = parse_time(lines[i - 1])
                elif "JSONB element access" in lines[i] and i > 0:
                    jsonb_elem = parse_time(lines[i - 1])
                elif "SEXP element access" in lines[i] and i > 0:
                    sexp_elem = parse_time(lines[i - 1])
                i += 1
            results.append(
                BenchResult(
                    "Array/List Length (5K rows)",
                    jsonb_time,
                    sexp_time,
                    notes="Get length of 10-element arrays",
                )
            )
            results.append(
                BenchResult(
                    "Array/List Element Access",
                    jsonb_elem,
                    sexp_elem,
                    notes="Access 5th element in 5K arrays",
                )
            )
            continue

        # Test 9: String Storage
        if "Test 9:" in line:
            jsonb_size = sexp_size = 0
            while i < len(lines) and "Benchmark Complete" not in lines[i]:
                if "JSONB string storage" in lines[i]:
                    jsonb_size = parse_size(lines[i])
                elif "SEXP string storage" in lines[i]:
                    sexp_size = parse_size(lines[i])
                i += 1
            results.append(
                BenchResult(
                    "String Storage (5K records)",
                    0,
                    0,
                    jsonb_size,
                    sexp_size,
                    notes="Long strings with descriptions",
                )
            )
            continue

        i += 1

    # Print Performance Results
    print("\n" + "=" * 70)
    print("                      PERFORMANCE RESULTS")
    print("=" * 70)
    print()
    print(f"{'Test':<35} {'JSONB':>10} {'SEXP':>10} {'Ratio':>10}")
    print("-" * 70)

    for r in results:
        if r.jsonb_time > 0 or r.sexp_time > 0:
            ratio = r.sexp_time / r.jsonb_time if r.jsonb_time > 0 else 0
            winner = "JSONB" if r.jsonb_time < r.sexp_time else "SEXP"
            print(
                f"{r.test_name:<35} {r.jsonb_time:>8.2f}ms {r.sexp_time:>8.2f}ms {ratio:>8.2f}x"
            )

    # Print Storage Results
    print("\n" + "=" * 70)
    print("                       STORAGE RESULTS")
    print("=" * 70)
    print()
    print(f"{'Test':<35} {'JSONB':>12} {'SEXP':>12} {'Ratio':>10}")
    print("-" * 70)

    for r in results:
        if r.jsonb_size > 0 or r.sexp_size > 0:
            ratio = r.sexp_size / r.jsonb_size if r.jsonb_size > 0 else 0
            print(
                f"{r.test_name:<35} {r.jsonb_size:>10} B {r.sexp_size:>10} B {ratio:>8.2f}x"
            )

    # Summary
    print("\n" + "=" * 70)
    print("                          SUMMARY")
    print("=" * 70)
    print("""
JSONB Advantages:
  - Built-in and highly optimized by SQLite team
  - Better storage efficiency (uses binary JSON format)
  - Path-based extraction with '$.foo.bar' syntax
  - Generally faster for most operations

SEXP Advantages:
  - Faster list length operation (O(1) count in header)
  - Similar round-trip performance
  - Symbol deduplication helps with repeated identifiers
  - Structural containment matching (@> semantics)
  - Lisp-friendly syntax for code-as-data

Recommendations:
  - Use JSONB for general-purpose structured data storage
  - Use SEXP for Lisp/Scheme ASTs, symbolic computation, or
    when structural pattern matching is important
  - SEXP excels when data has many repeated symbols
""")


if __name__ == "__main__":
    generate_report()
