#include "BranchPredictor.h"

int BranchPredictor::predict(int current_pc, int imm, OpCode op) {
    if (op != OpCode::BEQ && op != OpCode::BNE && op != OpCode::BLT && op != OpCode::BLE) {
        return current_pc + 1;
    }

    int s = 0;
    auto it = state.find(current_pc);
    if (it != state.end()) s = it->second;

    bool predict_taken = (s <= 1);
    return predict_taken ? (current_pc + imm) : (current_pc + 1);
}

void BranchPredictor::update(int pc, int actual_target, bool taken, bool was_correct) {
    total_branches++;
    if (was_correct) {
        correct_predictions++;
    }

    int &s = state[pc]; // default initial state = 0
    if (taken) {
        if (s == 1) s = 0;
        else if (s == 2) s = 1;
        else if (s == 3) s = 2;
        // state 0 stays 0
    } else {
        if (s == 0) s = 1;
        else if (s == 1) s = 2;
        else if (s == 2) s = 3;
        // state 3 stays 3
    }
    (void)actual_target; // kept for interface compatibility / debugging.
}
