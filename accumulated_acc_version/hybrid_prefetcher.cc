#include "stdio.h"
#include "stdlib.h"
#include <algorithm>

#include "hybrid_prefetcher.h"
#include "prefetcher.h"
#include "simulator.h"
#include "config.hpp"

using namespace std;

HybridPrefetcher::HybridPrefetcher(String configName, core_id_t core_id) {
    isb_prefetcher = new IsbPrefetcher(configName, core_id);
    bo_prefetcher = new BOPrefetcher(configName, core_id);

    time_t t;
    srand((unsigned) time(&t));

#if NORMAL_MODE 
    tracked_pc_size = 0;
    tracked_addr_size = 0;
    tracked_addr_cursor = 0;
    memory_spare = 45000;

    tracked_pc = new uint32_t*[STORAGE_SIZE];
    for (int i = 0; i < STORAGE_SIZE; i++) {
        tracked_pc[i] = new uint32_t[8];
    }

    tracked_addr = new uint32_t*[STORAGE_SIZE*ADDR_TABLE_SIZE_RATIO];
    for (int i = 0; i < STORAGE_SIZE*ADDR_TABLE_SIZE_RATIO; i++) {
        tracked_addr[i] = new uint32_t[4];
    }

    #if IF_RECORD_ACC
    tracked_acc = new float*[STORAGE_SIZE];
    for (int i = 0; i < STORAGE_SIZE; i++) {
        tracked_acc[i] = new float[8];
        for (int j = 0; j < 8; j++) {
            tracked_acc[i][j] = -1.0;
        }
    }
    foutacc_bo.open("some_pc_acc_bo.csv");
    foutacc_isb.open("some_pc_acc_isb.csv");
    foutphase_bo.open("corres_phase_bo.csv");
    foutphase_isb.open("corres_phase_isb.csv");

    current_phase = 0;
    sample_interval = 10;
    sample_counter = 0;
    testing_pc = 4552376;
    #endif

    #if GLOBAL_DECISION
    global_bo_decision = SECOND;
    global_isb_decision = FIRST;
    #endif

#endif


#if HEADROOM_TEST
    strcpy(decision_file_name, "decision.csv");
    decision_list_size = 0;
    decision_list = new uint32_t*[STORAGE_SIZE];
    for (int i = 0; i < STORAGE_SIZE; i++) {
        decision_list[i] = new uint32_t[2];
    }

    char linebuf[256];
    char num_str[32];
    std::ifstream fin(decision_file_name);

    while (fin.getline(linebuf, sizeof(linebuf))) {
        char* comma = linebuf;
        char* space = NULL;
        comma = strchr(comma + 1, ':');
        space = strchr(comma + 1, ' ');
        strncpy(num_str, comma + 1, space - comma);
        num_str[space - comma] = '\0';
        decision_list[decision_list_size][0] = atoi(num_str);
        
        comma = strchr(comma + 1, ':');
        strncpy(num_str, comma + 1, 1);
        num_str[1] = '\0';
        decision_list[decision_list_size++][1] = atoi(num_str);
    }

    fin.close();
#endif

#if HEADROOM_TEST_MULTI
    current_phase = 0;
    strcpy(decision_file_name, "decision.csv");
    decision_list_size = 0;
    decision_list = new int*[STORAGE_SIZE*ADDR_TABLE_SIZE_RATIO];
    for (int i = 0; i < STORAGE_SIZE*ADDR_TABLE_SIZE_RATIO; i++) {
        decision_list[i] = new int[2];
    }

    char linebuf[256];
    char num_str[32];
    std::ifstream fin(decision_file_name);

    uint32_t num_phases = 0;
    while (fin.getline(linebuf, sizeof(linebuf))) {
        if (!strncmp(linebuf, "phase:", strlen("phase:"))) {
            decision_list[decision_list_size][0] = -1;
            decision_list[decision_list_size++][1] = num_phases;
            num_phases++;
            continue;
        }
        char* comma = linebuf;
        char* space = NULL;
        comma = strchr(comma + 1, ':');
        space = strchr(comma + 1, ' ');
        strncpy(num_str, comma + 1, space - comma);
        num_str[space - comma] = '\0';
        decision_list[decision_list_size][0] = atoi(num_str);
        
        comma = strchr(comma + 1, ':');
        strncpy(num_str, comma + 1, 1);
        num_str[1] = '\0';
        decision_list[decision_list_size++][1] = atoi(num_str);
    }

    fin.close();
#endif

}

HybridPrefetcher::~HybridPrefetcher() {
    delete isb_prefetcher;
    delete bo_prefetcher;

#if HEADROOM_TEST
    for (int i = 0; i < STORAGE_SIZE; i++) {
        delete []decision_list[i];
    }
    delete []decision_list;
#endif

#if HEADROOM_TEST_MULTI
    for (int i = 0; i < STORAGE_SIZE*ADDR_TABLE_SIZE_RATIO; i++) {
        delete []decision_list[i];
    }
    delete []decision_list;
#endif

#if NORMAL_MODE
    hybrid_info_print();

    #if IF_RECORD_ACC
    acc_info_print();
    for (int i = 0; i < STORAGE_SIZE; i++) {
        delete []tracked_acc[i];
    }
    delete []tracked_acc;
    foutacc_bo.close();
    foutacc_isb.close();
    foutphase_bo.close();
    foutphase_isb.close();
    #endif

    for (int i = 0; i < STORAGE_SIZE; i++) {
        delete []tracked_pc[i];
    }
    delete []tracked_pc;

    for (int i = 0; i < STORAGE_SIZE*ADDR_TABLE_SIZE_RATIO; i++) {
        delete []tracked_addr[i];
    }
    delete []tracked_addr;

#endif
}

std::vector<IntPtr> HybridPrefetcher::getNextAddress(IntPtr pc, IntPtr currentAddress, bool cache_hit, core_id_t core_id) {
    prefetchList.clear();

#if (HEADROOM_TEST || HEADROOM_TEST_MULTI || NAIVE_HYBRID)
    std::vector<IntPtr> isb_prefetchList = isb_prefetcher->getNextAddress(pc, currentAddress, cache_hit, core_id);
    std::vector<IntPtr> bo_prefetchList = bo_prefetcher->getNextAddress(pc, currentAddress, cache_hit, core_id);

    uint32_t decision = BOTH;

    #if HEADROOM_TEST 
    uint32_t i = 0;
    for (i = 0; i < decision_list_size; i++) {
        if (pc == decision_list[i][0]) {
            decision = decision_list[i][1]; 
            break;
        }
    }
    #endif

    #if HEADROOM_TEST_MULTI 
    int i = 0;
    int phase_start = -1;
    int phase_end = -1;
    for (i = 0; i < decision_list_size; i++) {
        if (decision_list[i][0] == -1 && decision_list[i][1] == current_phase) {
            phase_start = i;
        }
        if (decision_list[i][0] == -1 && decision_list[i][1] == current_phase + 1) {
            phase_end = i;
        }
        if (phase_start != -1 && phase_end != -1) {
            break;
        }
    }

    if (phase_end == -1) {
        phase_end = decision_list_size;
    }

    bool if_find = false;
    if (phase_start != -1) {
        for (i = phase_start + 1; i < phase_end; i++) {
            if ((int) pc == decision_list[i][0]) {
                decision = decision_list[i][1]; 
                if_find = true;
                break;
            }
        }

        if (!if_find) {
            for (i = phase_start - 1; i > 0; i--) {
                if ((int) pc == decision_list[i][0]) {
                    decision = decision_list[i][1]; 
                    if_find = true;
                    break;
                }
            }
        }

        if (!if_find) {
            for (i = phase_end + 1; i < decision_list_size; i++) {
                if ((int) pc == decision_list[i][0]) {
                    decision = decision_list[i][1]; 
                    if_find = true;
                    break;
                }
            }
        }
    }

    if (!if_find) {
        for (i = decision_list_size - 1; i > 0; i--) {
            if ((int) pc == decision_list[i][0]) {
                decision = decision_list[i][1]; 
                break;
            }
        }
    }
    #endif

    switch(decision) {
        case FIRST: prefetchList.insert(prefetchList.end(), isb_prefetchList.begin(), isb_prefetchList.end()); break;
        case SECOND: prefetchList.insert(prefetchList.end(), bo_prefetchList.begin(), bo_prefetchList.end()); break;
        case BOTH: {
                    prefetchList.insert(prefetchList.end(), isb_prefetchList.begin(), isb_prefetchList.end());
                    for (std::vector<IntPtr>::iterator it = bo_prefetchList.begin(); it != bo_prefetchList.end(); ++it) {
                        if (std::find(prefetchList.begin(), prefetchList.end(), *it) == prefetchList.end()) { 
                            prefetchList.push_back(*it);
                        }
                    }
                    break;
                }
        case NEITHER:
        default: prefetchList.clear();
    }
#endif


#if NORMAL_MODE 
    #if TRAIN_BOTH
    std::vector<IntPtr> isb_prefetchList = isb_prefetcher->getNextAddress(pc, currentAddress, cache_hit, core_id);
    std::vector<IntPtr> bo_prefetchList = bo_prefetcher->getNextAddress(pc, currentAddress, cache_hit, core_id);
    #endif

    uint32_t decision = BOTH;
    int pc_index = searchPCTable(pc);
    if (pc_index == -1) {
        pc_index = tracked_pc_size;
        initNewPC(pc);
    } else {
        decision = tracked_pc[pc_index][DECI];
    }

    // bool decision_changed = false;
    #if USE_BANDWIDTH
    if (memory_spare < LOW_SPARE && decision == BOTH) {
        decision = FIRST;
    } else if(memory_spare > HIGH_SPARE && decision > NEITHER) {
        decision = BOTH;
    } else if(memory_spare > HIGH_SPARE && decision == NEITHER) {
        decision = FIRST;
    }
    #endif

    #if GLOBAL_DECISION
    uint32_t global_bo_pre = 0;
    uint32_t global_isb_pre = 0;
    uint32_t global_bo_hit = 0;
    uint32_t global_isb_hit = 0;
    uint32_t i = 0; 

    if (tracked_pc[pc_index][BO_PRE] > BO_ACCESS_THRES) {
        for (i = 0; i < tracked_pc_size; ++i) {
            if (tracked_pc[i][BO_PRE] > BO_ACCESS_THRES) {
                global_bo_pre += tracked_pc[i][BO_PRE];
                global_bo_hit += tracked_pc[i][BO_HIT];
            }   
        }
        double global_bo_acc = (global_bo_pre)?((double) global_bo_hit/global_bo_pre):0;
        // printf("bo_acc = %f\n", global_bo_acc);
        global_bo_decision = (global_bo_acc > GLOBAL_BO_THRES)?SECOND:0;
    }
    
    if (tracked_pc[pc_index][ISB_PRE] > ISB_ACCESS_THRES) {
        for (i = 0; i < tracked_pc_size; ++i) {
            if (tracked_pc[i][ISB_PRE] > ISB_ACCESS_THRES) {
                global_isb_pre += tracked_pc[i][ISB_PRE];
                global_isb_hit += tracked_pc[i][ISB_HIT];
            }
        }
        double global_isb_acc = (global_isb_pre)?((double) global_isb_hit/global_isb_pre):0;
        // printf("isb_acc = %f\n", global_isb_acc);
        global_isb_decision = (global_isb_acc > GLOBAL_ISB_THRES)?FIRST:0;
    }

    uint32_t decision_bo = (tracked_pc[pc_index][BO_PRE] > BO_ACCESS_THRES)?global_bo_decision:SECOND;
    uint32_t decision_isb = (tracked_pc[pc_index][ISB_PRE] > ISB_ACCESS_THRES)?global_isb_decision:FIRST;
    decision = decision_bo + decision_isb;
    #endif

    switch(decision) {
        case FIRST: {
                        #if !TRAIN_BOTH
                        std::vector<IntPtr> isb_prefetchList = isb_prefetcher->getNextAddress(pc, currentAddress, cache_hit, core_id);
                        #endif
                        InsertAddrTable(pc_index, isb_prefetchList, bo_prefetchList, FIRST);
                        prefetchList.insert(prefetchList.end(), isb_prefetchList.begin(), isb_prefetchList.end());
                        break;
                    }
        case SECOND: {
                         #if !TRAIN_BOTH
                         std::vector<IntPtr> bo_prefetchList = bo_prefetcher->getNextAddress(pc, currentAddress, cache_hit, core_id);
                         #endif
                         InsertAddrTable(pc_index, isb_prefetchList, bo_prefetchList, SECOND);
                         prefetchList.insert(prefetchList.end(), bo_prefetchList.begin(), bo_prefetchList.end());
                         break;
                     }
        case BOTH: {
                    #if !TRAIN_BOTH
                    std::vector<IntPtr> isb_prefetchList = isb_prefetcher->getNextAddress(pc, currentAddress, cache_hit, core_id);
                    std::vector<IntPtr> bo_prefetchList = bo_prefetcher->getNextAddress(pc, currentAddress, cache_hit, core_id);
                    #endif
                    InsertAddrTable(pc_index, isb_prefetchList, bo_prefetchList, BOTH);
                    prefetchList.insert(prefetchList.end(), isb_prefetchList.begin(), isb_prefetchList.end());
                    for (std::vector<IntPtr>::iterator it = bo_prefetchList.begin(); it != bo_prefetchList.end(); ++it) {
                        if (std::find(prefetchList.begin(), prefetchList.end(), *it) == prefetchList.end()) { 
                            prefetchList.push_back(*it);
                        }
                    }
                    break;
                }
        case NEITHER:
        default: prefetchList.clear();
    }
#endif

    return prefetchList;
}


#if NORMAL_MODE 
int HybridPrefetcher::searchPCTable(uint32_t target_pc) {
    int index = -1;
    uint32_t i;

    for (i = 0; i < tracked_pc_size; ++i) {
        if (target_pc == tracked_pc[i][PC]) {
            index = i;
            break;
        }
    }

    return index;
}

int HybridPrefetcher::searchAddrTable(uint32_t target_addr) {
    int index = -1;
    uint32_t i;

    for (i = 0; i < tracked_addr_size; ++i) {
        if (target_addr == tracked_addr[i][ADDR]) {
            index = i;
            break;
        }
    }

    return index;
}

void HybridPrefetcher::initNewPC(uint32_t new_pc) {
    int i = 0;
    for (i = 0; i < 8; ++i) {
        tracked_pc[tracked_pc_size][i] = 0;
    }
    tracked_pc[tracked_pc_size][PC] = new_pc;
    tracked_pc[tracked_pc_size][DECI] = BOTH;
    tracked_pc_size++;

    return;
}

void HybridPrefetcher::initNewAddr(uint32_t new_addr, uint32_t type, uint32_t pc_index) {
    tracked_addr[tracked_addr_cursor][ADDR] = new_addr;
    tracked_addr[tracked_addr_cursor][TYPE] = type;
    tracked_addr[tracked_addr_cursor][PC_INDEX] = pc_index;
    tracked_addr[tracked_addr_cursor][DANGER] = 0;

    tracked_addr_cursor = (tracked_addr_cursor + 1)%(ADDR_TABLE_SIZE_RATIO*STORAGE_SIZE);
    tracked_addr_size += (tracked_addr_size != (ADDR_TABLE_SIZE_RATIO*STORAGE_SIZE))?1:0;

    return;
}

void HybridPrefetcher::InsertAddrTable(uint32_t pc_index, std::vector<IntPtr> list, uint32_t type) {
    int addr_index = -1;
    for (std::vector<IntPtr>::iterator it = list.begin(); it != list.end(); ++it) {
        addr_index = searchAddrTable(*it);
        if (addr_index == -1) {
            initNewAddr(*it, type, pc_index);
        } else {
            if (pc_index == tracked_addr[addr_index][PC_INDEX]) {
                tracked_addr[addr_index][DANGER] -= (tracked_addr[addr_index][DANGER]!=0)?1:0;
                if (tracked_addr[addr_index][TYPE] == BOTH && type != BOTH) {
                    tracked_addr[addr_index][TYPE] = type;
                } else if (tracked_addr[addr_index][TYPE] != type){
                    tracked_addr[addr_index][TYPE] = BOTH;
                }
            } else if (tracked_addr[addr_index][DANGER] < DANGER_THRES) {
#if DEBUG
                printf("Insert but different pc_index: pc=%d\n",tracked_pc[pc_index][PC]);
#endif
                tracked_addr[addr_index][DANGER]++;
            } else if (tracked_addr[addr_index][DANGER] == DANGER_THRES) {
                tracked_addr[addr_index][DANGER] = 0;
                tracked_addr[addr_index][PC_INDEX] = pc_index;
                tracked_addr[addr_index][TYPE] = type;
            }
        }
    }

    return;
}

void HybridPrefetcher::InsertAddrTable(uint32_t pc_index, std::vector<IntPtr> list0, std::vector<IntPtr> list1, uint32_t type) {
    std::vector<IntPtr> isb_list;
    std::vector<IntPtr> bo_list;
    std::vector<IntPtr> both_list;

    for (std::vector<IntPtr>::iterator it = list0.begin(); it != list0.end(); ++it){
        if (std::find(list1.begin(), list1.end(), *it) == list1.end()) { 
            isb_list.push_back(*it);
        } else {
            both_list.push_back(*it);
        }
    }
     
    for (std::vector<IntPtr>::iterator it = list1.begin(); it != list1.end(); ++it){
        if (std::find(list0.begin(), list0.end(), *it) == list0.end()) { 
            bo_list.push_back(*it);
        }
    }

    InsertAddrTable(pc_index, both_list, BOTH);
    if (type == FIRST || type == BOTH) {
        InsertAddrTable(pc_index, isb_list, FIRST);
    }

    if (type == SECOND || type == BOTH) {
        InsertAddrTable(pc_index, bo_list, SECOND);
    }

#if DEBUG
    int size0 = 2*both_list.size() + isb_list.size() + bo_list.size(); 
    int size1 = list0.size() + list1.size();
    if (size1 != size0) {
        printf("pc_index = %d, size0 = %d, size1 = %d\n", pc_index, size0, size1);
    }
#endif

    return;
}


void HybridPrefetcher::returnPrefetchIssuedInfo(IntPtr prefetch_pc, IntPtr current_address) {
    int addr_index = searchAddrTable(current_address);
    if (addr_index == -1) {
#if DEBUG
        printf("Prefetch: Address %ld got flushed\n", current_address);
#endif
        return;
    }
    uint32_t addr_prefetch_type = tracked_addr[addr_index][TYPE];
    uint32_t pc_index_cur_addr = tracked_addr[addr_index][PC_INDEX];
    if (prefetch_pc == tracked_pc[pc_index_cur_addr][PC]) {
        tracked_addr[addr_index][DANGER] -= (tracked_addr[addr_index][DANGER] != 0) ;
        //printf("Issued prefetcher recorded pc = %ld, addr_index = %d\n", prefetch_pc, addr_index);
        switch(addr_prefetch_type) {
            case FIRST: tracked_pc[pc_index_cur_addr][ISB_PRE] += 1; break;
            case SECOND: tracked_pc[pc_index_cur_addr][BO_PRE] += 1; break;
            case BOTH: {
                           tracked_pc[pc_index_cur_addr][ISB_PRE] += 1;
                           tracked_pc[pc_index_cur_addr][BO_PRE] += 1;
                           tracked_pc[pc_index_cur_addr][PRE] += 1;
                           break;
                       }
            default: printf("Shouldn't be\n");
        }
        updatePCDecision(pc_index_cur_addr);
    } else {
#if DEBUG
        printf("Issued but Different PC: Address %ld got flushed addr_table_size = %d, addr_index = %d\n",
                current_address, tracked_addr_size, addr_index);
#endif
        uint32_t pc_index = searchPCTable(prefetch_pc);
        if (tracked_addr[addr_index][DANGER] < DANGER_THRES) {
            ++ tracked_addr[addr_index][DANGER];
            tracked_pc[pc_index][ISB_PRE] += (addr_prefetch_type != SECOND);
            tracked_pc[pc_index][BO_PRE] += (addr_prefetch_type != FIRST );
            tracked_pc[pc_index][PRE] += (addr_prefetch_type == BOTH );
        } else {
            tracked_addr[addr_index][PC_INDEX] = pc_index;
            tracked_addr[addr_index][DANGER] = DANGER_THRES/2;
            // tracked_addr[addr_index][TYPE] = BOTH;
        }
    }

    return;
}

void HybridPrefetcher::returnPrefetchHitInfo(IntPtr prefetch_pc, IntPtr current_address) {
    int addr_index = searchAddrTable(current_address);
    if (addr_index == -1) {
#if DEBUG
        printf("Hit: Address %ld got flushed\n", current_address);
#endif
        return;
    }
    uint32_t addr_prefetch_type = tracked_addr[addr_index][TYPE];
    uint32_t pc_index_cur_addr = tracked_addr[addr_index][PC_INDEX];
    if (prefetch_pc == tracked_pc[pc_index_cur_addr][PC]) {
        tracked_addr[addr_index][DANGER] -= (tracked_addr[addr_index][DANGER] != 0) ;
        //printf("hit prefetcher recorded pc = %ld, addr_index = %d\n", prefetch_pc, addr_index);
        switch(addr_prefetch_type) {
            case FIRST: tracked_pc[pc_index_cur_addr][ISB_HIT] += 1; break;
            case SECOND: tracked_pc[pc_index_cur_addr][BO_HIT] += 1; break;
            case BOTH: {
                           tracked_pc[pc_index_cur_addr][ISB_HIT] += 1;
                           tracked_pc[pc_index_cur_addr][BO_HIT] += 1;
                           tracked_pc[pc_index_cur_addr][HIT] += 1;
                           break;
                       }
            default: printf("Shouldn't be\n");
        }
        updatePCDecision(pc_index_cur_addr);
    } else {
#if DEBUG
        printf("Hit but Different PC: Address %ld got flushed addr_table_size = %d, addr_index = %d\n",
                current_address, tracked_addr_size, addr_index);
#endif
        uint32_t pc_index = searchPCTable(prefetch_pc);
        if (tracked_addr[addr_index][DANGER] < DANGER_THRES) {
            ++ tracked_addr[addr_index][DANGER];
            tracked_pc[pc_index][ISB_HIT] += (addr_prefetch_type != SECOND);
            tracked_pc[pc_index][BO_HIT] += (addr_prefetch_type != FIRST );
            tracked_pc[pc_index][HIT] += (addr_prefetch_type == BOTH );
        } else {
            tracked_addr[addr_index][PC_INDEX] = pc_index;
            tracked_addr[addr_index][DANGER] = DANGER_THRES/2;
            // tracked_addr[addr_index][TYPE] = BOTH;
        }
        // updatePCDecision(pc_index);
    }


    return;
}
#endif


#if NORMAL_MODE
void HybridPrefetcher::updatePCDecision(uint32_t pc_index) {
    if (tracked_pc[pc_index][ISB_PRE] <= ISB_ACCESS_THRES && tracked_pc[pc_index][BO_PRE] <= BO_ACCESS_THRES) {
        return;
    } else if (tracked_pc[pc_index][ISB_PRE] <= ISB_ACCESS_THRES && tracked_pc[pc_index][BO_PRE] > BO_ACCESS_THRES) {
        uint32_t bo_hit = tracked_pc[pc_index][BO_HIT];
        uint32_t bo_pre = tracked_pc[pc_index][BO_PRE];
        double accu_bo = (double)bo_hit/bo_pre;

        if (accu_bo < BO_LOW_THRES) {
            tracked_pc[pc_index][DECI] = FIRST;
        } else {
            tracked_pc[pc_index][DECI] = BOTH;
        }

        #if IF_RECORD_ACC
            tracked_acc[pc_index][BO_END_ACC] = accu_bo;
            if (tracked_acc[pc_index][BO_START_ACC] != -1) {
                if (accu_bo > tracked_acc[pc_index][BO_HI_ACC]) {
                    tracked_acc[pc_index][BO_HI_ACC] = accu_bo;
                }
                if (accu_bo < tracked_acc[pc_index][BO_LOW_ACC]) {
                    tracked_acc[pc_index][BO_LOW_ACC] = accu_bo;
                }

            } else {
                tracked_acc[pc_index][BO_START_ACC] = accu_bo;
                tracked_acc[pc_index][BO_HI_ACC] = accu_bo;
                tracked_acc[pc_index][BO_LOW_ACC] = accu_bo;
            }

            if (tracked_pc[pc_index][PC] == testing_pc && !((sample_counter++)%sample_interval)) {
                foutacc_bo << accu_bo << "\n";
                foutphase_bo << current_phase << "\n";
            }
        #endif

        return;
    } else if (tracked_pc[pc_index][BO_PRE] <= BO_ACCESS_THRES && tracked_pc[pc_index][ISB_PRE] > ISB_ACCESS_THRES) {
        uint32_t isb_hit = tracked_pc[pc_index][ISB_HIT];
        uint32_t isb_pre = tracked_pc[pc_index][ISB_PRE];
        double accu_isb = (double)isb_hit/isb_pre;

        if (accu_isb < ISB_LOW_THRES) {
            tracked_pc[pc_index][DECI] = SECOND;
        } else {
            tracked_pc[pc_index][DECI] = BOTH;
        }

        #if IF_RECORD_ACC
            tracked_acc[pc_index][ISB_END_ACC] = accu_isb;
            if (tracked_acc[pc_index][ISB_START_ACC] != -1) {
                if (accu_isb > tracked_acc[pc_index][ISB_HI_ACC]) {
                    tracked_acc[pc_index][ISB_HI_ACC] = accu_isb;
                }
                if (accu_isb < tracked_acc[pc_index][ISB_LOW_ACC]) {
                    tracked_acc[pc_index][ISB_LOW_ACC] = accu_isb;
                }

            } else {
                tracked_acc[pc_index][ISB_START_ACC] = accu_isb;
                tracked_acc[pc_index][ISB_HI_ACC] = accu_isb;
                tracked_acc[pc_index][ISB_LOW_ACC] = accu_isb;
            }
            if (tracked_pc[pc_index][PC] == testing_pc && !((sample_counter++)%sample_interval)) {
                foutacc_isb << accu_isb << "\n";
                foutphase_isb << current_phase << "\n";
            }
        #endif

        return;
    }

    // apply strat
    int shared_hit = tracked_pc[pc_index][HIT];
    int shared_pre = tracked_pc[pc_index][PRE];
    int isb_hit = tracked_pc[pc_index][ISB_HIT];
    int isb_pre = tracked_pc[pc_index][ISB_PRE];
    int bo_hit = tracked_pc[pc_index][BO_HIT];
    int bo_pre = tracked_pc[pc_index][BO_PRE];
    int hy_hit = isb_hit + bo_hit - shared_hit;
    int hy_pre = isb_pre + bo_pre - shared_pre;

    double accu_isb = (isb_pre)?((double)isb_hit/isb_pre):0;
    double accu_bo = (bo_pre)?((double)bo_hit/bo_pre):0;
    double accu_hy = (hy_pre)?((double)hy_hit/hy_pre):0;


    #if IF_RECORD_ACC
            tracked_acc[pc_index][BO_END_ACC] = accu_bo;
            if (tracked_acc[pc_index][BO_START_ACC] != -1) {
                if (accu_bo > tracked_acc[pc_index][BO_HI_ACC]) {
                    tracked_acc[pc_index][BO_HI_ACC] = accu_bo;
                }
                if (accu_bo < tracked_acc[pc_index][BO_LOW_ACC]) {
                    tracked_acc[pc_index][BO_LOW_ACC] = accu_bo;
                }

            } else {
                tracked_acc[pc_index][BO_START_ACC] = accu_bo;
                tracked_acc[pc_index][BO_HI_ACC] = accu_bo;
                tracked_acc[pc_index][BO_LOW_ACC] = accu_bo;
            }

            tracked_acc[pc_index][ISB_END_ACC] = accu_isb;
            if (tracked_acc[pc_index][ISB_START_ACC] != -1) {
                if (accu_isb > tracked_acc[pc_index][ISB_HI_ACC]) {
                    tracked_acc[pc_index][ISB_HI_ACC] = accu_isb;
                }
                if (accu_isb < tracked_acc[pc_index][ISB_LOW_ACC]) {
                    tracked_acc[pc_index][ISB_LOW_ACC] = accu_isb;
                }

            } else {
                tracked_acc[pc_index][ISB_START_ACC] = accu_isb;
                tracked_acc[pc_index][ISB_HI_ACC] = accu_isb;
                tracked_acc[pc_index][ISB_LOW_ACC] = accu_isb;
            }

            if (tracked_pc[pc_index][PC] == testing_pc && !((sample_counter++)%sample_interval)) {
                foutacc_bo << accu_bo << "\n";
                foutacc_isb << accu_isb << "\n";
                foutphase_bo << current_phase << "\n";
                foutphase_isb << current_phase << "\n";
            }
    #endif


    uint32_t choice = BOTH;

    #if SEPARATE_ACC
    uint32_t isb_choice = 0;
    if (accu_isb > ISB_THRES) {
        isb_choice = FIRST;
    } else {
        isb_choice = 0;
    }

    uint32_t bo_choice = false;
    if (accu_bo > BO_THRES) {
        bo_choice = SECOND;
    } else {
        bo_choice = 0;
    }
    isb_choice = 0;
    #endif

    #if STRAT_SHARED
    const double misprefetch_punish = (1.0 - accu_hy); /*headroom5*/
    double score_isb = isb_hit - (isb_pre - isb_hit)*misprefetch_punish;
    double score_bo = bo_hit - (bo_pre - bo_hit)*misprefetch_punish;
    
    if (accu_hy > BOTH_THRES) {
        choice = BOTH; 
    } else if (accu_hy < NEITHER_THRES) {
        // another option is to use score strats
        if (accu_isb >= accu_bo && accu_isb > ACCURACY_THRES) {
            choice = FIRST; 
        } else if (accu_bo >= accu_isb && accu_bo > ACCURACY_THRES) {
            choice = SECOND; 
        } else {
            choice = NEITHER; 
        }
    } else {
        if (accu_bo < ACCURACY_THRES && accu_isb > ACCURACY_THRES) {
            choice = FIRST; 
        } else if (accu_bo > ACCURACY_THRES && accu_isb < ACCURACY_THRES) {
            choice = SECOND; 
        } else {
            double shared_score = shared_hit - accu_hy*shared_pre;
            if (shared_score <= SHARED_SCORE_THRES) {
                choice = BOTH; 
            } else {
                if (score_isb >= score_bo) {
                    choice = FIRST; 
                } else {
                    choice = SECOND; 
                }
            }
        }
    }
    #endif

    /* testing some pc
    if (tracked_pc[pc_index][PC] == 4206346) {
        printf("pc = %d, choice = %d\n", tracked_pc[pc_index][PC], choice);
        printf("isb_hit = %d, isb_pre = %d, accu_isb = %f\n", isb_hit, isb_pre, accu_isb);
        printf("bo_hit = %d, bo_pre = %d, accu_bo = %f\n", bo_hit, bo_pre, accu_bo);
        printf("hy_hit = %d, hy_pre = %d, accu_hy = %f\n", hy_hit, hy_pre, accu_hy);
        printf("shared_hit = %d, shared_pre = %d\n", shared_hit, shared_pre);
        printf("score_isb = %f, scorebo = %f\n", score_isb, score_bo);
        printf("memory_spare = %f\n", memory_spare);
    }
    */
    
    tracked_pc[pc_index][DECI] = choice;
#if DEBUG
    printf("pc = %d, choice = %d\n", tracked_pc[pc_index][PC], choice);
#endif

    return;
}
#endif


#if IF_RECORD_ACC
void HybridPrefetcher::acc_info_print() {
    ofstream fout0("bo_acc.info");
    ofstream fout1("isb_acc.info");
    int i;

    for (i = 0; i < STORAGE_SIZE; i++) {
        if (tracked_acc[i][BO_START_ACC] == -1 || tracked_acc[i][BO_HI_ACC] < BO_THRES) {
            continue;
        }

        float* p = tracked_acc[i];
        fout0 << tracked_pc[i][PC] << " start_acc:" << p[BO_START_ACC]
                                  << " end_acc:" << p[BO_END_ACC]
                                  << " HI_acc:" << p[BO_HI_ACC]
                                  << " LO_acc:" << p[BO_LOW_ACC]
                                  << " diff:" << (p[BO_HI_ACC] - p[BO_LOW_ACC])
                                  << " #pre:" << tracked_pc[i][BO_PRE]
                                  << "\n";
    }

    for (i = 0; i < STORAGE_SIZE; i++) {
        if (tracked_acc[i][ISB_START_ACC] == -1 || tracked_acc[i][ISB_HI_ACC] < ISB_THRES) {
            continue;
        }

        float* p = tracked_acc[i];
        fout1 << tracked_pc[i][PC] << " start_acc:" << p[ISB_START_ACC]
                                  << " end_acc:" << p[ISB_END_ACC]
                                  << " HI_acc:" << p[ISB_HI_ACC]
                                  << " LO_acc:" << p[ISB_LOW_ACC]
                                  << " diff:" << (p[ISB_HI_ACC] - p[ISB_LOW_ACC])
                                  << " #pre:" << tracked_pc[i][ISB_PRE]
                                  << "\n";
    }

    fout0.close();
    fout1.close();
}
#endif
