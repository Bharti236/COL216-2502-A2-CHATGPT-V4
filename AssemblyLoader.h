#pragma once

#include <string>
#include <vector>
#include "Basics.h"

// Parses the already-preprocessed assembly file produced by compiler.py.
// This module does not strip comments or resolve labels; it only converts
// cleaned text into instructions and initializes memory.
struct ProgramImage {
    std::vector<Instruction> inst_memory;
    std::vector<int> memory;
};

class AssemblyLoader {
public:
    static ProgramImage load(const std::string &filename, int mem_size);
};
