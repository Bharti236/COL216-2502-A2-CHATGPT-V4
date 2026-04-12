#include "Processor.h"
#include "AssemblyLoader.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <stdexcept>
#include <limits>

namespace {
const char *opcodeName(OpCode op) {
    switch (op) {
        case OpCode::ADD: return "ADD";
        case OpCode::SUB: return "SUB";
        case OpCode::ADDI: return "ADDI";
        case OpCode::MUL: return "MUL";
        case OpCode::DIV: return "DIV";
        case OpCode::REM: return "REM";
        case OpCode::LW: return "LW";
        case OpCode::SW: return "SW";
        case OpCode::BEQ: return "BEQ";
        case OpCode::BNE: return "BNE";
        case OpCode::BLT: return "BLT";
        case OpCode::BLE: return "BLE";
        case OpCode::J: return "J";
        case OpCode::SLT: return "SLT";
        case OpCode::SLTI: return "SLTI";
        case OpCode::AND: return "AND";
        case OpCode::OR: return "OR";
        case OpCode::XOR: return "XOR";
        case OpCode::ANDI: return "ANDI";
        case OpCode::ORI: return "ORI";
        case OpCode::XORI: return "XORI";
    }
    return "UNKNOWN";
}
}

Processor::Processor(ProcessorConfig &cfg) : config(cfg) {
    pc = 0;
    clock_cycle = 0;
    ARF.assign(config.num_regs, 0);
    Memory.assign(config.mem_size, 0);
    RAT.assign(config.num_regs, -1);
    RAT[0] = -1;

    units.emplace_back(UnitType::ADDER, config.add_lat, config.adder_rs_size);
    units.emplace_back(UnitType::MULTIPLIER, config.mul_lat, config.mult_rs_size);
    units.emplace_back(UnitType::DIVIDER, config.div_lat, config.div_rs_size);
    units.emplace_back(UnitType::BRANCH, config.add_lat, config.br_rs_size); // branch lat to be assumed equal to add lat as instructed
    units.emplace_back(UnitType::LOGIC, config.logic_lat, config.logic_rs_size);
    lsq = new LoadStoreQueue(config.mem_lat, config.lsq_rs_size);
}



bool Processor::isBranchOp(OpCode op) {
    return op == OpCode::BEQ || op == OpCode::BNE || op == OpCode::BLT || op == OpCode::BLE;
}

bool Processor::writesRegister(OpCode op) {
    return op == OpCode::ADD || op == OpCode::SUB || op == OpCode::ADDI ||
           op == OpCode::MUL || op == OpCode::DIV || op == OpCode::REM ||
           op == OpCode::LW || op == OpCode::SLT || op == OpCode::SLTI ||
           op == OpCode::AND || op == OpCode::OR || op == OpCode::XOR ||
           op == OpCode::ANDI || op == OpCode::ORI || op == OpCode::XORI;
}

bool Processor::isLogicOp(OpCode op) {
    return op == OpCode::AND || op == OpCode::OR || op == OpCode::XOR ||
           op == OpCode::ANDI || op == OpCode::ORI || op == OpCode::XORI;
}

int Processor::allocateROB(const Instruction &inst) {
    if ((int)rob_order.size() >= config.rob_size) return -1;
    int tag = next_rob_tag++;
    ROBEntry e;
    e.busy = true;
    e.ready = false;
    e.tag = tag;
    e.pc = inst.pc;
    e.inst = inst;
    e.dest = inst.dest;
    rob_table[tag] = e;
    rob_order.push_back(tag);
    return tag;
}

ROBEntry *Processor::getROB(int tag) {
    auto it = rob_table.find(tag);
    if (it == rob_table.end()) return nullptr;
    return &it->second;
}

bool Processor::robEmpty() const { return rob_order.empty(); }

void Processor::clearFrontEnd() {
    fetch_buffer_valid = false;
    fetch_buffer_pred_pc = -1;
}

void Processor::flushSpeculationPreserveRAT() {
    clearFrontEnd();
    for (auto &u : units) u.flush();
    if (lsq) lsq->flush();
    rob_order.clear();
    rob_table.clear();
}

void Processor::flushAll() {
    flushSpeculationPreserveRAT();
    std::fill(RAT.begin(), RAT.end(), -1);
    enforceX0Zero();
}

void Processor::flush() {
    // Public helper: clear speculative state while keeping the architectural state.
    flushSpeculationPreserveRAT();
}

void Processor::loadProgram(const std::string &filename) {
    setupLogging();

    // Reset machine state so loadProgram can be called more than once safely.
    pc = 0;
    clock_cycle = 0;
    exception = false;
    squash_rest_of_cycle = false;
    std::fill(ARF.begin(), ARF.end(), 0);
    std::fill(Memory.begin(), Memory.end(), 0);
    std::fill(RAT.begin(), RAT.end(), -1);
    enforceX0Zero();
    inst_memory.clear();
    rob_order.clear();
    rob_table.clear();
    next_rob_tag = 0;
    clearFrontEnd();
    pending_cdb.clear();
    for (auto &u : units) u.flush();
    if (lsq) lsq->flush();
    bp.state.clear();
    bp.total_branches = 0;
    bp.correct_predictions = 0;

    ProgramImage program = AssemblyLoader::load(filename, config.mem_size);
    inst_memory = std::move(program.inst_memory);
    Memory = std::move(program.memory);
    enforceX0Zero();
    logEvent("Loaded program: " + filename + " (" + std::to_string(inst_memory.size()) + " instructions)");
}

void Processor::enqueueToUnit(const RSEntry &e) {
    if (e.op == OpCode::MUL) {
        units[1].addEntry(e);
    } else if (e.op == OpCode::DIV || e.op == OpCode::REM) {
        units[2].addEntry(e);
    } else if (isBranchOp(e.op)) {
        units[3].addEntry(e);
    } else if (isLogicOp(e.op)) {
        units[4].addEntry(e);
    } else {
        units[0].addEntry(e);
    }
}

bool Processor::operandReady(int reg, int &val, int &tag) {
    if (reg < 0) {
        val = 0;
        tag = -1;
        return true;
    }
    if (reg == 0) {
        val = 0;
        tag = -1;
        return true;
    }

    int producer = RAT[reg];
    if (producer == -1) {
        val = ARF[reg];
        tag = -1;
        return true;
    }

    auto *re = getROB(producer);
    if (!re) {
        RAT[reg] = -1;
        val = ARF[reg];
        tag = -1;
        return true;
    }

    if (re && re->ready && !re->has_exception) {
        val = re->value;
        tag = -1;
        return true;
    }

    tag = producer;
    val = 0;
    return false;
}

void Processor::stageFetch() {
    if (exception) {
        logEvent("Fetch: skipped because exception is set");
        return;
    }
    if (fetch_buffer_valid) {
        logEvent("Fetch: stalled because fetch buffer is occupied");
        return;
    }
    if (pc < 0 || pc >= (int)inst_memory.size()) {
        logEvent("Fetch: no instruction to fetch");
        return;
    }

    Instruction inst = inst_memory[pc];
    inst.pc = pc;
    fetch_buffer_inst = inst;
    fetch_buffer_valid = true;

    if (inst.op == OpCode::J) {
        fetch_buffer_pred_pc = pc + inst.imm;
        pc = fetch_buffer_pred_pc;
    } else if (isBranchOp(inst.op)) {
        fetch_buffer_pred_pc = bp.predict(pc, inst.imm, inst.op);
        pc = fetch_buffer_pred_pc;
    } else {
        fetch_buffer_pred_pc = pc + 1;
        pc = fetch_buffer_pred_pc;
    }

    logEvent(
        "Fetch: pc=" + std::to_string(inst.pc) +
        " op=" + opcodeName(inst.op) +
        " predicted_next_pc=" + std::to_string(fetch_buffer_pred_pc)
    );
}

void Processor::stageDecode() {
    if (exception) {
        logEvent("Decode: skipped because exception is set");
        return;
    }
    if (!fetch_buffer_valid) {
        logEvent("Decode: no fetched instruction");
        return;
    }
    const Instruction &inst = fetch_buffer_inst;

    if ((int)rob_order.size() >= config.rob_size) {
        logEvent("Decode: stalled because ROB is full");
        return;
    }

    bool can_issue = true;
    if (inst.op == OpCode::LW || inst.op == OpCode::SW) {
        can_issue = !lsq->rsFull(config.lsq_rs_size);
    } else if (inst.op == OpCode::J) {
        can_issue = true;
    } else if (inst.op == OpCode::MUL) {
        can_issue = !units[1].rsFull(config.mult_rs_size);
    } else if (inst.op == OpCode::DIV || inst.op == OpCode::REM) {
        can_issue = !units[2].rsFull(config.div_rs_size);
    } else if (isBranchOp(inst.op)) {
        can_issue = !units[3].rsFull(config.br_rs_size);
    } else if (isLogicOp(inst.op)) {
        can_issue = !units[4].rsFull(config.logic_rs_size);
    } else {
        can_issue = !units[0].rsFull(config.adder_rs_size);
    }

    if (!can_issue) {
        logEvent(std::string("Decode: stalled because target queue is full for ") + opcodeName(inst.op));
        return;
    }

    int tag = allocateROB(inst);
    if (tag == -1) {
        logEvent("Decode: failed to allocate ROB entry");
        return;
    }

    ROBEntry *rob = getROB(tag);
    rob->inst = inst;
    rob->dest = inst.dest;

    if (inst.op == OpCode::J) {
        // Unconditional jump: no execution unit; mark the ROB entry ready directly.
        rob->ready = true;
        rob->actual_target = inst.pc + inst.imm;
        rob->predicted_target = rob->actual_target;
        fetch_buffer_valid = false;
        logEvent(
            "Decode: issued pc=" + std::to_string(inst.pc) +
            " op=J to ROB " + std::to_string(tag)
        );
        return;
    }

    if (isBranchOp(inst.op)) {
        rob->predicted_target = fetch_buffer_pred_pc;

        RSEntry e;
        e.busy = true;
        e.op = inst.op;
        e.rob_tag = tag;
        e.pc = inst.pc;
        e.imm = inst.imm;
        e.insert_cycle = clock_cycle;
        e.age = ++units[3].age_counter;

        operandReady(inst.src1, e.Vj, e.Qj);
        operandReady(inst.src2, e.Vk, e.Qk);
        enqueueToUnit(e);
        fetch_buffer_valid = false;
        logEvent(
            "Decode: issued pc=" + std::to_string(inst.pc) +
            " op=" + opcodeName(inst.op) +
            " to ROB " + std::to_string(tag)
        );
        return;
    }

    if (inst.op == OpCode::LW) {
        LSQEntry e;
        e.busy = true;
        e.op = inst.op;
        e.rob_tag = tag;
        e.dest = inst.dest;
        e.pc = inst.pc;
        e.imm = inst.imm;
        e.insert_cycle = clock_cycle;
        e.age = ++lsq->age_counter;
        operandReady(inst.src1, e.Vj, e.Qj); // base register
        lsq->addEntry(e);

        if (inst.dest != 0) {
            rob->prev_rename = RAT[inst.dest];
            RAT[inst.dest] = tag;
        }
        fetch_buffer_valid = false;
        logEvent(
            "Decode: issued pc=" + std::to_string(inst.pc) +
            " op=LW to ROB " + std::to_string(tag)
        );
        return;
    }

    if (inst.op == OpCode::SW) {
        LSQEntry e;
        e.busy = true;
        e.op = inst.op;
        e.rob_tag = tag;
        e.pc = inst.pc;
        e.imm = inst.imm;
        e.insert_cycle = clock_cycle;
        e.age = ++lsq->age_counter;
        operandReady(inst.src1, e.Vj, e.Qj); // store data
        operandReady(inst.src2, e.Vk, e.Qk); // base address
        lsq->addEntry(e);
        fetch_buffer_valid = false;
        logEvent(
            "Decode: issued pc=" + std::to_string(inst.pc) +
            " op=SW to ROB " + std::to_string(tag)
        );
        return;
    }

    RSEntry e;
    e.busy = true;
    e.op = inst.op;
    e.rob_tag = tag;
    e.dest = inst.dest;
    e.pc = inst.pc;
    e.imm = inst.imm;
    e.insert_cycle = clock_cycle;
    e.age = 0;

    if (inst.op == OpCode::ADDI || inst.op == OpCode::SLTI ||
        inst.op == OpCode::ANDI || inst.op == OpCode::ORI || inst.op == OpCode::XORI) {
        operandReady(inst.src1, e.Vj, e.Qj);
        e.Qk = -1;
        e.Vk = 0;
    } else {
        operandReady(inst.src1, e.Vj, e.Qj);
        operandReady(inst.src2, e.Vk, e.Qk);
    }

    if (inst.dest != 0 && writesRegister(inst.op)) {
        rob->prev_rename = RAT[inst.dest];
        RAT[inst.dest] = tag;
    }

    enqueueToUnit(e);
    fetch_buffer_valid = false;
    logEvent(
        "Decode: issued pc=" + std::to_string(inst.pc) +
        " op=" + opcodeName(inst.op) +
        " to ROB " + std::to_string(tag)
    );
}

void Processor::broadcastOnCDB() {
    if (pending_cdb.empty()) {
        logEvent("Broadcast: no results this cycle");
    }

    for (const auto &ev : pending_cdb) {
        auto *re = getROB(ev.rob_tag);
        if (!re) continue; // flushed already

        if (ev.has_exception) {
            re->has_exception = true;
            re->ready = true;
            logEvent("Broadcast: ROB " + std::to_string(ev.rob_tag) + " raised an exception");
            continue;
        }

        int broadcast_value = ev.value;
        if (ev.has_value && re->inst.op == OpCode::LW) {
            broadcast_value = resolveLoadValueFromOlderStores(ev.rob_tag, ev.addr, ev.value);
        }

        if (ev.has_value) {
            re->value = broadcast_value;
            re->ready = true;
            logEvent(
                "Broadcast: ROB " + std::to_string(ev.rob_tag) +
                " value=" + std::to_string(broadcast_value)
            );
        }
        if (ev.has_store) {
            re->addr = ev.addr;
            re->store_data = ev.store_data;
            re->ready = true;
            logEvent(
                "Broadcast: ROB " + std::to_string(ev.rob_tag) +
                " store addr=" + std::to_string(ev.addr) +
                " data=" + std::to_string(ev.store_data)
            );
        }
        if (ev.has_branch) {
            re->branch_taken = ev.branch_taken;
            re->actual_target = ev.actual_target;
            re->ready = true;
            logEvent(
                "Broadcast: ROB " + std::to_string(ev.rob_tag) +
                " branch taken=" + std::to_string(ev.branch_taken ? 1 : 0) +
                " target=" + std::to_string(ev.actual_target)
            );
        }

        if (ev.has_value) {
            for (auto &u : units) u.capture(ev.rob_tag, broadcast_value);
            if (lsq) lsq->capture(ev.rob_tag, broadcast_value);
        }
    }
    pending_cdb.clear();
}

void Processor::stageExecuteAndBroadcast() {
    pending_cdb.clear();
    if (exception) {
        logEvent("Execute: skipped because exception is set");
        return;
    }

    for (auto &u : units) {
        auto out = u.executeCycle(clock_cycle);
        pending_cdb.insert(pending_cdb.end(), out.begin(), out.end());
    }
    if (lsq) {
        auto out = lsq->executeCycle(Memory, clock_cycle);
        pending_cdb.insert(pending_cdb.end(), out.begin(), out.end());
    }

    if (pending_cdb.empty()) {
        logEvent("Execute: no unit finished this cycle");
    } else {
        logEvent("Execute: " + std::to_string(pending_cdb.size()) + " result(s) ready for broadcast");
    }

    broadcastOnCDB();
}

void Processor::stageCommit() {
    squash_rest_of_cycle = false;

    if (exception) {
        logEvent("Commit: skipped because exception is already set");
        return;
    }

    if (rob_order.empty()) {
        logEvent("Commit: ROB empty");
        return;
    }

    while (!rob_order.empty()) {
        int tag = rob_order.front();
        auto it = rob_table.find(tag);
        if (it == rob_table.end()) {
            rob_order.pop_front();
            continue;
        }

        ROBEntry &e = it->second;
        if (!e.ready) {
            logEvent("Commit: waiting on ROB " + std::to_string(tag));
            break;
        }

        // Precise exception handling: the exception is only architecturally raised here.
        if (e.has_exception) {
            logEvent("Commit: exception from ROB " + std::to_string(tag) + ", flushing speculation");
            exception = true;
            pc = e.pc;
            flushAll();
            return;
        }

        if (e.inst.op == OpCode::LW) {
            if (e.dest != 0) {
                ARF[e.dest] = e.value;
                if (RAT[e.dest] == tag) {
                    RAT[e.dest] = -1;
                }
            }
            logEvent(
                "Commit: ROB " + std::to_string(tag) +
                " wrote x" + std::to_string(e.dest) +
                "=" + std::to_string(e.value)
            );
        } else if (e.inst.op == OpCode::SW) {
            if (e.addr < 0 || e.addr >= (int)Memory.size()) {
                logEvent("Commit: store address out of bounds at ROB " + std::to_string(tag));
                exception = true;
                pc = e.pc;
                flushAll();
                return;
            }
            Memory[e.addr] = e.store_data;
            logEvent(
                "Commit: ROB " + std::to_string(tag) +
                " stored M[" + std::to_string(e.addr) +
                "]=" + std::to_string(e.store_data)
            );
        } else if (isBranchOp(e.inst.op)) {
            bool was_correct = (e.actual_target == e.predicted_target);
            bp.update(e.pc, e.actual_target, e.branch_taken, was_correct);

            if (!was_correct) {
                // Roll back younger speculative renames in reverse program order.
                std::vector<int> younger_tags;
                for (auto it2 = std::next(rob_order.begin()); it2 != rob_order.end(); ++it2) {
                    younger_tags.push_back(*it2);
                }
                for (auto rit = younger_tags.rbegin(); rit != younger_tags.rend(); ++rit) {
                    auto jt = rob_table.find(*rit);
                    if (jt == rob_table.end()) continue;
                    const ROBEntry &y = jt->second;
                    if (y.dest != 0 && y.dest >= 0 && RAT[y.dest] == y.tag) {
                        RAT[y.dest] = y.prev_rename;
                    }
                }

                // Remove the branch itself and flush all younger speculation.
                rob_table.erase(it);
                rob_order.pop_front();
                flushSpeculationPreserveRAT();
                pc = e.actual_target;
                squash_rest_of_cycle = true;
                logEvent(
                    "Commit: branch mispredict at ROB " + std::to_string(tag) +
                    ", redirect pc=" + std::to_string(e.actual_target)
                );
                return;
            }
            logEvent("Commit: branch at ROB " + std::to_string(tag) + " was correct");
        } else if (e.inst.op == OpCode::J) {
            // No additional architectural action required.
            logEvent("Commit: jump at ROB " + std::to_string(tag));
        } else if (writesRegister(e.inst.op)) {
            if (e.dest != 0) {
                ARF[e.dest] = e.value;
                if (RAT[e.dest] == tag) {
                    RAT[e.dest] = -1;
                }
            }
            logEvent(
                "Commit: ROB " + std::to_string(tag) +
                " wrote x" + std::to_string(e.dest) +
                "=" + std::to_string(e.value)
            );
        }

        rob_table.erase(it);
        rob_order.pop_front();
    }
}

bool Processor::hasPendingWork() const {
    if (exception) return false;
    if (fetch_buffer_valid) return true;
    if (!rob_order.empty()) return true;
    if (pc >= 0 && pc < (int)inst_memory.size()) return true;
    for (const auto &u : units) {
        if (!u.rs.empty() || !u.pipeline.empty()) return true;
    }
    if (!lsq->entries.empty() || !lsq->pipeline.empty()) return true;
    return false;
}

int Processor::resolveLoadValueFromOlderStores(int load_tag, int addr, int default_value) const {
    int value = default_value;

    for (int tag : rob_order) {
        if (tag == load_tag) break;

        auto it = rob_table.find(tag);
        if (it == rob_table.end()) continue;

        const ROBEntry &entry = it->second;
        if (entry.inst.op != OpCode::SW) continue;
        if (!entry.ready || entry.has_exception) continue;
        if (entry.addr != addr) continue;

        value = entry.store_data;
    }

    return value;
}

void Processor::enforceX0Zero() {
    if (!ARF.empty()) {
        ARF[0] = 0;
    }
    if (!RAT.empty()) {
        RAT[0] = -1;
    }
}

bool Processor::step() {
    clock_cycle++;
    logEvent("");
    logEvent("===== Cycle " + std::to_string(clock_cycle) + " =====");

    stageCommit();
    if (squash_rest_of_cycle) {
        logEvent("Cycle control: stopping remaining stages after branch redirect");
        enforceX0Zero();
        squash_rest_of_cycle = false;
        return hasPendingWork();
    }
    stageExecuteAndBroadcast();
    stageDecode();
    stageFetch();
    enforceX0Zero();

    return hasPendingWork();
}

void Processor::dumpArchitecturalState() {
    std::cout << "\n=== ARCHITECTURAL STATE (CYCLE " << clock_cycle << ") ===\n";
    for (size_t i = 0; i < ARF.size(); i++) {
        std::cout << "x" << i << ": " << std::setw(4) << ARF[i] << " | ";
        if ((i + 1) % 8 == 0) std::cout << std::endl;
    }
    if (exception) {
        std::cout << "EXCEPTION raised by instruction " << pc + 1 << std::endl;
    }
    std::cout << "Branch Predictor Stats: " << bp.correct_predictions << "/" << bp.total_branches << " correct.\n";
}

void Processor::setupLogging() {
    namespace fs = std::filesystem;

    if (log_file.is_open()) {
        log_file.close();
    }

    if (!fs::exists("logs")) {
        fs::create_directory("logs");
    }

    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);

    std::stringstream ss;
    ss << "logs/run_" << std::put_time(std::localtime(&in_time_t), "%Y%m%d_%H%M%S") << ".log";
    std::string filename = ss.str();

    log_file.open(filename);
    if (log_file.is_open()) {
        log_file << "Simulation started in " << filename << "\n";
    }
}

void Processor::logEvent(const std::string &message) {
    if (!log_file.is_open()) return;
    log_file << message << "\n";
}
