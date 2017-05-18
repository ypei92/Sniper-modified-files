#ifndef __HYBRID_PREFETCHER_H
#define __HYBRID_PREFETCHER_H

#include "prefetcher.h"
#include "isb_prefetcher.h"
#include "bo.h"

#define DEBUG 0
#define HEADROOM_TEST 0
#define NAIVE_HYBRID 0
#define STRAT_ACCURACY 0
#define STRAT_SCORE 0
#define STRAT_SHARED 1
#define TRAIN_BOTH 1

#define NEITHER 0
#define FIRST 1
#define SECOND 2
#define BOTH 3

#define STORAGE_SIZE 4096 
/* all others
#define ADDR_TABLE_SIZE_RATIO 16
*/
/* real7
 * */
#define ADDR_TABLE_SIZE_RATIO 8

/* real4
#define ISB_ACCESS_THRES 2048 
#define BO_ACCESS_THRES 1024 
#define ACCURACY_THRES 0.1
*/
/* real5 real6 real7 real1
#define ISB_ACCESS_THRES 512 
#define BO_ACCESS_THRES 256 
#define ACCURACY_THRES 0.15
*/
/* real8 real0
#define ISB_ACCESS_THRES 1024 
#define BO_ACCESS_THRES 512 
#define ACCURACY_THRES 0.15
*/
/* real9
#define ISB_ACCESS_THRES 256 
#define BO_ACCESS_THRES 128 
#define ACCURACY_THRES 0.15
*/
/* real2
*/
#define ISB_ACCESS_THRES 750 
#define BO_ACCESS_THRES 500
#define ACCURACY_THRES 0.15


#define PC 0
#define DECI 1
#define HIT 2
#define PRE 3
#define ISB_HIT 4
#define ISB_PRE 5
#define BO_HIT 6
#define BO_PRE 7

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

#if STRAT_SHARED
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
        void hybrid_info_print()
        {
            uint32_t i = 0;
            for ( i = 0; i < tracked_pc_size ; i++) {
                if ((tracked_pc[i][BO_HIT] + tracked_pc[i][ISB_HIT]) > 32) {
                    printf("pc:%d deci:%d bo_hit:%d bo_pre:%d isb_hit:%d isb_pre:%d shared_hit:%d shared_pre:%d\n",
                            tracked_pc[i][PC], tracked_pc[i][DECI],tracked_pc[i][BO_HIT], tracked_pc[i][BO_PRE],
                            tracked_pc[i][ISB_HIT], tracked_pc[i][ISB_PRE], tracked_pc[i][HIT], tracked_pc[i][PRE]);
                }
            }
        }
#endif

    private:
        IsbPrefetcher *isb_prefetcher; 
        BOPrefetcher *bo_prefetcher; 

        std::vector<IntPtr> prefetchList;

#if HEADROOM_TEST
        char decision_file_name[32];
        uint32_t decision_list[4096][2];
        uint32_t decision_list_size;
#endif


#if STRAT_SHARED
        /* Prefetcher decider variables */
        /* 0: PC
         * 1: Decision default:both
         * 2: hit
         * 3: access
         * 4: isb hit
         * 5: isb access
         * 6: bo hit
         * 7: bo access
         * uint32_t tracked_pc_cursor;
         * uint32_t find_stale_pc();
         */
        uint32_t tracked_pc[STORAGE_SIZE][8];
        uint32_t tracked_pc_size;
        /*
         * 0: address
         * 1: isb:0, bo:1, both:2
         * 2: pc_index
         */
        uint32_t tracked_addr[ADDR_TABLE_SIZE_RATIO*STORAGE_SIZE][4];
        uint32_t tracked_addr_size;
        uint32_t tracked_addr_cursor;

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

