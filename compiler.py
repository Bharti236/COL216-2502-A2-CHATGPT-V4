#!/usr/bin/env python3
"""
Preprocess the RISC-V-like assembly file in-place.

This script performs two jobs:
1) Removes comments and blank lines.
2) Resolves labels so the C++ simulator can read a simplified file.
"""

import sys
import re
from pathlib import Path


def strip_comment(line: str) -> str:
    if '#' in line:
        line = line[: line.index('#')]
    return line.strip()


def tokenize(s: str):
    s = s.replace(',', ' ').replace('(', ' ').replace(')', ' ')
    return [t for t in s.split() if t]


def is_number(s: str) -> bool:
    return bool(re.fullmatch(r'[+-]?\d+', s))


def main(path: str):
    p = Path(path)
    lines = [strip_comment(l) for l in p.read_text().splitlines()]
    lines = [l for l in lines if l]

    inst_labels = {}
    mem_labels = {}
    inst_lines = []
    mem_blocks = []
    pc = 0
    mem_addr = 0

    # First pass: collect labels and split memory blocks / instruction lines.
    for raw in lines:
        s = raw.strip()
        if not s:
            continue

        if s.startswith('.'):
            colon = s.find(':')
            if colon == -1:
                raise ValueError(f'Bad memory declaration: {s}')
            label = s[1:colon].strip()
            values = s[colon + 1 :].strip()
            toks = values.split()
            vals = []
            for t in toks:
                if not is_number(t):
                    raise ValueError(f'Bad memory value {t} in {s}')
                vals.append(int(t))
            mem_labels[label] = mem_addr
            mem_addr += len(vals)
            mem_blocks.append((label, vals))
            continue

        colon = s.find(':')
        if colon != -1:
            label = s[:colon].strip()
            rest = s[colon + 1 :].strip()
            inst_labels[label] = pc
            if rest:
                inst_lines.append(rest)
                pc += 1
            continue

        inst_lines.append(s)
        pc += 1

    # Second pass: rewrite branch targets and memory-label operands.
    out = []
    pc = 0
    for line in inst_lines:
        toks = tokenize(line)
        if not toks:
            continue
        op = toks[0]

        def resolve_mem(tok: str) -> str:
            if is_number(tok):
                return tok
            key = tok[1:] if tok.startswith('.') else tok
            if key not in mem_labels:
                raise ValueError(f'Unknown memory label: {tok}')
            return str(mem_labels[key])

        def resolve_branch(tok: str) -> str:
            if is_number(tok):
                return tok
            if tok not in inst_labels:
                raise ValueError(f'Unknown branch label: {tok}')
            return str(inst_labels[tok] - pc)

        if op in {'lw', 'sw'}:
            if len(toks) != 4:
                raise ValueError(f'Bad memory instruction: {line}')
            out.append(f'{op} {toks[1]}, {resolve_mem(toks[2])}({toks[3]})')
        elif op in {'beq', 'bne', 'blt', 'ble', 'j'}:
            if op == 'j':
                if len(toks) != 2:
                    raise ValueError(f'Bad j instruction: {line}')
                out.append(f'{op} {resolve_branch(toks[1])}')
            else:
                if len(toks) != 4:
                    raise ValueError(f'Bad branch instruction: {line}')
                out.append(f'{op} {toks[1]}, {toks[2]}, {resolve_branch(toks[3])}')
        else:
            out.append(line)
        pc += 1

    # Memory declarations are preserved at the end of the file.
    out.extend([f'.{label}: ' + ' '.join(map(str, vals)) for label, vals in mem_blocks])
    p.write_text('\n'.join(out) + '\n')


if __name__ == '__main__':
    if len(sys.argv) != 2:
        print('Usage: python3 compiler.py <filename.s>', file=sys.stderr)
        sys.exit(1)
    main(sys.argv[1])
