#ifndef __HYBRID_PREFETCHER3_H
#define __HYBRID_PREFETCHER3_H

#include "prefetcher.h"
#include "isb_prefetcher.h"
#include "bo.h"
#include <fstream>

#define DEBUG 0
#define SEPARATE_ACC 1

#define USE_PRE_PHASE 1
#define USE_MISS_PHASE 0

#define USE_SHADOW_CACHE 1

#define IF_RECORD_ACC 0
#define IF_CONFLICT_ISSUE_TABLE 0

/* if train the prefetch even don't issue it*/
#define TRAIN_BOTH 1

#define NEITHER 0
#define FIRST 1
#define SECOND 2
#define BOTH 3

#define STORAGE_SIZE 4096 
#define ADDR_TABLE_SIZE_RATIO 8 
#define SHADOW_PC_SIZE_RATIO 1
#define SHADOW_CACHE_SIZE_RATIO 2
#define CONFLICT_ISSUE_TABLE_SIZE_RATIO 2



/*---------------GLOBAL setting-----------------*/
#define ISB_ACCESS_THRES 1024 
#define BO_ACCESS_THRES 512 

/*---------------Heuristic setting-----------------*/
#define ISB_COV_THRES 10 
#define BO_COV_THRES 10 

#define ISB_THRES 0.6
#define ISB_LOW_THRES 0.4
#define BO_THRES 0.5
#define BO_LOW_THRES 0.3

#define ISB_TURN_BACK_THRES 0.7
#define BO_TURN_BACK_THRES 0.6

/*---------------Other definition-----------------*/
#if USE_PRE_PHASE
    #define PC 100 
    #define DECI 0
    #define ISB_HIT 1
    #define ISB_PRE 2
    #define ISB_SHAHIT 3
    #define ISB_SHAPRE 4
    #define BO_HIT 5
    #define BO_PRE 6
    #define BO_SHAHIT 7
    #define BO_SHAPRE 8
    #define PC_MISS 9 
    #define TRACKED_PC_LINE_LENGTH 10 

    #define PRE_PHASE_LENGTH 4096
    #define MISS_PHASE_LENGTH 8192
#endif

/*---------------------addr table--------------------*/
#define ADDR 100 
#define TYPE 0
#define PC_INDEX 1 
#define STATUS 2
    #define EMPTY 0 
    #define INSERTED 1 
    #define ISSUED 2 
    #define HIT 3
#define TRACKED_ADDR_LINE_LENGTH 3 
#define SHADOW_CACHE_LINE_LENGTH 3 
#define CONFLICT_ISSUE_TABLE_LINE_LENGTH 3 


class HybridPrefetcher3: public Prefetcher
{
    public:
        HybridPrefetcher3(String configName, core_id_t core_id);
        ~HybridPrefetcher3();

        std::vector<IntPtr> getNextAddress(IntPtr pc, IntPtr currentAddress, bool cache_hit, core_id_t core_id);

        void initNewPC(uint64_t new_pc);
        void initNewAddr(uint64_t new_addr, uint32_t type, uint32_t pc_index);
        int searchAddrTable(uint64_t target_addr);
        int searchPCTable(uint64_t target_pc);
        void updatePCDecision(uint32_t pc_index);
        void InsertAddrTable(uint32_t pc_index, std::vector<IntPtr> list, uint32_t type);
        void InsertAddrTable(uint32_t pc_index, std::vector<IntPtr> list0, std::vector<IntPtr> list1, uint32_t type);
        virtual void returnCacheMissInfo(IntPtr current_pc, IntPtr current_address); 
        virtual void returnPrefetchIssuedInfo(IntPtr prefetch_pc, IntPtr current_address); //From cache_cntlr, issued pc
        virtual void returnPrefetchHitInfo(IntPtr prefetch_pc, IntPtr current_address); //From cache_cntlr, hit pc
        void hybrid_info_print() {
            uint32_t i = 0;
            for ( i = 0; i < tracked_pc_size ; i++) {
                if ((tracked_pc[i][BO_HIT] + tracked_pc[i][ISB_HIT]) > 32) {
                    printf("pc:%ld deci:%d bo_hit:%d bo_pre:%d isb_hit:%d isb_pre:%d\n",
                            tracked_pc_list[i], tracked_pc[i][DECI],tracked_pc[i][BO_HIT], tracked_pc[i][BO_PRE],
                            tracked_pc[i][ISB_HIT], tracked_pc[i][ISB_PRE]);
                }
            }
        }

        #if USE_SHADOW_CACHE
        void initNewShadowPrefetch(uint64_t new_addr, uint32_t type, uint32_t pc_index);
        int searchShadowCache(uint64_t target_addr);
        void InsertShadowCache(uint32_t pc_index, std::vector<IntPtr> list, uint32_t type);
        void updateShadowPCPRE(uint32_t pc_index, uint32_t type);
        void updateShadowPCHIT(uint32_t pc_index, uint32_t type);
        #endif

    private:
        IsbPrefetcher *isb_prefetcher; 
        BOPrefetcher *bo_prefetcher; 

        std::vector<IntPtr> prefetchList;

        /* Prefetcher decider variables */
        uint64_t* tracked_pc_list;
        uint32_t** tracked_pc;
        uint32_t tracked_pc_size;

        /*
         * 0: address
         * 1: isb:0, bo:1, both:2
         * 2: pc_index
         * 3: status
         */
        uint64_t* tracked_addr_list;
        uint32_t** tracked_addr;
        uint32_t tracked_addr_size;
        uint32_t tracked_addr_cursor;

        #if USE_SHADOW_CACHE
        uint64_t* shadow_cache_list;
        uint32_t** shadow_cache;
        uint32_t shadow_cache_size;
        uint32_t shadow_cache_cursor;
        #endif

        uint32_t got_flushed;
};

#endif // __HYBRID_PREFETCHER3_H
