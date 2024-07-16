#pragma once
#include "core/mmu.h"
#include "core/core.h"



class GB {
public:
    void runEmu(char* filename);
    void doctor_log(u32 frame, std::ofstream& log, Core& core, MMU& mem);
    GB() = default;
    ~GB() = default;

};

