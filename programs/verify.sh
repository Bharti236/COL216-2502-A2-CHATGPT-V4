#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"

simulator=""
if [[ -x "$script_dir/../main" ]]; then
    simulator="$script_dir/../main"
elif [[ -x "$script_dir/../main.exe" ]]; then
    simulator="$script_dir/../main.exe"
else
    echo "Could not find simulator executable. Expected 'main' or 'main.exe' in the repository root." >&2
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

    actual_file="$(mktemp)"
    expected_file="$(mktemp)"

    "$simulator" "$test_file" | normalize_lines > "$actual_file"
    convert_answer "$answer_file" | normalize_lines > "$expected_file"

    if diff -u "$expected_file" "$actual_file" >/dev/null 2>&1; then
        echo "$(basename "$test_file"): PASS"
    else
        echo "$(basename "$test_file"): FAIL"
        status=1
    fi

    rm -f "$actual_file" "$expected_file"
done

exit "$status"
