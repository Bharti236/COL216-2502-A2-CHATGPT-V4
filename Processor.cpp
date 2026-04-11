#include "Processor.h"
#include "AssemblyLoader.h"

#include <stdexcept>
#include <limits>

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
    units.emplace_back(UnitType::BRANCH, 1, config.br_rs_size);
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
    if (!RAT.empty()) RAT[0] = -1;
}

void Processor::flush() {
    // Public helper: clear speculative state while keeping the architectural state.
    flushSpeculationPreserveRAT();
}

void Processor::loadProgram(const std::string &filename) {
    // Reset machine state so loadProgram can be called more than once safely.
    pc = 0;
    clock_cycle = 0;
    exception = false;
    std::fill(ARF.begin(), ARF.end(), 0);
    std::fill(Memory.begin(), Memory.end(), 0);
    std::fill(RAT.begin(), RAT.end(), -1);
    if (!RAT.empty()) RAT[0] = -1;
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
    if (exception) return;
    if (fetch_buffer_valid) return;
    if (pc < 0 || pc >= (int)inst_memory.size()) return;

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
}

void Processor::stageDecode() {
    if (!fetch_buffer_valid || exception) return;
    const Instruction &inst = fetch_buffer_inst;

    if ((int)rob_order.size() >= config.rob_size) return; // stall

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

    if (!can_issue) return;

    int tag = allocateROB(inst);
    if (tag == -1) return;

    ROBEntry *rob = getROB(tag);
    rob->inst = inst;
    rob->dest = inst.dest;

    if (inst.op == OpCode::J) {
        // Unconditional jump: no execution unit; mark the ROB entry ready directly.
        rob->ready = true;
        rob->actual_target = inst.pc + inst.imm;
        rob->predicted_target = rob->actual_target;
        fetch_buffer_valid = false;
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

        if (inst.dest != 0) RAT[inst.dest] = tag;
        fetch_buffer_valid = false;
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
}

void Processor::broadcastOnCDB() {
    for (const auto &ev : pending_cdb) {
        auto *re = getROB(ev.rob_tag);
        if (!re) continue; // flushed already

        if (ev.has_exception) {
            re->has_exception = true;
            re->ready = true;
            continue;
        }

        if (ev.has_value) {
            re->value = ev.value;
            re->ready = true;
        }
        if (ev.has_store) {
            re->addr = ev.addr;
            re->store_data = ev.store_data;
            re->ready = true;
        }
        if (ev.has_branch) {
            re->branch_taken = ev.branch_taken;
            re->actual_target = ev.actual_target;
            re->ready = true;
        }

        if (ev.has_value) {
            for (auto &u : units) u.capture(ev.rob_tag, ev.value);
            if (lsq) lsq->capture(ev.rob_tag, ev.value);
        }
    }
    pending_cdb.clear();
}

void Processor::stageExecuteAndBroadcast() {
    pending_cdb.clear();
    if (exception) return;

    for (auto &u : units) {
        auto out = u.executeCycle(clock_cycle);
        pending_cdb.insert(pending_cdb.end(), out.begin(), out.end());
    }
    if (lsq) {
        auto out = lsq->executeCycle(Memory, clock_cycle);
        pending_cdb.insert(pending_cdb.end(), out.begin(), out.end());
    }

    broadcastOnCDB();
}

void Processor::stageCommit() {
    if (exception) return;

    while (!rob_order.empty()) {
        int tag = rob_order.front();
        auto it = rob_table.find(tag);
        if (it == rob_table.end()) {
            rob_order.pop_front();
            continue;
        }

        ROBEntry &e = it->second;
        if (!e.ready) break;

        // Precise exception handling: the exception is only architecturally raised here.
        if (e.has_exception) {
            exception = true;
            pc = e.pc;
            flushAll();
            return;
        }

        if (e.inst.op == OpCode::LW) {
            if (e.dest != 0 && RAT[e.dest] == tag) {
                ARF[e.dest] = e.value;
                RAT[e.dest] = -1;
            }
        } else if (e.inst.op == OpCode::SW) {
            if (e.addr < 0 || e.addr >= (int)Memory.size()) {
                exception = true;
                pc = e.pc;
                flushAll();
                return;
            }
            Memory[e.addr] = e.store_data;
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
                return;
            }
        } else if (e.inst.op == OpCode::J) {
            // No additional architectural action required.
        } else if (writesRegister(e.inst.op)) {
            if (e.dest != 0 && RAT[e.dest] == tag) {
                ARF[e.dest] = e.value;
                RAT[e.dest] = -1;
            }
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

bool Processor::step() {
    clock_cycle++;

    stageCommit();
    stageExecuteAndBroadcast();
    stageDecode();
    stageFetch();

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
