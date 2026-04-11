#include "AssemblyLoader.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <cctype>

namespace {
    std::vector<std::string> tokenizeCopy(std::string s) {
        for (char &c : s) {
            if (c == ',' || c == '(' || c == ')') c = ' ';
        }
        std::istringstream iss(s);
        std::vector<std::string> tok;
        std::string w;
        while (iss >> w) tok.push_back(w);
        return tok;
    }

    int parseRegCopy(const std::string &s) {
        if (s.empty() || s[0] != 'x') return -1;
        return std::stoi(s.substr(1));
    }

    OpCode parseOpCopy(const std::string &s) {
        if (s == "add") return OpCode::ADD;
        if (s == "sub") return OpCode::SUB;
        if (s == "addi") return OpCode::ADDI;
        if (s == "mul") return OpCode::MUL;
        if (s == "div") return OpCode::DIV;
        if (s == "rem") return OpCode::REM;
        if (s == "lw") return OpCode::LW;
        if (s == "sw") return OpCode::SW;
        if (s == "beq") return OpCode::BEQ;
        if (s == "bne") return OpCode::BNE;
        if (s == "blt") return OpCode::BLT;
        if (s == "ble") return OpCode::BLE;
        if (s == "j") return OpCode::J;
        if (s == "slt") return OpCode::SLT;
        if (s == "slti") return OpCode::SLTI;
        if (s == "and") return OpCode::AND;
        if (s == "or") return OpCode::OR;
        if (s == "xor") return OpCode::XOR;
        if (s == "andi") return OpCode::ANDI;
        if (s == "ori") return OpCode::ORI;
        if (s == "xori") return OpCode::XORI;
        throw std::runtime_error("Unknown opcode: " + s);
    }

    int parseImmCopy(const std::string &s) {
        return std::stoi(s);
    }
}

ProgramImage AssemblyLoader::load(const std::string &filename, int mem_size) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open input file");
    }

    ProgramImage program;
    program.memory.assign(mem_size, 0);

    std::string line;
    int pc = 0;
    int mem_idx = 0;

    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        // The preprocessing script keeps memory declarations at the end,
        // so the loader only needs to parse the cleaned file format.
        if (line[0] == '.') {
            auto colon = line.find(':');
            if (colon == std::string::npos) {
                throw std::runtime_error("Bad memory declaration: " + line);
            }
            std::string values = line.substr(colon + 1);
            auto toks = tokenizeCopy(values);
            for (const auto &t : toks) {
                if (mem_idx >= mem_size) {
                    throw std::runtime_error("Memory declaration exceeds configured memory size");
                }
                program.memory[mem_idx++] = std::stoi(t);
            }
            continue;
        }

        auto tokens = tokenizeCopy(line);
        if (tokens.empty()) continue;

        Instruction inst{};
        inst.pc = pc++;
        inst.op = parseOpCopy(tokens[0]);

        switch (inst.op) {
            case OpCode::ADD:
            case OpCode::SUB:
            case OpCode::MUL:
            case OpCode::DIV:
            case OpCode::REM:
            case OpCode::SLT:
            case OpCode::AND:
            case OpCode::OR:
            case OpCode::XOR:
                if (tokens.size() < 4) throw std::runtime_error("Bad instruction: " + line);
                inst.dest = parseRegCopy(tokens[1]);
                inst.src1 = parseRegCopy(tokens[2]);
                inst.src2 = parseRegCopy(tokens[3]);
                break;

            case OpCode::ADDI:
            case OpCode::SLTI:
            case OpCode::ANDI:
            case OpCode::ORI:
            case OpCode::XORI:
                if (tokens.size() < 4) throw std::runtime_error("Bad instruction: " + line);
                inst.dest = parseRegCopy(tokens[1]);
                inst.src1 = parseRegCopy(tokens[2]);
                inst.imm = parseImmCopy(tokens[3]);
                break;

            case OpCode::LW:
                if (tokens.size() < 4) throw std::runtime_error("Bad instruction: " + line);
                inst.dest = parseRegCopy(tokens[1]);
                inst.imm = parseImmCopy(tokens[2]);
                inst.src1 = parseRegCopy(tokens[3]);
                break;

            case OpCode::SW:
                if (tokens.size() < 4) throw std::runtime_error("Bad instruction: " + line);
                inst.src1 = parseRegCopy(tokens[1]);
                inst.imm = parseImmCopy(tokens[2]);
                inst.src2 = parseRegCopy(tokens[3]);
                break;

            case OpCode::BEQ:
            case OpCode::BNE:
            case OpCode::BLT:
            case OpCode::BLE:
                if (tokens.size() < 4) throw std::runtime_error("Bad instruction: " + line);
                inst.src1 = parseRegCopy(tokens[1]);
                inst.src2 = parseRegCopy(tokens[2]);
                inst.imm = parseImmCopy(tokens[3]);
                break;

            case OpCode::J:
                if (tokens.size() < 2) throw std::runtime_error("Bad instruction: " + line);
                inst.imm = parseImmCopy(tokens[1]);
                break;
        }

        program.inst_memory.push_back(inst);
    }

    return program;
}
