#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <deque>
#include <cstdint>

// ISA opcodes supported by the assignment.
enum class OpCode {
    ADD, SUB, ADDI, MUL, DIV, REM, LW, SW, BEQ, BNE, BLT, BLE, J,
    SLT, SLTI, AND, OR, XOR, ANDI, ORI, XORI
};

enum class UnitType { ADDER, MULTIPLIER, DIVIDER, LOADSTORE, BRANCH, LOGIC };

struct Instruction {
    OpCode op = OpCode::ADD;
    int dest = -1;
    int src1 = -1;
    int src2 = -1;
    int imm = 0;
    int pc = 0;
    std::string text;
};

struct ProcessorConfig {
    int num_regs = 32;
    int rob_size = 64;
    int mem_size = 1024;

    int logic_lat = 1;
    int add_lat = 2;
    int mul_lat = 4;
    int div_lat = 5;
    int mem_lat = 4;

    int logic_rs_size = 4;
    int adder_rs_size = 4;
    int mult_rs_size = 2;
    int div_rs_size = 2;
    int br_rs_size = 2;
    int lsq_rs_size = 32;
};

// One ROB entry per in-flight instruction.
struct ROBEntry {
    bool busy = false;
    bool ready = false;
    bool has_exception = false;

    int tag = -1;
    int pc = -1;
    Instruction inst;

    int dest = -1;
    int value = 0;
    int addr = 0;
    int store_data = 0;

    bool branch_taken = false;
    int predicted_target = -1;
    int actual_target = -1;

    // Previous RAT mapping for rollback during branch misprediction recovery.
    int prev_rename = -1;
};

// Reservation-station entry for ALU-style units.
struct RSEntry {
    bool busy = false;
    OpCode op = OpCode::ADD;
    int rob_tag = -1;
    int dest = -1;
    int Vj = 0, Vk = 0;
    int Qj = -1, Qk = -1;
    int imm = 0;
    int pc = -1;
    int insert_cycle = 0;
    int age = 0;
};

// Reservation-station entry for the LSQ.
struct LSQEntry {
    bool busy = false;
    OpCode op = OpCode::LW;
    int rob_tag = -1;
    int dest = -1;
    int Vj = 0, Vk = 0;      // operands
    int Qj = -1, Qk = -1;    // tags for pending operands
    int imm = 0;
    int pc = -1;
    int insert_cycle = 0;
    int age = 0;

    int addr = 0;
    int store_data = 0;
};

// Common Data Bus event emitted by execution units.
struct BroadcastEvent {
    int rob_tag = -1;
    bool has_value = false;
    int value = 0;

    bool has_store = false;
    int addr = 0;
    int store_data = 0;

    bool has_branch = false;
    bool branch_taken = false;
    int actual_target = -1;

    bool has_exception = false;
};
