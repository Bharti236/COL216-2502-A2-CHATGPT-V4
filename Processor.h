#pragma once

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <iomanip>
#include <unordered_map>
#include <deque>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include "Basics.h"
#include "BranchPredictor.h"
#include "ExecutionUnit.h"
#include "LoadStoreQueue.h"

class Processor {
public:
    int pc;
    int clock_cycle;

    std::vector<Instruction> inst_memory;

    // Architectural state (do not change these names).
    std::vector<int> ARF;
    std::vector<int> Memory;
    bool exception = false;

    // Speculative structures.
    std::vector<int> RAT;                // reg -> ROB tag, -1 means no rename
    std::deque<int> rob_order;           // ROB tags in program order
    std::unordered_map<int, ROBEntry> rob_table;
    int next_rob_tag = 0;

    // One fetched instruction waiting to be decoded.
    bool fetch_buffer_valid = false;
    Instruction fetch_buffer_inst;
    int fetch_buffer_pred_pc = -1;

    std::vector<ExecutionUnit> units;
    LoadStoreQueue *lsq;
    BranchPredictor bp;

    std::ofstream log_file;

    ProcessorConfig config;

    Processor(ProcessorConfig &cfg);

    void loadProgram(const std::string &filename);

    void flush();
    void broadcastOnCDB();
    void stageFetch();
    void stageDecode();
    void stageExecuteAndBroadcast();
    void stageCommit();
    bool step();
    void dumpArchitecturalState();
    void setupLogging();

private:
    std::vector<BroadcastEvent> pending_cdb;
    static bool isBranchOp(OpCode op);
    static bool writesRegister(OpCode op);
    static bool isLogicOp(OpCode op);

    int allocateROB(const Instruction &inst);
    ROBEntry *getROB(int tag);
    bool robEmpty() const;
    void clearFrontEnd();
    void flushSpeculationPreserveRAT();
    void flushAll();
    void enqueueToUnit(const RSEntry &e);
    bool operandReady(int reg, int &val, int &tag);
    bool hasPendingWork() const;
    int resolveLoadValueFromOlderStores(int load_tag, int addr, int default_value) const;
    void enforceX0Zero();
    void logEvent(const std::string &message);
};
