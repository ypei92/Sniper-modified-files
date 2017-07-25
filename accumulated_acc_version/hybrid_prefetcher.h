#ifndef __HYBRID_PREFETCHER_H
#define __HYBRID_PREFETCHER_H

#include "prefetcher.h"
#include "isb_prefetcher.h"
#include "bo.h"
#include <fstream>

#define DEBUG 0

#define HEADROOM_TEST 0
#define HEADROOM_TEST_MULTI 0
#define NAIVE_HYBRID 0
#define STRAT_ACCURACY 0
#define STRAT_SCORE 0
#define STRAT_SHARED 0
#define SEPARATE_ACC 1

#define GLOBAL_DECISION 0
#define IF_RECORD_ACC 0

#define TEST_MODE (HEADROOM_TEST||HEADROOM_TEST_MULTI||NAIVE_HYBRID)
#define NORMAL_MODE !TEST_MODE

/* if train the prefetch even don't issue it*/
#define TRAIN_BOTH 1

#define NEITHER 0
#define FIRST 1
#define SECOND 2
#define BOTH 3

#define STORAGE_SIZE 4096 
#define ADDR_TABLE_SIZE_RATIO 8


/*---------------GLOBAL setting-----------------*/
/*
#define ISB_ACCESS_THRES 2048 
#define BO_ACCESS_THRES 1024 
*/

#define ISB_ACCESS_THRES 1024 
#define BO_ACCESS_THRES 512 

/*
#define ISB_ACCESS_THRES 1024 
#define BO_ACCESS_THRES 512 
*/

/* real9
#define ISB_ACCESS_THRES 256 
#define BO_ACCESS_THRES 128 
*/
/* real2
#define ISB_ACCESS_THRES 750 
#define BO_ACCESS_THRES 500
*/

/*---------------Heuristic setting-----------------*/
#if STRAT_SHARED
#define BOTH_THRES 0.7
#define NEITHER_THRES 0.25
#define SHARED_SCORE_THRES -5
#endif

#if GLOBAL_DECISION
#define GLOBAL_ISB_THRES 0.4
#define GLOBAL_BO_THRES 0.6
#endif

#if SEPARATE_ACC
/* best setting from headroom test 3.2 
#define ISB_THRES 0.7
#define ISB_LOW_THRES 0.3
#define BO_THRES 0.75
#define BO_LOW_THRES 0.4
*/

/* best setting from headroom test 6.4 
*/
#define ISB_THRES 0.4
#define ISB_LOW_THRES 0.4
#define BO_THRES 0.6
#define BO_LOW_THRES 0.5

/* collecting acc info 
#define ISB_THRES 0.3
#define ISB_LOW_THRES 0.3
#define BO_THRES 0.3
#define BO_LOW_THRES 0.3
*/

#endif

/*---------------Other definition-----------------*/
#define PC 0
#define DECI 1
#define HIT 2
#define PRE 3
#define ISB_HIT 4
#define ISB_PRE 5
#define BO_HIT 6
#define BO_PRE 7

#if IF_RECORD_ACC
#define BO_START_ACC 0
#define BO_END_ACC 1
#define BO_HI_ACC 2
#define BO_LOW_ACC 3
#define ISB_START_ACC 4
#define ISB_END_ACC 5
#define ISB_HI_ACC 6
#define ISB_LOW_ACC 7
#endif

#define ADDR 0
#define TYPE 1
#define PC_INDEX 2 
#define DANGER 3
#define DANGER_THRES 8 

#define USE_BANDWIDTH 0
#define HIGH_SPARE    40000 
#define LOW_SPARE   10000

class HybridPrefetcher: public Prefetcher
{
    public:
        HybridPrefetcher(String configName, core_id_t core_id);
        ~HybridPrefetcher();

        std::vector<IntPtr> getNextAddress(IntPtr pc, IntPtr currentAddress, bool cache_hit, core_id_t core_id);

#if NORMAL_MODE
        void initNewPC(uint32_t new_pc);
        void initNewAddr(uint32_t new_addr, uint32_t type, uint32_t related_pc);
        int searchAddrTable(uint32_t target_addr);
        int searchPCTable(uint32_t target_pc);
        void updatePCDecision(uint32_t pc_index);
        void InsertAddrTable(uint32_t pc_index, std::vector<IntPtr> list, uint32_t type);
        void InsertAddrTable(uint32_t pc_index, std::vector<IntPtr> list0, std::vector<IntPtr> list1, uint32_t type);
        virtual void returnPrefetchIssuedInfo(IntPtr prefetch_pc, IntPtr current_address); //From cache_cntlr, issued pc
        virtual void returnPrefetchHitInfo(IntPtr prefetch_pc, IntPtr current_address); //From cache_cntlr, hit pc
        virtual void returnMemoryUtilization(double spare) {memory_spare = spare;}
        void hybrid_info_print() {
            uint32_t i = 0;
            for ( i = 0; i < tracked_pc_size ; i++) {
                if ((tracked_pc[i][BO_HIT] + tracked_pc[i][ISB_HIT]) > 32) {
                    printf("pc:%d deci:%d bo_hit:%d bo_pre:%d isb_hit:%d isb_pre:%d\n",
                            tracked_pc[i][PC], tracked_pc[i][DECI],tracked_pc[i][BO_HIT], tracked_pc[i][BO_PRE],
                            tracked_pc[i][ISB_HIT], tracked_pc[i][ISB_PRE]);
                }
            }
        }
#endif

#if HEADROOM_TEST
        char decision_file_name[32];
        uint32_t** decision_list;
        uint32_t decision_list_size;
#endif

#if HEADROOM_TEST_MULTI
        char decision_file_name[32];
        int** decision_list;
        int decision_list_size;
        int current_phase;
        virtual void returnPhaseInfo(uint32_t c_phase) {
            current_phase = (int) c_phase;
            return;
        }
#endif

    private:
        IsbPrefetcher *isb_prefetcher; 
        BOPrefetcher *bo_prefetcher; 

        std::vector<IntPtr> prefetchList;

#if NORMAL_MODE 
        /* Prefetcher decider variables */
        /* 0: PC
         * 1: Decision default:both
         * 2: hit
         * 3: access
         * 4: isb hit
         * 5: isb access
         * 6: bo hit
         * 7: bo access
         */
        uint32_t** tracked_pc;
        uint32_t tracked_pc_size;

        /*
         * 0: address
         * 1: isb:0, bo:1, both:2
         * 2: pc_index
         */
        // uint32_t tracked_addr[ADDR_TABLE_SIZE_RATIO*STORAGE_SIZE][4];
        uint32_t** tracked_addr;
        uint32_t tracked_addr_size;
        uint32_t tracked_addr_cursor;

        #if GLOBAL_DECISION
        uint32_t global_bo_decision;
        uint32_t global_isb_decision;
        #endif

        #if IF_RECORD_ACC
        float** tracked_acc;
        void acc_info_print();
        std::ofstream foutacc_bo;
        std::ofstream foutacc_isb;
        std::ofstream foutphase_bo;
        std::ofstream foutphase_isb;
        int current_phase;
        uint32_t testing_pc;
        virtual void returnPhaseInfo(uint32_t c_phase) {
            current_phase = (int) c_phase;
            return;
        }

        int sample_interval;
        int sample_counter;
        #endif

        double memory_spare;

        void print_addr_line(uint32_t index) {
            printf("[addr]=%d, [type]=%d, [pc_index]=%d, [danger]=%d\n",
                    tracked_addr[index][0], tracked_addr[index][1], tracked_addr[index][2], tracked_addr[index][3]);
        }
        void print_pc_line(uint32_t index) {

        }
#endif


};

#endif // __HYBRID_PREFETCHER_H
