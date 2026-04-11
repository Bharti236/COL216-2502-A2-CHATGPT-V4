#include "ExecutionUnit.h"

ExecutionUnit::ExecutionUnit(UnitType t, int lat, int rs_size) : name(t), latency(lat) {
    rs.reserve(rs_size);
}

bool ExecutionUnit::full() const {
    // The unit is pipelined; only the reservation station capacity limits issue.
    return false;
}

bool ExecutionUnit::rsFull(int capacity) const {
    return (int)rs.size() >= capacity;
}

void ExecutionUnit::flush() {
    rs.clear();
    pipeline.clear();
    has_result = false;
    has_exception = false;
}

void ExecutionUnit::addEntry(const RSEntry &e) {
    rs.push_back(e);
}

void ExecutionUnit::capture(int tag, int val) {
    for (auto &e : rs) {
        if (e.busy && e.Qj == tag) {
            e.Qj = -1;
            e.Vj = val;
        }
        if (e.busy && e.Qk == tag) {
            e.Qk = -1;
            e.Vk = val;
        }
    }
}

static int32_t checked32(long long x, bool &overflow) {
    if (x > INT32_MAX || x < INT32_MIN) {
        overflow = true;
        return 0;
    }
    return static_cast<int32_t>(x);
}

BroadcastEvent ExecutionUnit::computeResult(const RSEntry &e) {
    BroadcastEvent ev;
    ev.rob_tag = e.rob_tag;

    bool overflow = false;
    long long a = e.Vj;
    long long b = e.Vk;
    long long res = 0;

    switch (e.op) {
        case OpCode::ADD:  res = a + b; break;
        case OpCode::SUB:  res = a - b; break;
        case OpCode::ADDI: res = a + e.imm; break;
        case OpCode::MUL:  res = a * b; break;
        case OpCode::DIV:
            if (b == 0) { ev.has_exception = true; return ev; }
            res = a / b;
            break;
        case OpCode::REM:
            if (b == 0) { ev.has_exception = true; return ev; }
            res = a % b;
            break;
        case OpCode::SLT:  res = (a < b) ? 1 : 0; break;
        case OpCode::SLTI: res = (a < e.imm) ? 1 : 0; break;
        case OpCode::AND:  res = (static_cast<int32_t>(a) & static_cast<int32_t>(b)); break;
        case OpCode::OR:   res = (static_cast<int32_t>(a) | static_cast<int32_t>(b)); break;
        case OpCode::XOR:  res = (static_cast<int32_t>(a) ^ static_cast<int32_t>(b)); break;
        case OpCode::ANDI: res = (static_cast<int32_t>(a) & static_cast<int32_t>(e.imm)); break;
        case OpCode::ORI:  res = (static_cast<int32_t>(a) | static_cast<int32_t>(e.imm)); break;
        case OpCode::XORI: res = (static_cast<int32_t>(a) ^ static_cast<int32_t>(e.imm)); break;
        case OpCode::BEQ:
            ev.has_branch = true;
            ev.branch_taken = (a == b);
            ev.actual_target = ev.branch_taken ? (e.pc + e.imm) : (e.pc + 1);
            return ev;
        case OpCode::BNE:
            ev.has_branch = true;
            ev.branch_taken = (a != b);
            ev.actual_target = ev.branch_taken ? (e.pc + e.imm) : (e.pc + 1);
            return ev;
        case OpCode::BLT:
            ev.has_branch = true;
            ev.branch_taken = (a < b);
            ev.actual_target = ev.branch_taken ? (e.pc + e.imm) : (e.pc + 1);
            return ev;
        case OpCode::BLE:
            ev.has_branch = true;
            ev.branch_taken = (a <= b);
            ev.actual_target = ev.branch_taken ? (e.pc + e.imm) : (e.pc + 1);
            return ev;
        default:
            break;
    }

    if (e.op == OpCode::ADD || e.op == OpCode::SUB || e.op == OpCode::ADDI ||
        e.op == OpCode::MUL || e.op == OpCode::DIV || e.op == OpCode::REM) {
        checked32(res, overflow);
    }

    if (overflow) {
        ev.has_exception = true;
        return ev;
    }

    ev.has_value = true;
    ev.value = static_cast<int32_t>(res);
    return ev;
}

std::vector<BroadcastEvent> ExecutionUnit::executeCycle(int current_cycle) {
    std::vector<BroadcastEvent> finished;
    has_result = false;
    has_exception = false;

    // Issue at most one oldest-ready entry per cycle.
    int best_idx = -1;
    int best_age = INT_MAX;
    for (int i = 0; i < (int)rs.size(); ++i) {
        const auto &e = rs[i];
        if (!e.busy) continue;
        if (e.Qj != -1 || e.Qk != -1) continue;
        if (e.insert_cycle >= current_cycle) continue; // do not execute in same cycle as decode
        if (e.age < best_age) {
            best_age = e.age;
            best_idx = i;
        }
    }

    if (best_idx != -1) {
        ActiveOp op;
        op.entry = rs[best_idx];
        op.remaining = latency;
        pipeline.push_back(op);
        rs.erase(rs.begin() + best_idx);
    }

    // Advance pipeline.
    for (auto &op : pipeline) {
        op.remaining--;
    }

    // Collect completed ops.
    std::deque<ActiveOp> still_active;
    while (!pipeline.empty()) {
        auto op = pipeline.front();
        pipeline.pop_front();
        if (op.remaining <= 0) {
            BroadcastEvent ev = computeResult(op.entry);
            finished.push_back(ev);
            has_result = has_result || ev.has_value || ev.has_branch;
            has_exception = has_exception || ev.has_exception;
        } else {
            still_active.push_back(op);
        }
    }
    pipeline.swap(still_active);
    return finished;
}
