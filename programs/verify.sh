#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"
compiler="$repo_root/compiler.py"

simulator=""
if [[ -x "$repo_root/main" ]]; then
    simulator="$repo_root/main"
elif [[ -x "$repo_root/main.exe" ]]; then
    simulator="$repo_root/main.exe"
else
    echo "Could not find simulator executable. Expected 'main' or 'main.exe' in the repository root." >&2
    exit 1
fi

if [[ ! -f "$compiler" ]]; then
    echo "Could not find compiler.py in the repository root." >&2
    exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 is required to preprocess test files." >&2
    exit 1
fi

normalize_lines() {
    # Remove empty lines and trailing whitespace
    # Also strip UTF-8 BOM if present
    sed '1s/^\xef\xbb\xbf//' | grep -v '^[[:space:]]*$' | sed 's/[[:space:]]*$//'
}

convert_answer() {
    # Detect file encoding and convert to UTF-8 if needed
    local encoding=$(file -b --mime-encoding "$1")
    if [[ "$encoding" == *"utf-16"* ]]; then
        # For UTF-16, use UTF-16 (not UTF-16LE) to handle BOM properly
        iconv -f UTF-16 -t UTF-8 "$1" 2>/dev/null || cat "$1"
    elif [[ "$encoding" == *"utf-8"* ]]; then
        cat "$1"
    else
        cat "$1"
    fi
}

status=0

cd "$script_dir"

# Process all code files regardless of naming pattern
for test_file in code*.txt; do
    [[ -e "$test_file" ]] || continue

    answer_file="${test_file/code/ans}"
    
    if [[ ! -f "$answer_file" ]]; then
        echo "$(basename "$test_file"): MISSING ANSWER"
        status=1
        continue
    fi

    prepared_file="$(mktemp)"
    actual_raw_file="$(mktemp)"
    actual_file="$(mktemp)"
    expected_file="$(mktemp)"
    error_file="$(mktemp)"

    cp "$test_file" "$prepared_file"

    if ! python3 "$compiler" "$prepared_file" >"$error_file" 2>&1; then
        echo "$(basename "$test_file"): PREPROCESS FAIL"
        sed 's/^/  /' "$error_file" >&2
        status=1
        rm -f "$prepared_file" "$actual_raw_file" "$actual_file" "$expected_file" "$error_file"
        continue
    fi

    if ! "$simulator" "$prepared_file" >"$actual_raw_file" 2>"$error_file"; then
        echo "$(basename "$test_file"): EXEC FAIL"
        sed 's/^/  /' "$error_file" >&2
        status=1
        rm -f "$prepared_file" "$actual_raw_file" "$actual_file" "$expected_file" "$error_file"
        continue
    fi

    normalize_lines < "$actual_raw_file" > "$actual_file"
    convert_answer "$answer_file" | normalize_lines > "$expected_file"

    if diff -u "$expected_file" "$actual_file" >/dev/null 2>&1; then
        echo "$(basename "$test_file"): PASS"
    else
        echo "$(basename "$test_file"): FAIL"
        status=1
    fi

    rm -f "$prepared_file" "$actual_raw_file" "$actual_file" "$expected_file" "$error_file"
done

exit "$status"
