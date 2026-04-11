#pragma once

#include <iostream>
#include <vector>
#include <deque>
#include <climits>
#include "Basics.h"

// Generic pipelined execution unit for ALU-style operations.
class ExecutionUnit {
public:
    UnitType name = UnitType::ADDER;
    int latency = 1;

    bool has_result = false;
    bool has_exception = false;

    std::vector<RSEntry> rs;
    int age_counter = 0;

    struct ActiveOp {
        RSEntry entry;
        int remaining = 0;
    };
    std::deque<ActiveOp> pipeline;

    ExecutionUnit() = default;
    ExecutionUnit(UnitType t, int lat, int rs_size);

    bool full() const;
    bool rsFull(int capacity) const;
    void flush();
    void addEntry(const RSEntry &e);
    void capture(int tag, int val);
    BroadcastEvent computeResult(const RSEntry &e);
    std::vector<BroadcastEvent> executeCycle(int current_cycle);
};
