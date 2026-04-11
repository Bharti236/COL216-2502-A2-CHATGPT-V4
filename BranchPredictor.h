#pragma once

#include "Basics.h"
#include <iostream>
#include <unordered_map>

// Per-instruction 2-bit saturating predictor.
class BranchPredictor {
public:
    int total_branches = 0;
    int correct_predictions = 0;

    // 0,1 => predict taken. 2,3 => predict not taken.
    std::unordered_map<int, int> state; // PC -> predictor state

    int predict(int current_pc, int imm, OpCode op);
    void update(int pc, int actual_target, bool taken, bool was_correct);
};
