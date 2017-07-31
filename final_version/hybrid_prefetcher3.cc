#include "stdio.h"
#include "stdlib.h"
#include <algorithm>

#include "hybrid_prefetcher3.h"
#include "prefetcher.h"
#include "simulator.h"
#include "config.hpp"

using namespace std;

HybridPrefetcher3::HybridPrefetcher3(String configName, core_id_t core_id) {
    isb_prefetcher = new IsbPrefetcher(configName, core_id);
    bo_prefetcher = new BOPrefetcher(configName, core_id);


    tracked_pc_size = 0;
    tracked_pc_list = new uint64_t[STORAGE_SIZE];
    tracked_pc = new uint32_t*[STORAGE_SIZE];
    for (int i = 0; i < STORAGE_SIZE; i++) {
        tracked_pc[i] = new uint32_t[TRACKED_PC_LINE_LENGTH];
    }

    tracked_addr_size = 0;
    tracked_addr_cursor = 0;
    tracked_addr_list = new uint64_t[STORAGE_SIZE*ADDR_TABLE_SIZE_RATIO];
    tracked_addr = new uint32_t*[STORAGE_SIZE*ADDR_TABLE_SIZE_RATIO];
    for (int i = 0; i < STORAGE_SIZE*ADDR_TABLE_SIZE_RATIO; i++) {
        tracked_addr[i] = new uint32_t[TRACKED_ADDR_LINE_LENGTH];
        for (int j = 0; j < TRACKED_ADDR_LINE_LENGTH; j++) {
            tracked_addr[i][j] = 0;
        }
    }

    #if USE_SHADOW_CACHE
    shadow_cache_size = 0;
    shadow_cache_cursor = 0;
    shadow_cache_list = new uint64_t[STORAGE_SIZE*SHADOW_CACHE_SIZE_RATIO];
    shadow_cache = new uint32_t*[STORAGE_SIZE*SHADOW_CACHE_SIZE_RATIO];
    for (int i = 0; i < STORAGE_SIZE*SHADOW_CACHE_SIZE_RATIO; i++) {
        shadow_cache[i] = new uint32_t[SHADOW_CACHE_LINE_LENGTH];
        for (int j = 0; j < SHADOW_CACHE_LINE_LENGTH; j++) {
            shadow_cache[i][j] = 0;
        }
    }
    #endif
    got_flushed = 0;

    printf("HybridPrefetcher3 running\n");
}

HybridPrefetcher3::~HybridPrefetcher3() {
    delete isb_prefetcher;
    delete bo_prefetcher;

    hybrid_info_print();

    for (int i = 0; i < STORAGE_SIZE; i++) {
        delete []tracked_pc[i];
    }
    delete []tracked_pc;
    delete []tracked_pc_list;

    for (int i = 0; i < STORAGE_SIZE*ADDR_TABLE_SIZE_RATIO; i++) {
        delete []tracked_addr[i];
    }
    delete []tracked_addr;
    delete []tracked_addr_list;

    #if USE_SHADOW_CACHE
    for (int i = 0; i < STORAGE_SIZE*SHADOW_CACHE_SIZE_RATIO; i++) {
        delete []shadow_cache[i];
    }
    delete []shadow_cache;
    delete []shadow_cache_list;
    #endif
}

std::vector<IntPtr> HybridPrefetcher3::getNextAddress(IntPtr pc, IntPtr currentAddress, bool cache_hit, core_id_t core_id) {
    prefetchList.clear();

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
        case NEITHER: {
                          #if USE_SHADOW_CACHE
                          InsertAddrTable(pc_index, isb_prefetchList, bo_prefetchList, NEITHER);
                          #endif
                      }
        default: prefetchList.clear();
    }

    return prefetchList;
}


int HybridPrefetcher3::searchPCTable(uint64_t target_pc) {
    int index = -1;
    uint32_t i;

    for (i = 0; i < tracked_pc_size; ++i) {
        if (target_pc == tracked_pc_list[i]) {
            index = i;
            break;
        }
    }

    return index;
}

int HybridPrefetcher3::searchAddrTable(uint64_t target_addr) {
    int index = -1;
    uint32_t i;

    for (i = 0; i < tracked_addr_size; ++i) {
        if (target_addr == tracked_addr_list[i]) {
            index = i;
            break;
        }
    }

    return index;
}

void HybridPrefetcher3::initNewPC(uint64_t new_pc) {
    int i = 0;
    for (i = 0; i < TRACKED_PC_LINE_LENGTH; i++) {
        tracked_pc[tracked_pc_size][i] = 0;
    }

    tracked_pc_list[tracked_pc_size] = new_pc;
    tracked_pc[tracked_pc_size][DECI] = BOTH;
    tracked_pc_size++;

    return;
}

void HybridPrefetcher3::initNewAddr(uint64_t new_addr, uint32_t type, uint32_t pc_index) {
    tracked_addr_list[tracked_addr_cursor] = new_addr;
    tracked_addr[tracked_addr_cursor][TYPE] = type;
    tracked_addr[tracked_addr_cursor][PC_INDEX] = pc_index;
    tracked_addr[tracked_addr_cursor][STATUS] = INSERTED;

    tracked_addr_size += (tracked_addr_size != (ADDR_TABLE_SIZE_RATIO*STORAGE_SIZE))?1:0;
    tracked_addr_cursor = (tracked_addr_cursor + 1)%(ADDR_TABLE_SIZE_RATIO*STORAGE_SIZE);
   
    return;
}

void HybridPrefetcher3::InsertAddrTable(uint32_t pc_index, std::vector<IntPtr> list, uint32_t type) {
    int addr_index = -1;
    for (std::vector<IntPtr>::iterator it = list.begin(); it != list.end(); ++it) {
        addr_index = searchAddrTable(*it);

        if (addr_index == -1) {
            initNewAddr(*it, type, pc_index);
        } else {
            if (pc_index == tracked_addr[addr_index][PC_INDEX]) {
                switch(tracked_addr[addr_index][STATUS]) {
                    case INSERTED: tracked_addr[addr_index][TYPE] = (tracked_addr[addr_index][TYPE] == type)?type:BOTH; break;
                    case ISSUED: {
                                    if (tracked_addr[addr_index][TYPE] != type) {
                                        tracked_pc[pc_index][ISB_PRE] += (tracked_addr[addr_index][TYPE] == SECOND && type != SECOND);
                                        tracked_pc[pc_index][BO_PRE] += (tracked_addr[addr_index][TYPE] == FIRST && type != FIRST);
                                        tracked_addr[addr_index][TYPE] = BOTH;
                                        updatePCDecision(pc_index);
                                    }
                                    break;
                                 }
                    case HIT: {
                                tracked_addr[addr_index][TYPE] = type;
                                tracked_addr[addr_index][STATUS] = INSERTED;
                                break;
                              }
                    default: printf("shouldn't be EMPTY\n");
                }
                
            } else {
                switch(tracked_addr[addr_index][STATUS]) {
                    case ISSUED: {
                                    #if IF_CONFLICT_ISSUE_TABLE
                                    int conflict_index = searchConflictIssueTable(*it, pc_index);
                                    if (conflict_index != -1) {
                                        conflict_issue_table[conflict_index][TYPE] = type;
                                    } else {
                                        initNewConflictIssue(*it, type, pc_index);                               
                                    }
                                    #endif
                                    break;
                                 }
                    case HIT:
                    case INSERTED: {
                                       tracked_addr[addr_index][PC_INDEX] = pc_index;
                                       tracked_addr[addr_index][TYPE] = type;
                                       tracked_addr[addr_index][STATUS] = INSERTED;
                                       break;
                                   }
                    
                    default: printf("shouldn't be EMPTY\n");
                }
            }
        }
    }

    return;
}

void HybridPrefetcher3::InsertAddrTable(uint32_t pc_index, std::vector<IntPtr> list0, std::vector<IntPtr> list1, uint32_t type) {
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

    if (type == FIRST || type == BOTH) {
        InsertAddrTable(pc_index, both_list, BOTH);
        InsertAddrTable(pc_index, isb_list, FIRST);
        #if USE_SHADOW_CACHE
        InsertShadowCache(pc_index, bo_list, SECOND);
        #endif
    }

    if (type == SECOND || type == BOTH) {
        InsertAddrTable(pc_index, both_list, BOTH);
        InsertAddrTable(pc_index, bo_list, SECOND);
        #if USE_SHADOW_CACHE
        InsertShadowCache(pc_index, isb_list, FIRST);
        #endif
    }

    #if USE_SHADOW_CACHE
    if (type == NEITHER) {
        InsertShadowCache(pc_index, both_list, BOTH);
        InsertShadowCache(pc_index, bo_list, SECOND);
        InsertShadowCache(pc_index, isb_list, FIRST);
    }
    #endif

    return;
}


void HybridPrefetcher3::returnPrefetchIssuedInfo(IntPtr prefetch_pc, IntPtr current_address) {

    int addr_index = searchAddrTable(current_address);
    if (addr_index == -1) {
        printf("Prefetch: Address %lx got flushed counter=%d\n", current_address, got_flushed++);
        return;
    }

    uint32_t addr_prefetch_type = tracked_addr[addr_index][TYPE];
    int pc_index_cur_addr = tracked_addr[addr_index][PC_INDEX];
    tracked_addr[addr_index][STATUS] = ISSUED;
    if (prefetch_pc == tracked_pc_list[pc_index_cur_addr]) {
        switch(addr_prefetch_type) {
            case FIRST: tracked_pc[pc_index_cur_addr][ISB_PRE] += 1; break;
            case SECOND: tracked_pc[pc_index_cur_addr][BO_PRE] += 1; break;
            case BOTH: {
                           tracked_pc[pc_index_cur_addr][ISB_PRE] += 1;
                           tracked_pc[pc_index_cur_addr][BO_PRE] += 1;
                           break;
                       }
            default: printf("Shouldn't be\n");
        }
        updatePCDecision(pc_index_cur_addr);
    } else {

        uint32_t pc_index = searchPCTable(prefetch_pc);

        addr_prefetch_type = tracked_addr[addr_index][TYPE];
        tracked_pc[pc_index][ISB_PRE] += (addr_prefetch_type != SECOND);
        tracked_pc[pc_index][BO_PRE] += (addr_prefetch_type != FIRST );
        tracked_addr[addr_index][PC_INDEX] = pc_index;
        
        updatePCDecision(pc_index);
    }

    return;
}

void HybridPrefetcher3::returnPrefetchHitInfo(IntPtr prefetch_pc, IntPtr current_address) {

    int addr_index = searchAddrTable(current_address);
    if (addr_index == -1) {
        printf("Hit: Address %lx got flushed counter=%d\n", current_address, got_flushed++);
        return;
    }

    uint32_t addr_prefetch_type = tracked_addr[addr_index][TYPE];
    uint32_t pc_index_cur_addr = tracked_addr[addr_index][PC_INDEX];
    tracked_addr[addr_index][STATUS] = HIT;
    if (prefetch_pc == tracked_pc_list[pc_index_cur_addr]) {
        switch(addr_prefetch_type) {
            case FIRST: tracked_pc[pc_index_cur_addr][ISB_HIT] += 1; break;
            case SECOND: tracked_pc[pc_index_cur_addr][BO_HIT] += 1; break;
            case BOTH: {
                           tracked_pc[pc_index_cur_addr][ISB_HIT] += 1;
                           tracked_pc[pc_index_cur_addr][BO_HIT] += 1;
                           break;
                       }
            default: printf("Shouldn't be\n");
        }
        updatePCDecision(pc_index_cur_addr);
    } else {
        uint32_t pc_index = searchPCTable(prefetch_pc);

        // addr_prefetch_type = tracked_addr[addr_index][TYPE];
        tracked_pc[pc_index][ISB_HIT] += (addr_prefetch_type != SECOND);
        tracked_pc[pc_index][BO_HIT] += (addr_prefetch_type != FIRST );
        tracked_addr[addr_index][PC_INDEX] = pc_index;

        updatePCDecision(pc_index);
    }

    return;
}

void HybridPrefetcher3::returnCacheMissInfo(IntPtr current_pc, IntPtr current_address) {
#if USE_SHADOW_CACHE
    int addr_index = searchShadowCache(current_address);
    int pc_index = -1; 
    if (addr_index != -1) {
        pc_index = shadow_cache[addr_index][PC_INDEX];
        if (shadow_cache[addr_index][STATUS] == ISSUED) {
            shadow_cache[addr_index][STATUS] = HIT;
            updateShadowPCHIT(pc_index, shadow_cache[addr_index][TYPE]);
        }
    }

    pc_index = searchPCTable(current_pc);
    if (pc_index != -1) {
        tracked_pc[pc_index][PC_MISS]++;
        updatePCDecision(pc_index);
    }
#endif
}

void HybridPrefetcher3::updatePCDecision(uint32_t pc_index) {
#if USE_PRE_PHASE
    if (tracked_pc[pc_index][ISB_PRE] >= PRE_PHASE_LENGTH*2) {
        tracked_pc[pc_index][ISB_HIT] *= 0.5;
        tracked_pc[pc_index][ISB_PRE] = PRE_PHASE_LENGTH;
    }
    if (tracked_pc[pc_index][BO_PRE] >= PRE_PHASE_LENGTH*2) {
        tracked_pc[pc_index][BO_HIT] *= 0.5;
        tracked_pc[pc_index][BO_PRE] = PRE_PHASE_LENGTH;
    }

    #if USE_SHADOW_CACHE
    if (tracked_pc[pc_index][PC_MISS] >= MISS_PHASE_LENGTH*2) {
        if (tracked_pc[pc_index][BO_SHAPRE] >= 2*BO_ACCESS_THRES) {
            tracked_pc[pc_index][BO_SHAHIT] *= 0.5;
            tracked_pc[pc_index][BO_SHAPRE] *= 0.5;
        }
        if (tracked_pc[pc_index][ISB_SHAPRE] >= 2*ISB_ACCESS_THRES) {
            tracked_pc[pc_index][ISB_SHAHIT] *= 0.5;
            tracked_pc[pc_index][ISB_SHAPRE] *= 0.5;
        }

        tracked_pc[pc_index][PC_MISS] = MISS_PHASE_LENGTH;
    }
    #endif


    uint32_t isb_choice = 0;
    if (tracked_pc[pc_index][ISB_PRE] < ISB_ACCESS_THRES) {
        isb_choice = FIRST;
    } else {
        int isb_hit = tracked_pc[pc_index][ISB_HIT];
        int isb_pre = tracked_pc[pc_index][ISB_PRE];
        double accu_isb = (isb_pre)?((double)isb_hit/isb_pre):0;
        
        uint32_t cur_deci = tracked_pc[pc_index][DECI];
        if (cur_deci == FIRST) {
            isb_choice = (accu_isb > ISB_LOW_THRES)?FIRST:0;
        } 
        #if !USE_SHADOW_CACHE
        else if (cur_deci == BOTH || cur_deci == SECOND) {
            isb_choice = (accu_isb > ISB_THRES)?FIRST:0;
        } 
        #else
        else if (cur_deci == BOTH) {
            isb_choice = (accu_isb > ISB_THRES)?FIRST:0;
        }
        else if (tracked_pc[pc_index][ISB_SHAPRE] > ISB_ACCESS_THRES) {
            isb_hit = tracked_pc[pc_index][ISB_SHAHIT]; 
            isb_pre = tracked_pc[pc_index][ISB_SHAPRE]; 
            accu_isb = (double)isb_hit/isb_pre;

            if (accu_isb > ISB_TURN_BACK_THRES) {
                isb_choice = FIRST;
                tracked_pc[pc_index][ISB_HIT] = isb_hit;
                tracked_pc[pc_index][ISB_PRE] = isb_pre;
            }
        }
        #endif
    }

    uint32_t bo_choice = 0;
    if (tracked_pc[pc_index][BO_PRE] < BO_ACCESS_THRES) {
        bo_choice = SECOND;
    } else {
        int bo_hit = tracked_pc[pc_index][BO_HIT];
        int bo_pre = tracked_pc[pc_index][BO_PRE];
        double accu_bo = (bo_pre)?((double)bo_hit/bo_pre):0;

        uint32_t cur_deci = tracked_pc[pc_index][DECI];
        if (cur_deci == SECOND) {
            bo_choice = (accu_bo > BO_LOW_THRES)?SECOND:0;
        } 
        #if USE_SHADOW_CACHE
        else if (cur_deci == BOTH || cur_deci == FIRST) {
            bo_choice = (accu_bo > BO_THRES)?SECOND:0;
        }
        #else
        else if (cur_deci == BOTH) {
            bo_choice = (accu_bo > BO_THRES)?SECOND:0;
        }
        else if (tracked_pc[pc_index][BO_SHAPRE] > BO_ACCESS_THRES) {
            bo_hit = tracked_pc[pc_index][BO_SHAHIT]; 
            bo_pre = tracked_pc[pc_index][BO_SHAPRE]; 
            accu_bo = (double)bo_hit/bo_pre;

            if (accu_bo > BO_TURN_BACK_THRES) {
                bo_choice = SECOND;
                tracked_pc[pc_index][BO_HIT] = bo_hit;
                tracked_pc[pc_index][BO_PRE] = bo_pre;
            }
        }
        #endif
    }

    tracked_pc[pc_index][DECI] = isb_choice + bo_choice;
#endif

    return;
}




/* These are all adds-on
 */
#if USE_SHADOW_CACHE
void HybridPrefetcher3::initNewShadowPrefetch(uint64_t new_addr, uint32_t type, uint32_t pc_index) {
    shadow_cache_list[shadow_cache_cursor] = new_addr;
    shadow_cache[shadow_cache_cursor][TYPE] = type;
    shadow_cache[shadow_cache_cursor][PC_INDEX] = pc_index;
    shadow_cache[shadow_cache_cursor][STATUS] = ISSUED;

    shadow_cache_cursor = (shadow_cache_cursor + 1)%(SHADOW_CACHE_SIZE_RATIO*STORAGE_SIZE);
    shadow_cache_size += (shadow_cache_size != (SHADOW_CACHE_SIZE_RATIO*STORAGE_SIZE))?1:0;
    
    if (!shadow_cache_cursor) {
        printf("shadow_cache used up once\n");
    }

    return;
}

int HybridPrefetcher3::searchShadowCache(uint64_t target_addr) {
    int index = -1;
    uint32_t i;

    for (i = 0; i < shadow_cache_size; i++) {
        if (target_addr == shadow_cache_list[i]) {
            index = i;
            break;
        }
    }

    return index;
}

void HybridPrefetcher3::InsertShadowCache(uint32_t pc_index, std::vector<IntPtr> list, uint32_t type) {
    int addr_index = -1;
    for (std::vector<IntPtr>::iterator it = list.begin(); it != list.end(); ++it) {
        addr_index = searchShadowCache(*it);
        if (addr_index == -1) {
            initNewShadowPrefetch(*it, type, pc_index);
            updateShadowPCPRE(pc_index, type);
        } else {
            if (pc_index == shadow_cache[addr_index][PC_INDEX]) {
                switch(shadow_cache[addr_index][STATUS]) {
                    case ISSUED: {
                                    if (shadow_cache[addr_index][TYPE] != type) {
                                        tracked_pc[pc_index][BO_SHAPRE] += (shadow_cache[addr_index][TYPE] == FIRST && type != FIRST);
                                        tracked_pc[pc_index][ISB_SHAPRE] += (shadow_cache[addr_index][TYPE] == SECOND && type != SECOND);
                                        shadow_cache[addr_index][TYPE] = BOTH;
                                    }
                                    break;
                                 }
                    case HIT: {
                                shadow_cache[addr_index][TYPE] = type;
                                shadow_cache[addr_index][STATUS] = ISSUED;
                                updateShadowPCPRE(pc_index, type);
                                break;
                              }
                    case INSERTED:
                    default: printf("shadow_cache: shouldn't be\n");
                }
            } else {
                switch(shadow_cache[addr_index][STATUS]) {
                    // case ISSUED: break;
                    case ISSUED:
                    case HIT:
                    case INSERTED: {
                                       shadow_cache[addr_index][PC_INDEX] = pc_index;
                                       shadow_cache[addr_index][TYPE] = type;
                                       shadow_cache[addr_index][STATUS] = ISSUED;
                                       updateShadowPCPRE(pc_index, type);
                                       break;
                                   }
                    default: printf("shadow_cache: shouldn't be\n");
                }
            }
        }
    }

    return;
}

void HybridPrefetcher3::updateShadowPCPRE(uint32_t pc_index, uint32_t type) {
    switch(type) {
        case FIRST: tracked_pc[pc_index][ISB_SHAPRE]++; break;
        case SECOND: tracked_pc[pc_index][BO_SHAPRE]++; break;
        case BOTH: {
                       tracked_pc[pc_index][ISB_SHAPRE]++;
                       tracked_pc[pc_index][BO_SHAPRE]++;
                       break;
                   }
        default: printf("In shadowcache: Shouldn't be\n");
    }
    return;
}

void HybridPrefetcher3::updateShadowPCHIT(uint32_t pc_index, uint32_t type) {
    switch(type) {
        case FIRST: tracked_pc[pc_index][ISB_SHAHIT]++; break;
        case SECOND: tracked_pc[pc_index][BO_SHAHIT]++; break;
        case BOTH: {
                       tracked_pc[pc_index][ISB_SHAHIT]++;
                       tracked_pc[pc_index][BO_SHAHIT]++;
                       break;
                   }
        default: printf("In shadowcache: Shouldn't be\n");
    }
    return;
}
#endif
