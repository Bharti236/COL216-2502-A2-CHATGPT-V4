#include "LoadStoreQueue.h"

#include <algorithm>

LoadStoreQueue::LoadStoreQueue(int lat, int rs_size) : latency(lat) {
    entries.reserve(rs_size);
}

bool LoadStoreQueue::rsFull(int capacity) const {
    return (int)entries.size() >= capacity;
}

void LoadStoreQueue::flush() {
    entries.clear();
    pipeline.clear();
    has_result = false;
    has_exception = false;
    store_data = 0;
}

void LoadStoreQueue::addEntry(const LSQEntry &e) {
    entries.push_back(e);
}

void LoadStoreQueue::capture(int tag, int val) {
    for (auto &e : entries) {
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

BroadcastEvent LoadStoreQueue::computeResult(const LSQEntry &e, std::vector<int> &Memory) {
    BroadcastEvent ev;
    ev.rob_tag = e.rob_tag;

    long long addr = 0;
    if (e.op == OpCode::LW) {
        addr = (long long)e.Vj + (long long)e.imm;
        if (addr < 0 || addr >= (long long)Memory.size()) {
            ev.has_exception = true;
            return ev;
        }
        ev.addr = (int)addr;
        ev.has_value = true;
        ev.value = Memory[(size_t)addr];
    } else {
        // SW
        addr = (long long)e.Vk + (long long)e.imm;
        if (addr < 0 || addr >= (long long)Memory.size()) {
            ev.has_exception = true;
            return ev;
        }
        ev.has_store = true;
        ev.addr = (int)addr;
        ev.store_data = e.Vj;
    }
    return ev;
}

std::vector<BroadcastEvent> LoadStoreQueue::executeCycle(std::vector<int> &Memory, int current_cycle) {
    std::vector<BroadcastEvent> finished;
    has_result = false;
    has_exception = false;

    // Memory operations still enter in-order, but the LSQ slot is only
    // released once the memory operation has completed.
    int candidate_idx = -1;
    for (int i = 0; i < (int)entries.size(); ++i) {
        if (!entries[i].busy) continue;
        if (!entries[i].executing) {
            candidate_idx = i;
            break;
        }
    }

    if (candidate_idx != -1) {
        const auto &e = entries[candidate_idx];
        bool ready = false;
        if (e.op == OpCode::LW) {
            ready = (e.Qj == -1);
        } else {
            ready = (e.Qj == -1 && e.Qk == -1);
        }
        if (ready && e.insert_cycle < current_cycle) {
            ActiveOp op;
            op.entry = e;
            op.remaining = latency;
            pipeline.push_back(op);
            entries[candidate_idx].executing = true;
        }
    }

    // Advance active memory op.
    for (auto &op : pipeline) {
        op.remaining--;
    }

    while (!pipeline.empty() && pipeline.front().remaining <= 0) {
        auto op = pipeline.front();
        pipeline.pop_front();
        if (op.remaining <= 0) {
            BroadcastEvent ev = computeResult(op.entry, Memory);
            finished.push_back(ev);
            has_result = has_result || ev.has_value || ev.has_store;
            has_exception = has_exception || ev.has_exception;

            auto lsq_it = std::find_if(entries.begin(), entries.end(), [&](const LSQEntry &entry) {
                return entry.rob_tag == op.entry.rob_tag;
            });
            if (lsq_it != entries.end()) {
                entries.erase(lsq_it);
            }
        }
    }

    return finished;
}
