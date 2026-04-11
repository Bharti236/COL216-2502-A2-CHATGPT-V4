#pragma once

#include <iostream>
#include <vector>
#include <deque>
#include <climits>
#include "Basics.h"

// The LSQ executes strictly in program order.
class LoadStoreQueue {
public:
    int latency = 1;

    bool has_result = false;
    bool has_exception = false;
    int store_data = 0;

    std::vector<LSQEntry> entries;
    int age_counter = 0;

    struct ActiveOp {
        LSQEntry entry;
        int remaining = 0;
    };
    std::deque<ActiveOp> pipeline;

    LoadStoreQueue() = default;
    LoadStoreQueue(int lat, int rs_size);

    bool rsFull(int capacity) const;
    void flush();
    void addEntry(const LSQEntry &e);
    void capture(int tag, int val);
    BroadcastEvent computeResult(const LSQEntry &e, std::vector<int> &Memory);
    std::vector<BroadcastEvent> executeCycle(std::vector<int> &Memory, int current_cycle);
};
