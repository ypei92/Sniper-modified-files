#pragma once

#include "core.h"
#include "cache.h"
#include "prefetcher.h"
#include "shared_cache_block_info.h"
#include "address_home_lookup.h"
#include "../pr_l1_pr_l2_dram_directory_msi/shmem_msg.h"
#include "mem_component.h"
#include "semaphore.h"
#include "lock.h"
#include "setlock.h"
#include "fixed_types.h"
#include "shmem_perf_model.h"
#include "contention_model.h"
#include "req_queue_list_template.h"
#include "stats.h"
#include "subsecond_time.h"

#include "boost/tuple/tuple.hpp"

#include <fstream>

class DramCntlrInterface;
class ATD;

/* Enable to get a detailed count of state transitions */
//#define ENABLE_TRANSITIONS

/* Enable to track latency by HitWhere */
//#define TRACK_LATENCY_BY_HITWHERE

// Forward declarations
namespace ParametricDramDirectoryMSI
{
   class CacheCntlr;
   class MemoryManager;
}
class FaultInjector;
class ShmemPerf;

// Maximum size of the list of addresses to prefetch
#define PREFETCH_MAX_QUEUE_LENGTH 32
// Time between prefetches
#define PREFETCH_INTERVAL SubsecondTime::NS(1)

namespace ParametricDramDirectoryMSI
{
   class Transition
   {
      public:
         enum reason_t
         {
            REASON_FIRST = 0,
            CORE_RD = REASON_FIRST,
            CORE_WR,
            CORE_RDEX,
            UPGRADE,
            EVICT,
            BACK_INVAL,
            COHERENCY,
            NUM_REASONS
         };
   };

   class Prefetch
   {
      public:
         enum prefetch_type_t
         {
            NONE,    // Not a prefetch
            OWN,     // Prefetch initiated locally
            OTHER    // Prefetch initiated by a higher-level cache
         };
   };

   class CacheParameters
   {
      public:
         String configName;
         UInt32 size;
         UInt32 num_sets;
         UInt32 associativity;
         String hash_function;
         String replacement_policy;
         bool perfect;
         bool coherent;
         ComponentLatency data_access_time;
         ComponentLatency tags_access_time;
         ComponentLatency writeback_time;
         ComponentBandwidthPerCycle next_level_read_bandwidth;
         String perf_model_type;
         bool writethrough;
         UInt32 shared_cores;
         String prefetcher;
         UInt32 outstanding_misses;
         CacheParameters()
            : data_access_time(NULL,0)
            , tags_access_time(NULL,0)
            , writeback_time(NULL,0)
         {}
         CacheParameters(
            String _configName, UInt32 _size, UInt32 _associativity, UInt32 block_size,
            String _hash_function, String _replacement_policy, bool _perfect, bool _coherent,
            const ComponentLatency& _data_access_time, const ComponentLatency& _tags_access_time,
            const ComponentLatency& _writeback_time, const ComponentBandwidthPerCycle& _next_level_read_bandwidth,
            String _perf_model_type, bool _writethrough, UInt32 _shared_cores,
            String _prefetcher, UInt32 _outstanding_misses)
         :
            configName(_configName), size(_size), associativity(_associativity),
            hash_function(_hash_function), replacement_policy(_replacement_policy), perfect(_perfect), coherent(_coherent),
            data_access_time(_data_access_time), tags_access_time(_tags_access_time),
            writeback_time(_writeback_time), next_level_read_bandwidth(_next_level_read_bandwidth),
            perf_model_type(_perf_model_type), writethrough(_writethrough), shared_cores(_shared_cores),
            prefetcher(_prefetcher), outstanding_misses(_outstanding_misses)
         {
            num_sets = k_KILO * _size / (_associativity * block_size);
            LOG_ASSERT_ERROR(k_KILO * _size == num_sets * associativity * block_size, "Invalid cache configuration: size(%d Kb) != sets(%d) * associativity(%d) * block_size(%d)", _size, num_sets, associativity, block_size);
         }
   };

   class CacheCntlrList : public std::vector<CacheCntlr*>
   {
      public:
         #ifdef ENABLE_TRACK_SHARING_PREVCACHES
         PrevCacheIndex find(core_id_t core_id, MemComponent::component_t mem_component);
         #endif
   };

   class CacheDirectoryWaiter
   {
      public:
         bool exclusive;
         bool isPrefetch;
         CacheCntlr* cache_cntlr;
         SubsecondTime t_issue;
         CacheDirectoryWaiter(bool _exclusive, bool _isPrefetch, CacheCntlr* _cache_cntlr, SubsecondTime _t_issue) :
            exclusive(_exclusive), isPrefetch(_isPrefetch), cache_cntlr(_cache_cntlr), t_issue(_t_issue)
         {}
   };

   typedef ReqQueueListTemplate<CacheDirectoryWaiter> CacheDirectoryWaiterMap;

   struct MshrEntry {
      SubsecondTime t_issue, t_complete;
   };
   typedef std::unordered_map<IntPtr, MshrEntry> Mshr;

   class CacheMasterCntlr
   {
      private:
         Cache* m_cache;
         Lock m_cache_lock;
         Lock m_smt_lock; //< Only used in L1 cache, to protect against concurrent access from sibling SMT threads
         CacheCntlrList m_prev_cache_cntlrs;
         Prefetcher** m_prefetcher;
         DramCntlrInterface* m_dram_cntlr;
         ContentionModel* m_dram_outstanding_writebacks;

         Mshr mshr;
         ContentionModel m_l1_mshr;
         ContentionModel m_next_level_read_bandwidth;
         CacheDirectoryWaiterMap m_directory_waiters;
         IntPtr m_evicting_address;
         Byte* m_evicting_buf;

         std::vector<ATD*> m_atds;

         std::vector<SetLock> m_setlocks;
         UInt32 m_log_blocksize;
         UInt32 m_num_sets;

         std::deque<IntPtr> m_prefetch_list;
         std::deque<IntPtr> m_prefetch_list_pc;
         std::deque<IntPtr> m_prefetch_core_list;
         SubsecondTime m_prefetch_next;

         void createSetLocks(UInt32 cache_block_size, UInt32 num_sets, UInt32 core_offset, UInt32 num_cores);
         SetLock* getSetLock(IntPtr addr);

         void createATDs(String name, String configName, core_id_t core_id, UInt32 shared_cores, UInt32 size, UInt32 associativity, UInt32 block_size,
            String replacement_policy, CacheBase::hash_t hash_function);
         void accessATDs(Core::mem_op_t mem_op_type, bool hit, IntPtr address, UInt32 core_num);

         CacheMasterCntlr(String name, core_id_t core_id, UInt32 outstanding_misses)
            : m_cache(NULL)
            , m_prefetcher(NULL)
            , m_dram_cntlr(NULL)
            , m_dram_outstanding_writebacks(NULL)
            , m_l1_mshr(name + ".mshr", core_id, outstanding_misses)
            , m_next_level_read_bandwidth(name + ".next_read", core_id)
            , m_evicting_address(0)
            , m_evicting_buf(NULL)
            , m_atds()
            , m_prefetch_list()
            , m_prefetch_list_pc()
            , m_prefetch_core_list()
            , m_prefetch_next(SubsecondTime::Zero())
         {}
         ~CacheMasterCntlr();

         friend class CacheCntlr;
   };

   class CacheCntlr : ::CacheCntlr
   {
      private:
         // Data Members
         MemComponent::component_t m_mem_component;
         MemoryManager* m_memory_manager;
         CacheMasterCntlr* m_master;
         CacheCntlr* m_next_cache_cntlr;
         CacheCntlr* m_last_level;
         AddressHomeLookup* m_tag_directory_home_lookup;
         std::unordered_map<IntPtr, MemComponent::component_t> m_shmem_req_source_map;
         bool m_perfect;
         bool m_passthrough;
         bool m_coherent;
         bool m_prefetch_on_prefetch_hit;
         bool m_l1_mshr;

         UInt64 prefetch_inserted;
         UInt64 redundant_in_progress;
         UInt64 redundant_in_cache;
         UInt64 redundant_prefetch_buffer;
 
         uint64_t total_demand_accesses;
         uint64_t phase_length;
         uint32_t current_phase;
         bool if_last_level_cache;
         void printOnePhase() {
            /*
            std::ofstream OUTFILE1;
            if (current_phase) {
                OUTFILE1.open("pc_accesses.csv", std::ofstream::app);
            } else {
                OUTFILE1.open("pc_accesses.csv");
            }

            OUTFILE1 << "phase = " << current_phase << std::endl;
            for (std::map<IntPtr, UInt64>::iterator it = pc_accesses.begin(); it != pc_accesses.end(); it++) {
	            assert(pc_hits.find(it->first) != pc_hits.end());
	            if (it->second > 0) {
		            OUTFILE1 << "PC: " << it->first << " prefetch_hits + misses: " 
                        << it->second << " prefetch_hits: " << pc_hits[it->first];
		            OUTFILE1 << " accuracy: " << (double) pc_hits[it->first]/ (double) it->second 
                        << std::endl;
	            }
            }
            std::cout<<std::endl;
            OUTFILE1.close();
            */

            std::ofstream OUTFILE2;
            if (current_phase) {
                printf("phase = %d\n", current_phase);
                OUTFILE2.open("pc_prefetches.csv", std::ofstream::app);
            } else {
                printf("phase = %d\n", current_phase);
                OUTFILE2.open("pc_prefetches.csv");
            }
            OUTFILE2 << "phase = " << current_phase << std::endl;
            for (std::map<IntPtr, UInt64>::iterator it = pc_prefetches.begin(); 
                    it != pc_prefetches.end(); it++) {
	            assert(pc_prefetch_hits.find(it->first) != pc_prefetch_hits.end());
	            if(it->second > 0) {
		            OUTFILE2 << "PC: " << it->first << " prefetches: " << it->second 
                        << " prefetch_hits: " << pc_prefetch_hits[it->first];
		            OUTFILE2 << " accuracy: " << (double) pc_prefetch_hits[it->first]/ (double) it->second 
                        << std::endl;
	            }
            }
            OUTFILE2.close();
         }

         void clearOnePhase() {
             pc_accesses.clear();
             pc_hits.clear();
             pc_prefetches.clear();
             pc_prefetch_hits.clear();
         }

         std::map<IntPtr, UInt64> pc_accesses;
         std::map<IntPtr, UInt64> pc_hits;
         std::map<IntPtr, UInt64> pc_prefetches;
         std::map<IntPtr, UInt64> pc_prefetch_hits;
         std::map<IntPtr, IntPtr> addr_prefetch_pc;
         std::map< std::pair<IntPtr,IntPtr>, UInt64 > trigger_demand_cnt;
         std::map< std::pair<IntPtr,IntPtr>, UInt64 > demand_trigger_cnt;
         UInt64 prev_load_misses, prev_loads_prefetch;
         SubsecondTime t_prev;
         double bandwidth_usage;
         uint32_t time_cnt;

         struct {
           UInt64 loads, stores;
           UInt64 load_misses, store_misses;
           UInt64 load_overlapping_misses, store_overlapping_misses;
           UInt64 loads_state[CacheState::NUM_CSTATE_STATES], stores_state[CacheState::NUM_CSTATE_STATES];
           UInt64 loads_where[HitWhere::NUM_HITWHERES], stores_where[HitWhere::NUM_HITWHERES];
           UInt64 load_misses_state[CacheState::NUM_CSTATE_STATES], store_misses_state[CacheState::NUM_CSTATE_STATES];
           UInt64 loads_prefetch, stores_prefetch;
           UInt64 hits_prefetch, // lines which were prefetched and subsequently used by a non-prefetch access
                  evict_prefetch, // lines which were prefetched and evicted before being used
                  invalidate_prefetch; // lines which were prefetched and invalidated before being used
                  // Note: hits_prefetch+evict_prefetch+invalidate_prefetch will not account for all prefetched lines,
                  // some may still be in the cache, or could have been removed for some other reason.
                  // Also, in a shared cache, the prefetch may have been triggered by another core than the one
                  // accessing/evicting the line so *_prefetch statistics should be summed across the shared cache
           UInt64 evict[CacheState::NUM_CSTATE_STATES];
           UInt64 backinval[CacheState::NUM_CSTATE_STATES];
           UInt64 hits_warmup, evict_warmup, invalidate_warmup;
           SubsecondTime total_latency;
           SubsecondTime snoop_latency;
           SubsecondTime qbs_query_latency;
           SubsecondTime mshr_latency;
           UInt64 prefetches;
           UInt64 coherency_downgrades, coherency_upgrades, coherency_invalidates, coherency_writebacks;
           #ifdef ENABLE_TRANSITIONS
           UInt64 transitions[CacheState::NUM_CSTATE_SPECIAL_STATES][CacheState::NUM_CSTATE_SPECIAL_STATES];
           UInt64 transition_reasons[Transition::NUM_REASONS][CacheState::NUM_CSTATE_SPECIAL_STATES][CacheState::NUM_CSTATE_SPECIAL_STATES];
           std::unordered_map<IntPtr, Transition::reason_t> seen;
           #endif
         } stats;

         #ifdef TRACK_LATENCY_BY_HITWHERE
         std::unordered_map<HitWhere::where_t, StatHist> lat_by_where;
         #endif

         void updateCounters(Core::mem_op_t mem_op_type, IntPtr address, bool cache_hit, CacheState::cstate_t state, Prefetch::prefetch_type_t isPrefetch);
         void cleanupMshr();
         void transition(IntPtr address, Transition::reason_t reason, CacheState::cstate_t old_state, CacheState::cstate_t new_state);
         void updateUncoreStatistics(HitWhere::where_t hit_where, SubsecondTime now);

         core_id_t m_core_id;
         UInt32 m_cache_block_size;
         bool m_cache_writethrough;
         ComponentLatency m_writeback_time;
         ComponentBandwidthPerCycle m_next_level_read_bandwidth;

         UInt32 m_shared_cores;        /**< Number of cores this cache is shared with */
         core_id_t m_core_id_master;   /**< Core id of the 'master' (actual) cache controller we're proxying */

         Semaphore* m_user_thread_sem;
         Semaphore* m_network_thread_sem;
         volatile HitWhere::where_t m_last_remote_hit_where;

         ShmemPerf* m_shmem_perf;
         ShmemPerf* m_shmem_perf_global;
         SubsecondTime m_shmem_perf_totaltime;
         UInt64 m_shmem_perf_numrequests;

         ShmemPerfModel* m_shmem_perf_model;

         // Core-interfacing stuff
         void accessCache(
               Core::mem_op_t mem_op_type,
               IntPtr ca_address, UInt32 offset,
               Byte* data_buf, UInt32 data_length, bool update_replacement, IntPtr ca_pc);
         bool operationPermissibleinCache(
               IntPtr address, Core::mem_op_t mem_op_type, CacheBlockInfo **cache_block_info = NULL);

         void copyDataFromNextLevel(Core::mem_op_t mem_op_type, IntPtr address, IntPtr pc, bool modeled, SubsecondTime t_start);
         void trainPrefetcher(IntPtr pc, IntPtr address, bool cache_hit, bool prefetch_hit, SubsecondTime t_issue, core_id_t coreid);
         void Prefetch(SubsecondTime t_start);
         void doPrefetch(IntPtr prefetch_address, IntPtr pc, SubsecondTime t_start, core_id_t coreid);
         void inform_tlb_eviction(IntPtr addr, UInt32 way);

         // Cache meta-data operations
         SharedCacheBlockInfo* getCacheBlockInfo(IntPtr address);
         CacheState::cstate_t getCacheState(IntPtr address);
         CacheState::cstate_t getCacheState(CacheBlockInfo *cache_block_info);
         SharedCacheBlockInfo* setCacheState(IntPtr address, CacheState::cstate_t cstate);

         // Cache data operations
         void invalidateCacheBlock(IntPtr address);
         void retrieveCacheBlock(IntPtr address, Byte* data_buf, ShmemPerfModel::Thread_t thread_num, bool update_replacement, IntPtr pc=0);


         SharedCacheBlockInfo* insertCacheBlock(IntPtr address, IntPtr pc, CacheState::cstate_t cstate, Byte* data_buf, core_id_t requester, ShmemPerfModel::Thread_t thread_num);
         std::pair<SubsecondTime, bool> updateCacheBlock(IntPtr address, CacheState::cstate_t cstate, Transition::reason_t reason, Byte* out_buf, ShmemPerfModel::Thread_t thread_num);
         void writeCacheBlock(IntPtr address, UInt32 offset, Byte* data_buf, UInt32 data_length, ShmemPerfModel::Thread_t thread_num);

         // Handle Request from previous level cache
         HitWhere::where_t processShmemReqFromPrevCache(CacheCntlr* requester, Core::mem_op_t mem_op_type, IntPtr address, bool modeled, bool count, Prefetch::prefetch_type_t isPrefetch, SubsecondTime t_issue, bool have_write_lock, IntPtr ca_pc);

         // Process Request from L1 Cache
         boost::tuple<HitWhere::where_t, SubsecondTime> accessDRAM(Core::mem_op_t mem_op_type, IntPtr address, bool isPrefetch, Byte* data_buf);
         void initiateDirectoryAccess(Core::mem_op_t mem_op_type, IntPtr address, bool isPrefetch, SubsecondTime t_issue);
         void processExReqToDirectory(IntPtr address);
         void processShReqToDirectory(IntPtr address);
         void processUpgradeReqToDirectory(IntPtr address, ShmemPerf *perf, ShmemPerfModel::Thread_t thread_num);

         // Process Request from Dram Dir
         void processExRepFromDramDirectory(core_id_t sender, core_id_t requester, PrL1PrL2DramDirectoryMSI::ShmemMsg* shmem_msg);
         void processShRepFromDramDirectory(core_id_t sender, core_id_t requester, PrL1PrL2DramDirectoryMSI::ShmemMsg* shmem_msg);
         void processUpgradeRepFromDramDirectory(core_id_t sender, core_id_t requester, PrL1PrL2DramDirectoryMSI::ShmemMsg* shmem_msg);
         void processInvReqFromDramDirectory(core_id_t sender, PrL1PrL2DramDirectoryMSI::ShmemMsg* shmem_msg);
         void processFlushReqFromDramDirectory(core_id_t sender, PrL1PrL2DramDirectoryMSI::ShmemMsg* shmem_msg);
         void processWbReqFromDramDirectory(core_id_t sender, PrL1PrL2DramDirectoryMSI::ShmemMsg* shmem_msg);

         // Cache Block Size
         UInt32 getCacheBlockSize() { return m_cache_block_size; }
         MemoryManager* getMemoryManager() { return m_memory_manager; }
         ShmemPerfModel* getShmemPerfModel() { return m_shmem_perf_model; }

         void updateUsageBits(IntPtr address, CacheBlockInfo::BitsUsedType used);
         void walkUsageBits();
         static SInt64 __walkUsageBits(UInt64 arg0, UInt64 arg1) { ((CacheCntlr*)arg0)->walkUsageBits(); return 0; }

         // Wake up User Thread
         void wakeUpUserThread(Semaphore* user_thread_sem = NULL);
         // Wait for User Thread
         void waitForUserThread(Semaphore* network_thread_sem = NULL);

         // Wait for Network Thread
         void waitForNetworkThread(void);
         // Wake up Network Thread
         void wakeUpNetworkThread(void);

         Semaphore* getUserThreadSemaphore(void);
         Semaphore* getNetworkThreadSemaphore(void);

         // Dram Directory Home Lookup
         core_id_t getHome(IntPtr address) { return m_tag_directory_home_lookup->getHome(address); }

         CacheCntlr* lastLevelCache(void);

      public:

         CacheCntlr(MemComponent::component_t mem_component,
               String name,
               core_id_t core_id,
               MemoryManager* memory_manager,
               AddressHomeLookup* tag_directory_home_lookup,
               Semaphore* user_thread_sem,
               Semaphore* network_thread_sem,
               UInt32 cache_block_size,
               CacheParameters & cache_params,
               ShmemPerfModel* shmem_perf_model,
               bool is_last_level_cache);

         virtual ~CacheCntlr();

         Cache* getCache() { return m_master->m_cache; }
         Lock& getLock() { return m_master->m_cache_lock; }

         void setPrevCacheCntlrs(CacheCntlrList& prev_cache_cntlrs);
         void setNextCacheCntlr(CacheCntlr* next_cache_cntlr) { m_next_cache_cntlr = next_cache_cntlr; }
         void createSetLocks(UInt32 cache_block_size, UInt32 num_sets, UInt32 core_offset, UInt32 num_cores) { m_master->createSetLocks(cache_block_size, num_sets, core_offset, num_cores); }
         void setDRAMDirectAccess(DramCntlrInterface* dram_cntlr, UInt64 num_outstanding);

         HitWhere::where_t processMemOpFromCore(
               Core::lock_signal_t lock_signal,
               Core::mem_op_t mem_op_type,
               IntPtr ca_address, UInt32 offset,
               Byte* data_buf, UInt32 data_length,
               bool modeled,
               bool count,
               IntPtr pc);
         void updateHits(Core::mem_op_t mem_op_type, UInt64 hits);

         // Notify next level cache of so it can update its sharing set
         void notifyPrevLevelInsert(core_id_t core_id, MemComponent::component_t mem_component, IntPtr address);
         void notifyPrevLevelEvict(core_id_t core_id, MemComponent::component_t mem_component, IntPtr address);

         // Handle message from Dram Dir
         void handleMsgFromDramDirectory(core_id_t sender, PrL1PrL2DramDirectoryMSI::ShmemMsg* shmem_msg);
         // Acquiring and Releasing per-set Locks
         void acquireLock(UInt64 address);
         void releaseLock(UInt64 address);
         void acquireStackLock(UInt64 address, bool this_is_locked = false);
         void releaseStackLock(UInt64 address, bool this_is_locked = false);

         bool isMasterCache(void) { return m_core_id == m_core_id_master; }
         bool isFirstLevel(void) { return m_master->m_prev_cache_cntlrs.empty(); }
         bool isLastLevel(void) { return ! m_next_cache_cntlr; }
         bool isShared(core_id_t core_id); //< Return true if core shares this cache

         bool isInLowerLevelCache(CacheBlockInfo *block_info);
         void incrementQBSLookupCost();

         void enable() { m_master->m_cache->enable(); }
         void disable() { m_master->m_cache->disable(); }

#ifdef KNAPSACK
        void inform_L1_access(UInt64 PC, UInt64 address);
        void inform_L2_access(UInt64 PC, UInt64 address);
        void inform_prefetch(IntPtr pc, IntPtr address);
#endif
         friend class CacheCntlrList;
         friend class MemoryManager;
   };

}
