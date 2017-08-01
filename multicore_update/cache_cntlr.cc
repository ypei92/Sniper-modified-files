#include "cache_cntlr.h"
#include "log.h"
#include "memory_manager.h"
#include "core_manager.h"
#include "simulator.h"
#include "subsecond_time.h"
#include "config.hpp"
#include "fault_injection.h"
#include "hooks_manager.h"
#include "cache_atd.h"
#include "shmem_perf.h"

#include <cstring>

// Define to allow private L2 caches not to take the stack lock.
// Works in most cases, but seems to have some more bugs or race conditions, preventing it from being ready for prime time.
//#define PRIVATE_L2_OPTIMIZATION

Lock iolock;
#if 0
#  define LOCKED(...) { ScopedLock sl(iolock); fflush(stderr); __VA_ARGS__; fflush(stderr); }
#  define LOGID() fprintf(stderr, "[%s] %2u%c [ %2d(%2d)-L%u%c ] %-25s@%3u: ", \
                     itostr(getShmemPerfModel()->getElapsedTime(Sim()->getCoreManager()->amiUserThread() ? ShmemPerfModel::_USER_THREAD : ShmemPerfModel::_SIM_THREAD)).c_str(), Sim()->getCoreManager()->getCurrentCoreID(), \
                     Sim()->getCoreManager()->amiUserThread() ? '^' : '_', \
                     m_core_id_master, m_core_id, m_mem_component < MemComponent::L2_CACHE ? 1 : m_mem_component - MemComponent::L2_CACHE + 2, \
                     m_mem_component == MemComponent::L1_ICACHE ? 'I' : (m_mem_component == MemComponent::L1_DCACHE  ? 'D' : ' '),  \
                     __FUNCTION__, __LINE__ \
                  );
#  define MYLOG(...) LOCKED(LOGID(); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n");)
#  define DUMPDATA(data_buf, data_length) { for(UInt32 i = 0; i < data_length; ++i) fprintf(stderr, "%02x ", data_buf[i]); }
#else
#  define MYLOG(...) {}
#endif

namespace ParametricDramDirectoryMSI
{

char CStateString(CacheState::cstate_t cstate) {
   switch(cstate)
   {
      case CacheState::INVALID:           return 'I';
      case CacheState::SHARED:            return 'S';
      case CacheState::SHARED_UPGRADING:  return 'u';
      case CacheState::MODIFIED:          return 'M';
      case CacheState::EXCLUSIVE:         return 'E';
      case CacheState::OWNED:             return 'O';
      case CacheState::INVALID_COLD:      return '_';
      case CacheState::INVALID_EVICT:     return 'e';
      case CacheState::INVALID_COHERENCY: return 'c';
      default:                            return '?';
   }
}

const char * ReasonString(Transition::reason_t reason) {
   switch(reason)
   {
      case Transition::CORE_RD:     return "read";
      case Transition::CORE_RDEX:   return "readex";
      case Transition::CORE_WR:     return "write";
      case Transition::UPGRADE:     return "upgrade";
      case Transition::EVICT:       return "evict";
      case Transition::BACK_INVAL:  return "backinval";
      case Transition::COHERENCY:   return "coherency";
      default:                      return "other";
   }
}

MshrEntry make_mshr(SubsecondTime t_issue, SubsecondTime t_complete) {
   MshrEntry e;
   e.t_issue = t_issue; e.t_complete = t_complete;
   return e;
}

#ifdef ENABLE_TRACK_SHARING_PREVCACHES
PrevCacheIndex CacheCntlrList::find(core_id_t core_id, MemComponent::component_t mem_component)
{
   for(PrevCacheIndex idx = 0; idx < size(); ++idx)
      if ((*this)[idx]->m_core_id == core_id && (*this)[idx]->m_mem_component == mem_component)
         return idx;
   LOG_PRINT_ERROR("");
}
#endif

void CacheMasterCntlr::createSetLocks(UInt32 cache_block_size, UInt32 num_sets, UInt32 core_offset, UInt32 num_cores)
{
   m_log_blocksize = floorLog2(cache_block_size);
   m_num_sets = num_sets;
   m_setlocks.resize(m_num_sets, SetLock(core_offset, num_cores));
}

SetLock*
CacheMasterCntlr::getSetLock(IntPtr addr)
{
   return &m_setlocks.at((addr >> m_log_blocksize) & (m_num_sets-1));
}

void
CacheMasterCntlr::createATDs(String name, String configName, core_id_t master_core_id, UInt32 shared_cores, UInt32 size,
   UInt32 associativity, UInt32 block_size, String replacement_policy, CacheBase::hash_t hash_function)
{
   // Instantiate an ATD for each sharing core
   for(UInt32 core_id = master_core_id; core_id < master_core_id + shared_cores; ++core_id)
   {
      m_atds.push_back(new ATD(name + ".atd", configName, core_id, size, associativity, block_size, replacement_policy, hash_function));
   }
}

void
CacheMasterCntlr::accessATDs(Core::mem_op_t mem_op_type, bool hit, IntPtr address, UInt32 core_num)
{
   if (m_atds.size())
      m_atds[core_num]->access(mem_op_type, hit, address);
}

CacheMasterCntlr::~CacheMasterCntlr()
{
   delete m_cache;
   for(std::vector<ATD*>::iterator it = m_atds.begin(); it != m_atds.end(); ++it)
   {
      delete *it;
   }
   //if(m_prefetcher)
    //  delete m_prefetcher;
}

CacheCntlr::CacheCntlr(MemComponent::component_t mem_component,
      String name,
      core_id_t core_id,
      MemoryManager* memory_manager,
      AddressHomeLookup* tag_directory_home_lookup,
      Semaphore* user_thread_sem,
      Semaphore* network_thread_sem,
      UInt32 cache_block_size,
      CacheParameters & cache_params,
      ShmemPerfModel* shmem_perf_model,
      bool is_last_level_cache):
   m_mem_component(mem_component),
   m_memory_manager(memory_manager),
   m_next_cache_cntlr(NULL),
   m_last_level(NULL),
   m_tag_directory_home_lookup(tag_directory_home_lookup),
   m_perfect(cache_params.perfect),
   m_passthrough(Sim()->getCfg()->getBoolArray("perf_model/" + cache_params.configName + "/passthrough", core_id)),
   m_coherent(cache_params.coherent),
   m_prefetch_on_prefetch_hit(false),
   m_l1_mshr(cache_params.outstanding_misses > 0),
   m_core_id(core_id),
   m_cache_block_size(cache_block_size),
   m_cache_writethrough(cache_params.writethrough),
   m_writeback_time(cache_params.writeback_time),
   m_next_level_read_bandwidth(cache_params.next_level_read_bandwidth),
   m_shared_cores(cache_params.shared_cores),
   m_user_thread_sem(user_thread_sem),
   m_network_thread_sem(network_thread_sem),
   m_last_remote_hit_where(HitWhere::UNKNOWN),
   m_shmem_perf(new ShmemPerf()),
   m_shmem_perf_global(NULL),
   m_shmem_perf_model(shmem_perf_model)
{
   total_demand_accesses = 0;
   phase_length = 100000;
   current_phase = 0;
   if_last_level_cache = is_last_level_cache;

   prev_loads_prefetch = 0;
   prev_load_misses = 0;
   time_cnt = 0;
   bandwidth_usage = 0;
   m_core_id_master = m_core_id - m_core_id % m_shared_cores;
   Sim()->getStatsManager()->logTopology(name, core_id, m_core_id_master);

   LOG_ASSERT_ERROR(!Sim()->getCfg()->hasKey("perf_model/perfect_llc"),
                    "perf_model/perfect_llc is deprecated, use perf_model/lX_cache/perfect instead");
   if (is_last_level_cache)
      LOG_ASSERT_ERROR(m_passthrough == false, "Cache pass-through not supported on last-level cache");

   std::cout << name << " " << m_mem_component << std::endl;
   if (isMasterCache())
   {
      /* Master cache */
      m_master = new CacheMasterCntlr(name, core_id, cache_params.outstanding_misses);
      m_master->m_cache = new Cache(name,
            "perf_model/" + cache_params.configName,
            m_core_id,
            cache_params.num_sets,
            cache_params.associativity,
            m_cache_block_size,
            cache_params.replacement_policy,
            CacheBase::SHARED_CACHE,
            CacheBase::parseAddressHash(cache_params.hash_function),
            Sim()->getFaultinjectionManager()
               ? Sim()->getFaultinjectionManager()->getFaultInjector(m_core_id_master, mem_component)
               : NULL);

      m_master->m_prefetcher = new Prefetcher*[4];
      for(unsigned int c=0; c < 4; c++) {
         m_master->m_prefetcher[c] = Prefetcher::createPrefetcher(cache_params.prefetcher, cache_params.configName, c, m_shared_cores);
          cout << "Creating prefetcher object " << c << hex << " " << m_master->m_prefetcher[c] << std::endl;
      }

 
 //     m_master->m_prefetcher = Prefetcher::createPrefetcher(cache_params.prefetcher, cache_params.configName, m_core_id, m_shared_cores);

      if (Sim()->getCfg()->getBoolDefault("perf_model/" + cache_params.configName + "/atd/enabled", false))
      {
         m_master->createATDs(name,
               "perf_model/" + cache_params.configName,
               m_core_id,
               m_shared_cores,
               cache_params.num_sets,
               cache_params.associativity,
               m_cache_block_size,
               cache_params.replacement_policy,
               CacheBase::parseAddressHash(cache_params.hash_function));
      }

      Sim()->getHooksManager()->registerHook(HookType::HOOK_ROI_END, __walkUsageBits, (UInt64)this, HooksManager::ORDER_NOTIFY_PRE);
   }
   else
   {
      /* Shared, non-master cache, we're just a proxy */
      m_master = getMemoryManager()->getCacheCntlrAt(m_core_id_master, mem_component)->m_master;
   }

   //if (m_master->m_prefetcher)
    //  m_prefetch_on_prefetch_hit = Sim()->getCfg()->getBoolArray("perf_model/" + cache_params.configName + "/prefetcher/prefetch_on_prefetch_hit", core_id);

   bzero(&stats, sizeof(stats));

   registerStatsMetric(name, core_id, "loads", &stats.loads);
   registerStatsMetric(name, core_id, "stores", &stats.stores);
   registerStatsMetric(name, core_id, "load-misses", &stats.load_misses);
   registerStatsMetric(name, core_id, "store-misses", &stats.store_misses);
   // Does not work for loads, since the interval core model doesn't issue the loads until after the first miss has completed
   registerStatsMetric(name, core_id, "load-overlapping-misses", &stats.load_overlapping_misses);
   registerStatsMetric(name, core_id, "store-overlapping-misses", &stats.store_overlapping_misses);
   registerStatsMetric(name, core_id, "loads-prefetch", &stats.loads_prefetch);
   registerStatsMetric(name, core_id, "stores-prefetch", &stats.stores_prefetch);
   registerStatsMetric(name, core_id, "hits-prefetch", &stats.hits_prefetch);
   registerStatsMetric(name, core_id, "evict-prefetch", &stats.evict_prefetch);
   registerStatsMetric(name, core_id, "invalidate-prefetch", &stats.invalidate_prefetch);
   registerStatsMetric(name, core_id, "hits-warmup", &stats.hits_warmup);
   registerStatsMetric(name, core_id, "evict-warmup", &stats.evict_warmup);
   registerStatsMetric(name, core_id, "invalidate-warmup", &stats.invalidate_warmup);
   registerStatsMetric(name, core_id, "total-latency", &stats.total_latency);
   registerStatsMetric(name, core_id, "snoop-latency", &stats.snoop_latency);
   registerStatsMetric(name, core_id, "qbs-query-latency", &stats.qbs_query_latency);
   registerStatsMetric(name, core_id, "mshr-latency", &stats.mshr_latency);
   registerStatsMetric(name, core_id, "prefetches", &stats.prefetches);
   for(CacheState::cstate_t state = CacheState::CSTATE_FIRST; state < CacheState::NUM_CSTATE_STATES; state = CacheState::cstate_t(int(state)+1)) {
      registerStatsMetric(name, core_id, String("loads-")+CStateString(state), &stats.loads_state[state]);
      registerStatsMetric(name, core_id, String("stores-")+CStateString(state), &stats.stores_state[state]);
      registerStatsMetric(name, core_id, String("load-misses-")+CStateString(state), &stats.load_misses_state[state]);
      registerStatsMetric(name, core_id, String("store-misses-")+CStateString(state), &stats.store_misses_state[state]);
      registerStatsMetric(name, core_id, String("evict-")+CStateString(state), &stats.evict[state]);
      registerStatsMetric(name, core_id, String("backinval-")+CStateString(state), &stats.backinval[state]);
   }
   if (mem_component == MemComponent::L1_ICACHE || mem_component == MemComponent::L1_DCACHE) {
      for(HitWhere::where_t hit_where = HitWhere::WHERE_FIRST; hit_where < HitWhere::NUM_HITWHERES; hit_where = HitWhere::where_t(int(hit_where)+1)) {
         const char * where_str = HitWhereString(hit_where);
         if (where_str[0] == '?') continue;
         registerStatsMetric(name, core_id, String("loads-where-")+where_str, &stats.loads_where[hit_where]);
         registerStatsMetric(name, core_id, String("stores-where-")+where_str, &stats.stores_where[hit_where]);
      }
   }
   registerStatsMetric(name, core_id, "coherency-downgrades", &stats.coherency_downgrades);
   registerStatsMetric(name, core_id, "coherency-upgrades", &stats.coherency_upgrades);
   registerStatsMetric(name, core_id, "coherency-writebacks", &stats.coherency_writebacks);
   registerStatsMetric(name, core_id, "coherency-invalidates", &stats.coherency_invalidates);
#ifdef ENABLE_TRANSITIONS
   for(CacheState::cstate_t old_state = CacheState::CSTATE_FIRST; old_state < CacheState::NUM_CSTATE_STATES; old_state = CacheState::cstate_t(int(old_state)+1))
      for(CacheState::cstate_t new_state = CacheState::CSTATE_FIRST; new_state < CacheState::NUM_CSTATE_STATES; new_state = CacheState::cstate_t(int(new_state)+1))
         registerStatsMetric(name, core_id, String("transitions-")+CStateString(old_state)+"-"+CStateString(new_state), &stats.transitions[old_state][new_state]);
   for(Transition::reason_t reason = Transition::REASON_FIRST; reason < Transition::NUM_REASONS; reason = Transition::reason_t(int(reason)+1))
      for(CacheState::cstate_t old_state = CacheState::CSTATE_FIRST; old_state < CacheState::NUM_CSTATE_SPECIAL_STATES; old_state = CacheState::cstate_t(int(old_state)+1))
         for(CacheState::cstate_t new_state = CacheState::CSTATE_FIRST; new_state < CacheState::NUM_CSTATE_SPECIAL_STATES; new_state = CacheState::cstate_t(int(new_state)+1))
            registerStatsMetric(name, core_id, String("transitions-")+ReasonString(reason)+"-"+CStateString(old_state)+"-"+CStateString(new_state), &stats.transition_reasons[reason][old_state][new_state]);
#endif
   if (is_last_level_cache)
   {
      m_shmem_perf_global = new ShmemPerf();
      m_shmem_perf_totaltime = SubsecondTime::Zero();
      m_shmem_perf_numrequests = 0;

      for(int i = 0; i < ShmemPerf::NUM_SHMEM_TIMES; ++i)
      {
         ShmemPerf::shmem_times_type_t reason = ShmemPerf::shmem_times_type_t(i);
         registerStatsMetric(name, core_id, String("uncore-time-")+ShmemReasonString(reason), &m_shmem_perf_global->getComponent(reason));
      }
      registerStatsMetric(name, core_id, "uncore-totaltime", &m_shmem_perf_totaltime);
      registerStatsMetric(name, core_id, "uncore-requests", &m_shmem_perf_numrequests);
   }

#ifdef KNAPSACK
   mytimer = 0;
   L1timer = 0;
   std::cout << m_mem_component << " " << (cache_params.num_sets * cache_params.associativity) << std::endl;
   knapsack.init(cache_params.num_sets * cache_params.associativity);
   L1_knapsack.init(cache_params.num_sets * cache_params.associativity);

   num_ideal_prefetch_hits_L1 = 0;
   num_ideal_prefetch_hits_L2 = 0;
#endif
   prefetch_inserted = 0;
   redundant_in_progress = 0;
   redundant_in_cache = 0;
   redundant_prefetch_buffer = 0;
}

CacheCntlr::~CacheCntlr()
{

    if (isMasterCache())
    {
        delete m_master;
    }
    delete m_shmem_perf;
    if (m_shmem_perf_global)
        delete m_shmem_perf_global;

#ifdef TRACK_LATENCY_BY_HITWHERE
    for(std::unordered_map<HitWhere::where_t, StatHist>::iterator it = lat_by_where.begin(); it != lat_by_where.end(); ++it) {
        printf("%2u-%s: ", m_core_id, HitWhereString(it->first));
        it->second.print();
    }
#endif
    if (if_last_level_cache) {
        printOnePhase();
    }
    /*
    ofstream OUTFILE1("pc_accesses.csv");
    for (std::map<IntPtr, UInt64>::iterator it = pc_accesses.begin(); it != pc_accesses.end(); it++) {
	    assert(pc_hits.find(it->first) != pc_hits.end());
	    if (it->second > 0)
	    {
		    OUTFILE1 << "PC: " << it->first << " prefetch_hits + misses: " << it->second << " prefetch_hits: " << pc_hits[it->first];
		    OUTFILE1 << " accuracy: " << (double) pc_hits[it->first]/ (double) it->second << std::endl;
	    }
    }
    std::cout<<std::endl;
    OUTFILE1.close();
    ofstream OUTFILE2("pc_prefetches.csv");
    for (std::map<IntPtr, UInt64>::iterator it = pc_prefetches.begin(); it != pc_prefetches.end(); it++) {
	    assert(pc_prefetch_hits.find(it->first) != pc_prefetch_hits.end());
	    if(it->second > 0)
	    {
		    OUTFILE2 << "PC: " << it->first << " prefetches: " << it->second << " prefetch_hits: " << pc_prefetch_hits[it->first];
		    OUTFILE2 << " accuracy: " << (double) pc_prefetch_hits[it->first]/ (double) it->second << std::endl;
	    }
    }
    OUTFILE2.close();
    */

#ifdef KNAPSACK 
    assert(0);
    std::cout << std::endl << "================================================================"<< std::endl << std::endl;
    std::cout << "Knapsack for " << m_mem_component << std::dec << " " << mytimer << std::endl;
    knapsack.print();
    double average_bias = 0;
    double total = 0;
    double pc_opt_distribution[11] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    for(std::map<IntPtr, UInt64>::iterator it=pc_opt_accesses.begin(); it != pc_opt_accesses.end(); it++)
    {
        assert(pc_opt_hits.find(it->first) != pc_opt_hits.end());
        double bias = (double)pc_opt_hits[it->first]/(double)it->second;
        unsigned int index = (unsigned int)(bias*10);
        assert(index <= 10);
        pc_opt_distribution[index] += it->second;

        if(bias < 0.5)
            bias = 1 - bias;
        average_bias += bias*(it->second);
        total += it->second;
    }
    std::cout << "PC OPT bias: " << (double)average_bias/(double)total << std::endl;
    std::cout << "PC OPT dsitribution: " << std::endl;
    for(unsigned int i=0; i<=10; i++)
        std::cout << 100*pc_opt_distribution[i]/total << ", ";
    std::cout << std::endl << std::endl;
    std::cout << "Ideal misses: " << knapsack.get_num_opt_misses() << std::endl;
    std::cout << "Ideal traffic: " << knapsack.get_num_opt_misses() + num_ideal_prefetch_hits_L2 << std::endl;
    std::cout << "Ideal Prefetch Hits: " << num_ideal_prefetch_hits_L2 << std::endl;
    std::cout << std::endl << std::endl;
    std::cout << std::endl << std::endl;

    //Stats about the knapsack with LLC's size but L1's access stream.
    if(L1_knapsack.access != 0)
    {
        std::cout << "Knapsack for " << m_mem_component << std::dec << " with the L1 access stream " << L1timer << std::endl;
        /******************* PC stats ********************/
        L1_knapsack.print();
        average_bias = 0;
        total = 0;
        double fixed = 0;
        double not_fixed = 0;
        double l1_pc_opt_distribution[11] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        for(std::map<IntPtr, UInt64>::iterator it=l1_pc_opt_accesses.begin(); it != l1_pc_opt_accesses.end(); it++)
        {
            assert(l1_pc_opt_hits.find(it->first) != l1_pc_opt_hits.end());
            double bias = (double)l1_pc_opt_hits[it->first]/(double)it->second;
            unsigned int index = (unsigned int)(bias*10);
            assert(index <= 10);
            l1_pc_opt_distribution[index] += it->second;

            if(bias < 0.5)
                bias = 1 - bias;
            average_bias += bias*(it->second);
            total += it->second;

            if(pc_opt_accesses.find(it->first) != pc_opt_accesses.end())
            {
                double miss_stream_bias = (double)pc_opt_hits[it->first]/(double)pc_opt_accesses[it->first];
                if(miss_stream_bias < 0.9)
                {
                    //            std::cout << std::hex << it->first << std::dec << " " << m_next_cache_cntlr->pc_opt_accesses[it->first] << " " << bias << " " << miss_stream_bias;
                    if(bias >= 0.9)
                    {
                        //                 std::cout << ":-)" << std::endl;
                        fixed += pc_opt_accesses[it->first];
                    }
                    else
                    {
                        //               std::cout << ":-(" << std::endl;
                        not_fixed += pc_opt_accesses[it->first];
                    }
                    //          if(miss_stream_bias > bias)
                    //             std::cout << "??????????????" << std::endl;

                }
            }
        }
        std::cout << "PC OPT bias: " << (double)average_bias/(double)total << std::endl;
        std::cout << "Fixed by L1 stream: " << fixed << " " << 100*fixed/(fixed + not_fixed) << std::endl;

        /******************* PCxADDR stats ********************/
        average_bias = 0;
        total = 0;
        for(std::map<std::pair<IntPtr, IntPtr>, UInt64>::iterator it=l1_pcxaddr_opt_accesses.begin(); it != l1_pcxaddr_opt_accesses.end(); it++)
        {
            assert(l1_pcxaddr_opt_hits.find(it->first) != l1_pcxaddr_opt_hits.end());
            double bias = (double)l1_pcxaddr_opt_hits[it->first]/(double)it->second;

            if(bias < 0.5)
                bias = 1 - bias;
            average_bias += bias*(it->second);
            total += it->second;
        }
        std::cout << "PCxAddr OPT bias: " << (double)average_bias/(double)total << std::endl;

        std::cout << std::endl << std::endl;
        std::cout << "Ideal misses: " << L1_knapsack.get_num_opt_misses() << std::endl;
        std::cout << "Ideal traffic: " << L1_knapsack.get_num_opt_misses() + num_ideal_prefetch_hits_L1 << std::endl;
        std::cout << "Ideal Prefetch Hits: " << num_ideal_prefetch_hits_L1 << std::endl;

        /******************* Prefetch Aware Knapsack stats ********************/
    }
    std::cout << std::endl << "============================= ==================================="<< std::endl << std::endl;
#endif
    std::cout << "Inserted: " << prefetch_inserted << std::endl;
    std::cout << "Redundant.progress: " << redundant_in_progress << std::endl;
    std::cout << "Redundant.cache: " << redundant_in_cache << std::endl;
    std::cout << "Redundant.buffer: " << redundant_prefetch_buffer << std::endl;
}

void
CacheCntlr::setPrevCacheCntlrs(CacheCntlrList& prev_cache_cntlrs)
{
   /* Append our prev_caches list to the master one (only master nodes) */
   for(CacheCntlrList::iterator it = prev_cache_cntlrs.begin(); it != prev_cache_cntlrs.end(); it++)
      if ((*it)->isMasterCache())
         m_master->m_prev_cache_cntlrs.push_back(*it);
   #ifdef ENABLE_TRACK_SHARING_PREVCACHES
   LOG_ASSERT_ERROR(m_master->m_prev_cache_cntlrs.size() <= MAX_NUM_PREVCACHES, "shared locations vector too small, increase MAX_NUM_PREVCACHES to at least %u", m_master->m_prev_cache_cntlrs.size());
   #endif
}

void
CacheCntlr::setDRAMDirectAccess(DramCntlrInterface* dram_cntlr, UInt64 num_outstanding)
{
   m_master->m_dram_cntlr = dram_cntlr;
   m_master->m_dram_outstanding_writebacks = new ContentionModel("llc-evict-queue", m_core_id, num_outstanding);
}


/*****************************************************************************
 * operations called by core on first-level cache
 *****************************************************************************/

HitWhere::where_t
CacheCntlr::processMemOpFromCore(
      Core::lock_signal_t lock_signal,
      Core::mem_op_t mem_op_type,
      IntPtr ca_address, UInt32 offset,
      Byte* data_buf, UInt32 data_length,
      bool modeled,
      bool count,
      IntPtr ca_pc)
{

   //std::cout << m_mem_component << " " << std::dec << mytimer << " " << std::hex << ca_address << std::dec << std::endl;
   //#ifdef KNAPSACK
   //#define L1_KNAPSACK
   #ifdef L1_KNAPSACK
   assert(0);
    bool decision = false;
   //if(ca_pc != 0 && (mem_op_type != Core::WRITE))
   if((mem_op_type != Core::WRITE))
   {
        IntPtr pc = ca_pc;
        IntPtr address = ca_address;
        mytimer++;
        address = ( address >> 6 ) << 6;
        UInt64 curr_quanta = mytimer/QUANTUM;
        //assert(pc);
        knapsack.add_access(address, curr_quanta, pc);
        if(addr_history.find(address) != addr_history.end())
        {
            int last_quanta = addr_history[address].last_quanta;
            assert(last_quanta != -1);
            decision = knapsack.should_cache(address, curr_quanta, last_quanta);
            IntPtr last_pc = addr_history[address].PC;
            if(pc_opt_accesses.find(last_pc) == pc_opt_accesses.end())
            {
                pc_opt_accesses[last_pc] = 0;
                pc_opt_hits[last_pc] = 0;   
            }

            pc_opt_accesses[last_pc]++;
            if(decision)
                pc_opt_hits[last_pc]++;
        }
        else
            addr_history[address].init(curr_quanta);

        addr_history[address].update(mytimer, curr_quanta, pc, decision);
   }
   #endif
#ifdef LEDGER
   if(count)
   {
      IntPtr pc = ca_pc;
      IntPtr address = ca_address;
      address = ( address >> 6 ) << 6;
      m_last_level = lastLevelCache();
      assert(m_last_level);
      m_last_level->m_master->m_cache->m_L1_ledger->inform_access(pc, address);
      EVENT last_ledger_event = m_last_level->m_master->m_cache->m_L1_ledger->get_last_event();

      if(last_ledger_event.valid)
      {
         assert(last_ledger_event.addr == address);
         m_last_level->m_master->m_cache->hawkeye_L1_predictor->train(last_ledger_event.pc, last_ledger_event.addr, last_ledger_event.decision);
      }

     //Update my own cache
      m_master->m_cache->m_ledger->inform_access(pc, address);
      last_ledger_event = m_master->m_cache->m_ledger->get_last_event();

      if(last_ledger_event.valid)
      {
         assert(last_ledger_event.addr == address);
         m_master->m_cache->hawkeye_predictor->train(last_ledger_event.pc, last_ledger_event.addr, last_ledger_event.decision);
      }

      //m_next_cache_cntlr->inform_L1_access(pc, address);
   }
#endif

   SubsecondTime t_start_p = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);
   if (lock_signal != Core::LOCK && modeled)
   {
      Prefetch(t_start_p);
   }
   HitWhere::where_t hit_where = HitWhere::MISS;

   // Protect against concurrent access from sibling SMT threads
   ScopedLock sl_smt(m_master->m_smt_lock);

   LOG_PRINT("processMemOpFromCore(), lock_signal(%u), mem_op_type(%u), ca_address(0x%x)",
             lock_signal, mem_op_type, ca_address);
MYLOG("----------------------------------------------");
MYLOG("%c%c %lx+%u..+%u", mem_op_type == Core::WRITE ? 'W' : 'R', mem_op_type == Core::READ_EX ? 'X' : ' ', ca_address, offset, data_length);
LOG_ASSERT_ERROR((ca_address & (getCacheBlockSize() - 1)) == 0, "address at cache line + %x", ca_address & (getCacheBlockSize() - 1));
LOG_ASSERT_ERROR(offset + data_length <= getCacheBlockSize(), "access until %u > %u", offset + data_length, getCacheBlockSize());

   #ifdef PRIVATE_L2_OPTIMIZATION
   /* if this is the second part of an atomic operation: we already have the lock, don't lock again */
   if (lock_signal != Core::UNLOCK)
      acquireLock(ca_address);
   #else
   /* if we'll need the next level (because we're a writethrough cache, and either this is a write
      or we're part of an atomic pair in which this or the other memop is potentially a write):
      make sure to lock it now, so the cache line in L2 doesn't fall from under us
      between operationPermissibleinCache and the writethrough */
   bool lock_all = m_cache_writethrough && ((mem_op_type == Core::WRITE) || (lock_signal != Core::NONE));

    /* if this is the second part of an atomic operation: we already have the lock, don't lock again */
   if (lock_signal != Core::UNLOCK) {
      if (lock_all)
         acquireStackLock(ca_address);
      else
         acquireLock(ca_address);
   }
   #endif

   SubsecondTime t_start = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);

   CacheBlockInfo *cache_block_info;
   bool cache_hit = operationPermissibleinCache(ca_address, mem_op_type, &cache_block_info), prefetch_hit = false;

   if (!cache_hit && m_perfect)
   {
      cache_hit = true;
      hit_where = HitWhere::where_t(m_mem_component);
      if (cache_block_info)
         cache_block_info->setCState(CacheState::MODIFIED);
      else
      {
         insertCacheBlock(ca_address, ca_pc, mem_op_type == Core::READ ? CacheState::SHARED : CacheState::MODIFIED, NULL, m_core_id, ShmemPerfModel::_USER_THREAD);
         cache_block_info = getCacheBlockInfo(ca_address);
      }
   }
   else if (cache_hit && m_passthrough)
   {
      cache_hit = false;
      cache_block_info->invalidate();
      cache_block_info = NULL;
   }

   if (count)
   {
      ScopedLock sl(getLock());
      // Update the Cache Counters
      getCache()->updateCounters(cache_hit);
      updateCounters(mem_op_type, ca_address, cache_hit, getCacheState(cache_block_info), Prefetch::NONE);
   }

   if (cache_hit)
   {
MYLOG("L1 hit");
      getMemoryManager()->incrElapsedTime(m_mem_component, CachePerfModel::ACCESS_CACHE_DATA_AND_TAGS, ShmemPerfModel::_USER_THREAD);
      hit_where = (HitWhere::where_t)m_mem_component;

      if (cache_block_info->hasOption(CacheBlockInfo::WARMUP) && Sim()->getInstrumentationMode() != InstMode::CACHE_ONLY)
      {
         stats.hits_warmup++;
         cache_block_info->clearOption(CacheBlockInfo::WARMUP);
      }
      if (cache_block_info->hasOption(CacheBlockInfo::PREFETCH))
      {
         // This line was fetched by the prefetcher and has proven useful
         stats.hits_prefetch++;
         prefetch_hit = true;
         cache_block_info->clearOption(CacheBlockInfo::PREFETCH);
      }

      if (modeled && m_l1_mshr)
      {
         ScopedLock sl(getLock());
         SubsecondTime t_now = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);
         SubsecondTime t_completed = m_master->m_l1_mshr.getTagCompletionTime(ca_address);
         if (t_completed != SubsecondTime::MaxTime() && t_completed > t_now)
         {
            if (mem_op_type == Core::WRITE)
               ++stats.store_overlapping_misses;
            else
               ++stats.load_overlapping_misses;

            SubsecondTime latency = t_completed - t_now;
            getShmemPerfModel()->incrElapsedTime(latency, ShmemPerfModel::_USER_THREAD);
         }
      }

      if (modeled)
      {
         ScopedLock sl(getLock());
         // This is a hit, but maybe the prefetcher filled it at a future time stamp. If so, delay.
         SubsecondTime t_now = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);
         if (m_master->mshr.count(ca_address)
            && (m_master->mshr[ca_address].t_issue < t_now && m_master->mshr[ca_address].t_complete > t_now))
         {
            SubsecondTime latency = m_master->mshr[ca_address].t_complete - t_now;
            stats.mshr_latency += latency;
            getMemoryManager()->incrElapsedTime(latency, ShmemPerfModel::_USER_THREAD);
         }
      }

   } else {
      /* cache miss: either wrong coherency state or not present in the cache */
MYLOG("L1 miss");
      if (!m_passthrough)
         getMemoryManager()->incrElapsedTime(m_mem_component, CachePerfModel::ACCESS_CACHE_TAGS, ShmemPerfModel::_USER_THREAD);

      SubsecondTime t_miss_begin = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);
      SubsecondTime t_mshr_avail = t_miss_begin;
      if (modeled && m_l1_mshr && !m_passthrough)
      {
         ScopedLock sl(getLock());
         t_mshr_avail = m_master->m_l1_mshr.getStartTime(t_miss_begin);
         LOG_ASSERT_ERROR(t_mshr_avail >= t_miss_begin, "t_mshr_avail < t_miss_begin");
         SubsecondTime mshr_latency = t_mshr_avail - t_miss_begin;
         // Delay until we have an empty slot in the MSHR
         getShmemPerfModel()->incrElapsedTime(mshr_latency, ShmemPerfModel::_USER_THREAD);
         stats.mshr_latency += mshr_latency;
      }

      if (lock_signal == Core::UNLOCK)
         LOG_PRINT_ERROR("Expected to find address(0x%x) in L1 Cache", ca_address);

      #ifdef PRIVATE_L2_OPTIMIZATION
      #else
      if (!lock_all)
         acquireStackLock(ca_address, true);
      #endif

      // Invalidate the cache block before passing the request to L2 Cache
      if (getCacheState(ca_address) != CacheState::INVALID)
      {
         invalidateCacheBlock(ca_address);
      }

MYLOG("processMemOpFromCore l%d before next", m_mem_component);
      hit_where = m_next_cache_cntlr->processShmemReqFromPrevCache(this, mem_op_type, ca_address, modeled, count, Prefetch::NONE, t_start, false, ca_pc);
      bool next_cache_hit = hit_where != HitWhere::MISS;
MYLOG("processMemOpFromCore l%d next hit = %d", m_mem_component, next_cache_hit);

      if (next_cache_hit) {

      } else {
         /* last level miss, a message has been sent. */

MYLOG("processMemOpFromCore l%d waiting for sent message", m_mem_component);
         #ifdef PRIVATE_L2_OPTIMIZATION
         releaseLock(ca_address);
         #else
         releaseStackLock(ca_address);
         #endif

         waitForNetworkThread();
MYLOG("processMemOpFromCore l%d postwakeup", m_mem_component);

         //acquireStackLock(ca_address);
         // Pass stack lock through from network thread

         wakeUpNetworkThread();
MYLOG("processMemOpFromCore l%d got message reply", m_mem_component);

         /* have the next cache levels fill themselves with the new data */
MYLOG("processMemOpFromCore l%d before next fill", m_mem_component);
         hit_where = m_next_cache_cntlr->processShmemReqFromPrevCache(this, mem_op_type, ca_address, false, false, Prefetch::NONE, t_start, true, ca_pc);
MYLOG("processMemOpFromCore l%d after next fill", m_mem_component);
         LOG_ASSERT_ERROR(hit_where != HitWhere::MISS,
            "Tried to read in next-level cache, but data is already gone");

         #ifdef PRIVATE_L2_OPTIMIZATION
         releaseStackLock(ca_address, true);
         #else
         #endif
      }


      /* data should now be in next-level cache, go get it */
      SubsecondTime t_now = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);
      copyDataFromNextLevel(mem_op_type, ca_address, ca_pc, modeled, t_now);

      cache_block_info = getCacheBlockInfo(ca_address);

      #ifdef PRIVATE_L2_OPTIMIZATION
      #else
      if (!lock_all)
         releaseStackLock(ca_address, true);
      #endif

      LOG_ASSERT_ERROR(operationPermissibleinCache(ca_address, mem_op_type),
         "Expected %x to be valid in L1", ca_address);


      if (modeled && m_l1_mshr && !m_passthrough)
      {
         SubsecondTime t_miss_end = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);
         ScopedLock sl(getLock());
         m_master->m_l1_mshr.getCompletionTime(t_miss_begin, t_miss_end - t_mshr_avail, ca_address);
      }
   }


   if (modeled && m_next_cache_cntlr && !m_perfect && Sim()->getConfig()->hasCacheEfficiencyCallbacks())
   {
      bool new_bits = cache_block_info->updateUsage(offset, data_length);
      if (new_bits)
      {
         m_next_cache_cntlr->updateUsageBits(ca_address, cache_block_info->getUsage());
      }
   }


   accessCache(mem_op_type, ca_address, offset, data_buf, data_length, hit_where == HitWhere::where_t(m_mem_component) && count, ca_pc);
MYLOG("access done");


   SubsecondTime t_now = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);
   SubsecondTime total_latency = t_now - t_start;

   // From here on downwards: not long anymore, only stats update so blanket cntrl lock
   {
      ScopedLock sl(getLock());

      if (! cache_hit && count) {
         stats.total_latency += total_latency;
      }

      #ifdef TRACK_LATENCY_BY_HITWHERE
      if (count)
         lat_by_where[hit_where].update(total_latency.getNS());
      #endif

      /* if this is the first part of an atomic operation: keep the lock(s) */
      #ifdef PRIVATE_L2_OPTIMIZATION
      if (lock_signal != Core::LOCK)
         releaseLock(ca_address);
      #else
      if (lock_signal != Core::LOCK) {
         if (lock_all)
            releaseStackLock(ca_address);
         else
            releaseLock(ca_address);
      }
      #endif

      if (mem_op_type == Core::WRITE)
         stats.stores_where[hit_where]++;
      else
         stats.loads_where[hit_where]++;
   }
   // Call Prefetch on next-level caches (but not for atomic instructions as that causes a locking mess)
   if (count && modeled && m_master->m_prefetcher[m_core_id] && mem_op_type != Core::WRITE)
   {
      trainPrefetcher(ca_pc, ca_address, cache_hit, prefetch_hit, t_start, m_core_id);
   }

   if (Sim()->getConfig()->getCacheEfficiencyCallbacks().notify_access_func)
      Sim()->getConfig()->getCacheEfficiencyCallbacks().call_notify_access(cache_block_info->getOwner(), mem_op_type, hit_where);

   MYLOG("returning %s, latency %lu ns", HitWhereString(hit_where), total_latency.getNS());
   return hit_where;
}


void
CacheCntlr::updateHits(Core::mem_op_t mem_op_type, UInt64 hits)
{
   ScopedLock sl(getLock());

   while(hits > 0)
   {
      getCache()->updateCounters(true);
      updateCounters(mem_op_type, 0, true, mem_op_type == Core::READ ? CacheState::SHARED : CacheState::MODIFIED, Prefetch::NONE);
      hits--;
   }
}


void
CacheCntlr::copyDataFromNextLevel(Core::mem_op_t mem_op_type, IntPtr address, IntPtr pc, bool modeled, SubsecondTime t_now)
{
   // TODO: what if it's already gone? someone else may invalitate it between the time it arrived an when we get here...
   LOG_ASSERT_ERROR(m_next_cache_cntlr->operationPermissibleinCache(address, mem_op_type),
      "Tried to read from next-level cache, but data is already gone");
MYLOG("copyDataFromNextLevel l%d", m_mem_component);

   Byte data_buf[m_next_cache_cntlr->getCacheBlockSize()];
   m_next_cache_cntlr->retrieveCacheBlock(address, data_buf, ShmemPerfModel::_USER_THREAD, false);

   CacheState::cstate_t cstate = m_next_cache_cntlr->getCacheState(address);

   // TODO: increment time? tag access on next level, also data access if this is not an upgrade

   if (modeled && !m_next_level_read_bandwidth.isInfinite())
   {
      SubsecondTime delay = m_next_level_read_bandwidth.getRoundedLatency(getCacheBlockSize() * 8);
      SubsecondTime t_done = m_master->m_next_level_read_bandwidth.getCompletionTime(t_now, delay);
      // Assume cache access time already contains transfer latency, increment time by contention delay only
      LOG_ASSERT_ERROR(t_done >= t_now + delay, "Did not expect next-level cache to be this fast");
      getMemoryManager()->incrElapsedTime(t_done - t_now - delay, ShmemPerfModel::_USER_THREAD);
   }

   SharedCacheBlockInfo* cache_block_info = getCacheBlockInfo(address);
   if (cache_block_info)
   {
      // Block already present (upgrade): don't insert, but update
      updateCacheBlock(address, cstate, Transition::UPGRADE, NULL, ShmemPerfModel::_SIM_THREAD);
      MYLOG("copyDataFromNextLevel l%d done (updated)", m_mem_component);
   }
   else
   {
      // Insert the Cache Block in our own cache
      insertCacheBlock(address, pc, cstate, data_buf, m_core_id, ShmemPerfModel::_USER_THREAD);
      MYLOG("copyDataFromNextLevel l%d done (inserted)", m_mem_component);
   }
}


void
CacheCntlr::trainPrefetcher(IntPtr pc, IntPtr address, bool cache_hit, bool prefetch_hit, SubsecondTime t_issue, core_id_t coreid)
{
   ScopedLock sl(getLock());

   // Always train the prefetcher
   std::vector<IntPtr> prefetchList = m_master->m_prefetcher[coreid]->getNextAddress(pc, address, cache_hit, m_core_id);
   //assert(prefetchList.size() <= 1);
    //std::cout << "Training at " << t_issue << " and gen " << prefetchList.size() << "  already had " << m_master->m_prefetch_list.size() << std::endl;

   // Only do prefetches on misses, or on hits to lines previously brought in by the prefetcher (if enabled)
   //if (!cache_hit || (m_prefetch_on_prefetch_hit && prefetch_hit))
    if(true)
   {
      //m_master->m_prefetch_list.clear();
      //m_master->m_prefetch_list_pc.clear();
      // Just talked to the next-level cache, wait a bit before we start to prefetch
      m_master->m_prefetch_next = t_issue + PREFETCH_INTERVAL;

      prefetch_inserted += prefetchList.size();
      for(std::vector<IntPtr>::iterator it = prefetchList.begin(); it != prefetchList.end(); ++it)
      {
         // Keep at most PREFETCH_MAX_QUEUE_LENGTH entries in the prefetch queue
         //if (m_master->m_prefetch_list.size() > PREFETCH_MAX_QUEUE_LENGTH)
          //  break;
         bool overlapping = m_master->mshr.count(*it) && m_master->mshr[*it].t_issue < t_issue && m_master->mshr[*it].t_complete > t_issue;
         if (!operationPermissibleinCache(*it, Core::READ))
         {
            m_master->m_prefetch_list.push_back(*it);
            m_master->m_prefetch_list_pc.push_back(pc);
            m_master->m_prefetch_core_list.push_back(coreid);
            //break;
         }
         else if(overlapping)
           redundant_in_progress++; 
         else
           redundant_in_cache++; 
      }
   }

   //std::cout << "Master now has " << m_master->m_prefetch_list.size() << std::endl;
}

void
CacheCntlr::Prefetch(SubsecondTime t_now)
{
   //if(m_master->m_prefetcher)
    //   std::cout << " Prefetch at " << t_now << ": " << m_master->m_prefetch_next << " " << m_master->m_prefetch_list.size() << std::endl;
   IntPtr address_to_prefetch = INVALID_ADDRESS;
   std::vector<IntPtr> prefetchList;
   std::vector<IntPtr> prefetchPcList;
   std::vector<core_id_t> coreList;
   std::vector<SubsecondTime> issue_time;

   {
      ScopedLock sl(getLock());

      while(!m_master->m_prefetch_list.empty())
      {
         if (m_master->m_prefetch_next <= t_now)
         {
            IntPtr address = m_master->m_prefetch_list.front();
            IntPtr pc = m_master->m_prefetch_list_pc.front();
            core_id_t coreid = m_master->m_prefetch_core_list.front();
            m_master->m_prefetch_list.pop_front();
            m_master->m_prefetch_list_pc.pop_front();
            m_master->m_prefetch_core_list.pop_front();
          //  std::cout << "Prefetch : " << std::hex << address << std::dec << " -- " << m_master->m_prefetch_list.size() << std::endl;

            // Check address again, maybe some other core already brought it into the cache
            bool overlapping = m_master->mshr.count(address) && m_master->mshr[address].t_issue < t_now && m_master->mshr[address].t_complete > t_now;
            if (!operationPermissibleinCache(address, Core::READ))
            {
               address_to_prefetch = address;
               // Do at most one prefetch now, save the rest for a future call
      //         break;
               prefetchList.push_back(address);
               prefetchPcList.push_back(pc);
               coreList.push_back(coreid);
               issue_time.push_back(m_master->m_prefetch_next); 
               if (address_to_prefetch != INVALID_ADDRESS)
                   atomic_add_subsecondtime(m_master->m_prefetch_next, PREFETCH_INTERVAL);
            }
            else if(overlapping)
               redundant_in_progress++; 
           else
               redundant_in_cache++; 
         }
         else
            break;
      }
   }
   //AJ: What about locks on prefetchList? 
   //AJ: Since this function can only be called from L1 controller, and doPrefetch only 
   //calls L2+ controller, we hope that Prefetch() will not be called again before all 
   //calls to doPrefetch() finish.
   for(unsigned int i=0; i<prefetchList.size(); i++)
   {
       // Check address again, maybe some other core already brought it into the cache
       bool overlapping = m_master->mshr.count(prefetchList[i]) && m_master->mshr[prefetchList[i]].t_issue < t_now && m_master->mshr[prefetchList[i]].t_complete > t_now;
       if (!operationPermissibleinCache(prefetchList[i], Core::READ))
           doPrefetch(prefetchList[i], prefetchPcList[i], issue_time[i], coreList[i]);
       else if(overlapping)
           redundant_in_progress++; 
       else
           redundant_in_cache++; 
   }


   /*if (address_to_prefetch != INVALID_ADDRESS)
   {
      doPrefetch(address_to_prefetch, m_master->m_prefetch_next);
      atomic_add_subsecondtime(m_master->m_prefetch_next, PREFETCH_INTERVAL);
   }*/
   // In case the next-level cache has a prefetcher, run it
   if (m_next_cache_cntlr)
      m_next_cache_cntlr->Prefetch(t_now);
}

void
CacheCntlr::doPrefetch(IntPtr prefetch_address, IntPtr prefetch_pc, SubsecondTime t_start, core_id_t coreid)
{
   IntPtr temp_prefetch_pc = prefetch_pc;
   // prefetch_pc = 1;
#ifdef KNAPSACK
    inform_prefetch(prefetch_pc, prefetch_address);
#endif
   if (pc_prefetches.find(temp_prefetch_pc) == pc_prefetches.end()) {
     pc_prefetches[temp_prefetch_pc] = 0;
     pc_prefetch_hits[temp_prefetch_pc] = 0;
   }
   //std::cout<<"debug!"<< temp_prefetch_pc << std::endl;
   pc_prefetches[temp_prefetch_pc]++;
   addr_prefetch_pc[prefetch_address] = temp_prefetch_pc;
   m_master->m_prefetcher[coreid]->returnPrefetchIssuedInfo(temp_prefetch_pc, prefetch_address);

   ++stats.prefetches;
   acquireStackLock(prefetch_address);
   MYLOG("prefetching %lx", prefetch_address);
   //std::cout << "Do Prefetch: " << std::hex << prefetch_address << " at " << std::dec << t_start << std::endl;
   SubsecondTime t_before = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);
   getShmemPerfModel()->setElapsedTime(ShmemPerfModel::_USER_THREAD, t_start); // Start the prefetch at the same time as the original miss
   HitWhere::where_t hit_where = processShmemReqFromPrevCache(this, Core::READ, prefetch_address, true, true, Prefetch::OWN, t_start, false, prefetch_pc);
   if (hit_where == HitWhere::MISS)
   {
      /* last level miss, a message has been sent. */
      releaseStackLock(prefetch_address);
      waitForNetworkThread();
      wakeUpNetworkThread();

      hit_where = processShmemReqFromPrevCache(this, Core::READ, prefetch_address, false, false, Prefetch::OWN, t_start, false, prefetch_pc);

      LOG_ASSERT_ERROR(hit_where != HitWhere::MISS, "Line was not there after prefetch");
   }

   getShmemPerfModel()->setElapsedTime(ShmemPerfModel::_USER_THREAD, t_before); // Ignore changes to time made by the prefetch call
   releaseStackLock(prefetch_address);
}


/*****************************************************************************
 * operations called by cache on next-level cache
 *****************************************************************************/

HitWhere::where_t
CacheCntlr::processShmemReqFromPrevCache(CacheCntlr* requester, Core::mem_op_t mem_op_type, IntPtr address, bool modeled, bool count, Prefetch::prefetch_type_t isPrefetch, SubsecondTime t_issue, bool have_write_lock, IntPtr ca_pc)
{
   #ifdef L2KNAPSACK
   assert(0);
   bool decision = false;
   //if(count && isPrefetch == Prefetch::NONE && ca_pc != 0 && (mem_op_type != Core::WRITE))
   if(count && isPrefetch == Prefetch::NONE && (mem_op_type != Core::WRITE))
   {
        IntPtr pc = ca_pc;
        mytimer++;
        address = ( address >> 6 ) << 6;
        UInt64 curr_quanta = mytimer/QUANTUM;
        //assert(pc);
        knapsack.add_access(address, curr_quanta, pc);
        if(addr_history.find(address) != addr_history.end())
        {
            int last_quanta = addr_history[address].last_quanta;
            int last_prefetch_quanta = L1_addr_history[address].last_prefetch_quanta;

            prefetch_hit = ((last_prefetch_quanta != -1) && (last_prefetch_quanta >= last_quanta));
            if(!m_prefetcher)
                assert(last_quanta != -1);
            decision = knapsack.should_cache(address, curr_quanta, last_quanta);
            IntPtr last_pc = addr_history[address].PC;
            if(pc_opt_accesses.find(last_pc) == pc_opt_accesses.end())
            {
                pc_opt_accesses[last_pc] = 0;
                pc_opt_hits[last_pc] = 0;   
            }
            pc_opt_accesses[last_pc]++;
            if(decision)
                pc_opt_hits[last_pc]++;
        }
        else
            addr_history[address].init(curr_quanta);
        
        addr_history[address].update(mytimer, curr_quanta, pc, decision);
   }
   #endif
   //#ifdef KNAPSACK
#ifdef LEDGER
   if(count && isPrefetch == Prefetch::NONE )
   {
      IntPtr pc = ca_pc;
      address = ( address >> 6 ) << 6;
      //inform_L2_access(pc, address);
      m_master->m_cache->m_ledger->inform_access(pc, address);

      EVENT last_ledger_event = m_master->m_cache->m_ledger->get_last_event();

      if(last_ledger_event.valid)
      {
         assert(last_ledger_event.addr == address);
         m_master->m_cache->hawkeye_predictor->train(last_ledger_event.pc, last_ledger_event.addr, last_ledger_event.decision);
      }

   }
   else if(count)
   {
      assert( isPrefetch != Prefetch::NONE );
      address = ( address >> 6 ) << 6;
      m_master->m_cache->m_ledger->inform_prefetch(ca_pc, address);
      //m_master->m_cache->m_ledger->clear_entry(address); //TODO:revise
      //std::cout << "Prefetch PC: " << ca_pc << std::endl;
   }
#endif
   //#endif
   #ifdef PRIVATE_L2_OPTIMIZATION
   bool have_write_lock_internal = have_write_lock;
   if (! have_write_lock && m_shared_cores > 1)
   {
      acquireStackLock(address, true);
      have_write_lock_internal = true;
   }
   #else
   bool have_write_lock_internal = true;
   #endif

   bool cache_hit = operationPermissibleinCache(address, mem_op_type), sibling_hit = false, prefetch_hit = false;
   bool first_hit = cache_hit;
   HitWhere::where_t hit_where = HitWhere::MISS;
   SharedCacheBlockInfo* cache_block_info = getCacheBlockInfo(address);

   if (!cache_hit && m_perfect)
   {
      cache_hit = true;
      hit_where = HitWhere::where_t(m_mem_component);
      if (cache_block_info)
         cache_block_info->setCState(CacheState::MODIFIED);
      else
         cache_block_info = insertCacheBlock(address, ca_pc, mem_op_type == Core::READ ? CacheState::SHARED : CacheState::MODIFIED, NULL, m_core_id, ShmemPerfModel::_USER_THREAD);
   }
   else if (cache_hit && m_passthrough && count)
   {
      // Passthrough == false: cache that always misses (except in the L1 fill path, detected by count==false, where it should return the data)
      cache_hit = first_hit = false;
      cache_block_info->invalidate();
      cache_block_info = NULL;
      LOG_ASSERT_ERROR(m_next_cache_cntlr != NULL, "Cannot do passthrough on an LLC");
   }

   if (count)
   {
      ScopedLock sl(getLock());
      if (isPrefetch == Prefetch::NONE)
         getCache()->updateCounters(cache_hit);
      updateCounters(mem_op_type, address, cache_hit, getCacheState(address), isPrefetch);
   }

    if (count && isPrefetch == Prefetch::NONE ) {
        total_demand_accesses++;
        if (total_demand_accesses%phase_length == 0) {
            printOnePhase();   
            clearOnePhase();
            current_phase++;
            if(m_master->m_prefetcher[requester->m_core_id])
                m_master->m_prefetcher[requester->m_core_id]->returnPhaseInfo(current_phase);
        }
    }
   

   if (cache_hit)
   {
      if (isPrefetch == Prefetch::NONE && cache_block_info->hasOption(CacheBlockInfo::PREFETCH))
      {
         // This line was fetched by the prefetcher and has proven useful
         if(pc_accesses.find(ca_pc) == pc_accesses.end()) {
	        pc_hits[ca_pc] = 0;
	        pc_accesses[ca_pc] = 0;
         }
         /*
         if (pc_prefetches.find(ca_pc) == pc_prefetches.end()) {
            pc_prefetches[ca_pc] = 1;
            pc_prefetch_hits[ca_pc] = 0;
         }
         */
	     //std::cout<<"PC: "<< ca_pc << " accesses: " << pc_accesses[ca_pc] << " pc_hits: " << pc_hits[ca_pc] << std::endl;
         pc_hits[ca_pc]++;
         pc_accesses[ca_pc]++;
         UInt64 prefetch_pc = addr_prefetch_pc[address];
         if (pc_prefetches.find(prefetch_pc) == pc_prefetches.end()) {
            pc_prefetches[prefetch_pc] = 1;
            pc_prefetch_hits[prefetch_pc] = 0;
         }
         // assert(pc_prefetches.find(prefetch_pc) != pc_prefetches.end());
         pc_prefetch_hits[prefetch_pc]++;
        if(m_master->m_prefetcher[requester->m_core_id])
            m_master->m_prefetcher[requester->m_core_id]->returnPrefetchHitInfo(prefetch_pc, address);
         
         
         stats.hits_prefetch++;
         prefetch_hit = true;
         cache_block_info->clearOption(CacheBlockInfo::PREFETCH);
      }

      if (cache_block_info->hasOption(CacheBlockInfo::WARMUP) && Sim()->getInstrumentationMode() != InstMode::CACHE_ONLY)
      {
         stats.hits_warmup++;
         cache_block_info->clearOption(CacheBlockInfo::WARMUP);
      }
      // Increment Shared Mem Perf model cycle counts
      /****** TODO: for the last-level cache, this is also done by the network thread when the message comes in.
                    we probably shouldn't do this twice */
      /* TODO: if we end up getting the data from a sibling cache, the access time might be only that
         of the previous-level cache, not our (longer) access time */
      if (modeled)
      {
         ScopedLock sl(getLock());
         // This is a hit, but maybe the prefetcher filled it at a future time stamp. If so, delay.
         SubsecondTime t_now = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);
         if (m_master->mshr.count(address)
            && (m_master->mshr[address].t_issue < t_now && m_master->mshr[address].t_complete > t_now))
         {
            SubsecondTime latency = m_master->mshr[address].t_complete - t_now;
            stats.mshr_latency += latency;
            getMemoryManager()->incrElapsedTime(latency, ShmemPerfModel::_USER_THREAD);
         }
         else
         {
            getMemoryManager()->incrElapsedTime(m_mem_component, CachePerfModel::ACCESS_CACHE_DATA_AND_TAGS, ShmemPerfModel::_USER_THREAD);
         }
      }

      if (mem_op_type != Core::READ) // write that hits
      {
         /* Invalidate/flush in previous levels */
         SubsecondTime latency = SubsecondTime::Zero();
         for(CacheCntlrList::iterator it = m_master->m_prev_cache_cntlrs.begin(); it != m_master->m_prev_cache_cntlrs.end(); it++)
         {
            if (*it != requester)
            {
               std::pair<SubsecondTime, bool> res = (*it)->updateCacheBlock(address, CacheState::INVALID, Transition::COHERENCY, NULL, ShmemPerfModel::_USER_THREAD);
               latency = getMax<SubsecondTime>(latency, res.first);
               sibling_hit |= res.second;
            }
         }
         MYLOG("add latency %s, sibling_hit(%u)", itostr(latency).c_str(), sibling_hit);
         getMemoryManager()->incrElapsedTime(latency, ShmemPerfModel::_USER_THREAD);
         atomic_add_subsecondtime(stats.snoop_latency, latency);
         #ifdef ENABLE_TRACK_SHARING_PREVCACHES
         assert(! cache_block_info->hasCachedLoc());
         #endif
      }
      else if (cache_block_info->getCState() == CacheState::MODIFIED) // reading MODIFIED data
      {
         MYLOG("reading MODIFIED data");
         /* Writeback in previous levels */
         SubsecondTime latency = SubsecondTime::Zero();
         for(CacheCntlrList::iterator it = m_master->m_prev_cache_cntlrs.begin(); it != m_master->m_prev_cache_cntlrs.end(); it++) {
            if (*it != requester) {
               std::pair<SubsecondTime, bool> res = (*it)->updateCacheBlock(address, CacheState::SHARED, Transition::COHERENCY, NULL, ShmemPerfModel::_USER_THREAD);
               latency = getMax<SubsecondTime>(latency, res.first);
               sibling_hit |= res.second;
            }
         }
         MYLOG("add latency %s, sibling_hit(%u)", itostr(latency).c_str(), sibling_hit);
         getMemoryManager()->incrElapsedTime(latency, ShmemPerfModel::_USER_THREAD);
         atomic_add_subsecondtime(stats.snoop_latency, latency);
      }
      else if (cache_block_info->getCState() == CacheState::EXCLUSIVE) // reading EXCLUSIVE data
      {
         MYLOG("reading EXCLUSIVE data");
         // will have shared state
         SubsecondTime latency = SubsecondTime::Zero();
         for(CacheCntlrList::iterator it = m_master->m_prev_cache_cntlrs.begin(); it != m_master->m_prev_cache_cntlrs.end(); it++) {
            if (*it != requester) {
               std::pair<SubsecondTime, bool> res = (*it)->updateCacheBlock(address, CacheState::SHARED, Transition::COHERENCY, NULL, ShmemPerfModel::_USER_THREAD);
               latency = getMax<SubsecondTime>(latency, res.first);
               sibling_hit |= res.second;
            }
         }

         MYLOG("add latency %s, sibling_hit(%u)", itostr(latency).c_str(), sibling_hit);
         getMemoryManager()->incrElapsedTime(latency, ShmemPerfModel::_USER_THREAD);
         atomic_add_subsecondtime(stats.snoop_latency, latency);
      }

      if (m_last_remote_hit_where != HitWhere::UNKNOWN)
      {
         // handleMsgFromDramDirectory just provided us with the data. Its source was left in m_last_remote_hit_where
         hit_where = m_last_remote_hit_where;
         m_last_remote_hit_where = HitWhere::UNKNOWN;
      }
      else
         hit_where = HitWhere::where_t(m_mem_component + (sibling_hit ? HitWhere::SIBLING : 0));

   }
   else // !cache_hit: either data is not here, or operation on data is not permitted
   {
      if (count) {
   //      std::cout << m_core_id << " " << requester->m_core_id << endl;
        if(m_master->m_prefetcher[requester->m_core_id]) {
     //    std::cout << m_master->m_prefetcher[m_core_id] << endl;
         m_master->m_prefetcher[requester->m_core_id]->returnCacheMissInfo(ca_pc, address);
        }
      }

      if (pc_accesses.find(ca_pc) == pc_accesses.end())
	  {
         pc_hits[ca_pc] = 0;
         pc_accesses[ca_pc] = 0;
      }
      pc_accesses[ca_pc]++;

      /*
      total_demand_accesses++;
      if (total_demand_accesses%phase_threshold == 0) {
          printOnePhase();   
          clearOnePhase();
          current_phase++;
          m_master->m_prefetcher[requester->m_core_id]->returnPhaseInfo(current_phase);
      }
      */

      // Increment shared mem perf model cycle counts
      if (modeled)
         getMemoryManager()->incrElapsedTime(m_mem_component, CachePerfModel::ACCESS_CACHE_TAGS, ShmemPerfModel::_USER_THREAD);

      if (cache_block_info && (cache_block_info->getCState() == CacheState::SHARED))
      {
         // Data is present, but still no cache_hit => this is a write on a SHARED block. Do Upgrade
         SubsecondTime latency = SubsecondTime::Zero();
         for(CacheCntlrList::iterator it = m_master->m_prev_cache_cntlrs.begin(); it != m_master->m_prev_cache_cntlrs.end(); it++)
            if (*it != requester)
               latency = getMax<SubsecondTime>(latency, (*it)->updateCacheBlock(address, CacheState::INVALID, Transition::UPGRADE, NULL, ShmemPerfModel::_USER_THREAD).first);
         getMemoryManager()->incrElapsedTime(latency, ShmemPerfModel::_USER_THREAD);
         atomic_add_subsecondtime(stats.snoop_latency, latency);
         #ifdef ENABLE_TRACK_SHARING_PREVCACHES
         assert(! cache_block_info->hasCachedLoc());
         #endif
      }

      if (m_next_cache_cntlr)
      {
         if (cache_block_info)
            invalidateCacheBlock(address);

         // let the next cache level handle it.
         hit_where = m_next_cache_cntlr->processShmemReqFromPrevCache(this, mem_op_type, address, modeled, count, isPrefetch == Prefetch::NONE ? Prefetch::NONE : Prefetch::OTHER, t_issue, have_write_lock_internal, ca_pc);
         if (hit_where != HitWhere::MISS)
         {
            cache_hit = true;
            /* get the data for ourselves */
            SubsecondTime t_now = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);
            copyDataFromNextLevel(mem_op_type, address, ca_pc, modeled, t_now);
            if (isPrefetch != Prefetch::NONE)
               getCacheBlockInfo(address)->setOption(CacheBlockInfo::PREFETCH);
         }
      }
      else // last-level cache
      {
         if (cache_block_info && cache_block_info->getCState() == CacheState::EXCLUSIVE)
         {
            // Data is present, but still no cache_hit => this is a write on a SHARED block. Do Upgrade
            SubsecondTime latency = SubsecondTime::Zero();
            for(CacheCntlrList::iterator it = m_master->m_prev_cache_cntlrs.begin(); it != m_master->m_prev_cache_cntlrs.end(); it++)
               if (*it != requester)
                  latency = getMax<SubsecondTime>(latency, (*it)->updateCacheBlock(address, CacheState::INVALID, Transition::UPGRADE, NULL, ShmemPerfModel::_USER_THREAD).first);
            getMemoryManager()->incrElapsedTime(latency, ShmemPerfModel::_USER_THREAD);
            atomic_add_subsecondtime(stats.snoop_latency, latency);
            #ifdef ENABLE_TRACK_SHARING_PREVCACHES
            assert(! cache_block_info->hasCachedLoc());
            #endif

            cache_hit = true;
            hit_where = HitWhere::where_t(m_mem_component);
            MYLOG("Silent upgrade from E -> M for address %lx", address);
            cache_block_info->setCState(CacheState::MODIFIED);
         }
         else if (m_master->m_dram_cntlr)
         {
            // Direct DRAM access
            cache_hit = true;
            if (cache_block_info)
            {
               // We already have the line: it must have been SHARED and this is a write (else there wouldn't have been a miss)
               // Upgrade silently
               cache_block_info->setCState(CacheState::MODIFIED);
               hit_where = HitWhere::where_t(m_mem_component);
            }
            else
            {
               m_shmem_perf->reset(getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD), m_core_id);

               Byte data_buf[getCacheBlockSize()];
               SubsecondTime latency;

               // Do the DRAM access and increment local time
               boost::tie<HitWhere::where_t, SubsecondTime>(hit_where, latency) = accessDRAM(Core::READ, address, isPrefetch != Prefetch::NONE, data_buf);
               getMemoryManager()->incrElapsedTime(latency, ShmemPerfModel::_USER_THREAD);

               // Insert the line. Be sure to use SHARED/MODIFIED as appropriate (upgrades are free anyway), we don't want to have to write back clean lines
               insertCacheBlock(address, ca_pc, mem_op_type == Core::READ ? CacheState::SHARED : CacheState::MODIFIED, data_buf, m_core_id, ShmemPerfModel::_USER_THREAD);
               if (isPrefetch != Prefetch::NONE)
                  getCacheBlockInfo(address)->setOption(CacheBlockInfo::PREFETCH);

               updateUncoreStatistics(hit_where, getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD));
            }
         }
         else
         {
            initiateDirectoryAccess(mem_op_type, address, isPrefetch != Prefetch::NONE, t_issue);
         }
      }
   }

    /*
    if (count && isPrefetch == Prefetch::NONE ) {
        total_demand_accesses++;
        if (total_demand_accesses%phase_length == 0) {
            printOnePhase();   
            clearOnePhase();
            current_phase++;
            m_master->m_prefetcher[requester->m_core_id]->returnPhaseInfo(current_phase);
        }
    }
    */

    /*
    if (count && !cache_hit ) {
        m_master->m_prefetcher[requester->m_core_id]->returnCacheMissInfo(address);
    }
    */

   if (cache_hit)
   {
      MYLOG("Yay, hit!!");
      Byte data_buf[getCacheBlockSize()];
      retrieveCacheBlock(address, data_buf, ShmemPerfModel::_USER_THREAD, first_hit && count, ca_pc);
      /* Store completion time so we can detect overlapping accesses */
      if (modeled && !first_hit && !m_passthrough)
      {
         ScopedLock sl(getLock());
         m_master->mshr[address] = make_mshr(t_issue, getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD));
         cleanupMshr();
      }
   }

   /*if(m_mem_component == 4 && count && modeled && isPrefetch == Prefetch::NONE)
   {
      std::cout << "0x" << hex << ca_pc << " 0x" << address;
      if(mem_op_type == Core::WRITE)
        std::cout << std::dec << " S" << std::endl;
      else
        std::cout << std::dec << " L" << std::endl;
   }*/

   if (count && modeled && m_master->m_prefetcher[requester->m_core_id] && isPrefetch == Prefetch::NONE && mem_op_type != Core::WRITE)
   {
        m_master->m_prefetcher[requester->m_core_id]->returnMemoryUtilization(bandwidth_usage);
        trainPrefetcher(ca_pc, address, cache_hit, prefetch_hit, t_issue, requester->m_core_id);

        SubsecondTime t_now = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD); 
        if(t_now.getNS() > time_cnt * 1000000)
        {
             bandwidth_usage =(double) ((stats.load_misses + stats.loads_prefetch) - (prev_load_misses + prev_loads_prefetch));///((double)(t_prev.getNS() - t_now.getNS()));  
             cout << time_cnt << "\t" << bandwidth_usage << endl;
             //t_prev = t_now;
             prev_load_misses = stats.load_misses;
             prev_loads_prefetch = stats.loads_prefetch;
             time_cnt++;
             //cout << t_now.getNS() << endl;
        }
   }

   #ifdef PRIVATE_L2_OPTIMIZATION
   if (have_write_lock_internal && !have_write_lock)
   {
      releaseStackLock(address, true);
   }
   #else
   #endif

   MYLOG("returning %s", HitWhereString(hit_where));
   return hit_where;
}

void
CacheCntlr::notifyPrevLevelInsert(core_id_t core_id, MemComponent::component_t mem_component, IntPtr address)
{
   #ifdef ENABLE_TRACK_SHARING_PREVCACHES
   SharedCacheBlockInfo* cache_block_info = getCacheBlockInfo(address);
   assert(cache_block_info);
   PrevCacheIndex idx = m_master->m_prev_cache_cntlrs.find(core_id, mem_component);
   cache_block_info->setCachedLoc(idx);
   #endif
}

void
CacheCntlr::notifyPrevLevelEvict(core_id_t core_id, MemComponent::component_t mem_component, IntPtr address)
{
MYLOG("@%lx", address);
   if (m_master->m_evicting_buf && address == m_master->m_evicting_address) {
MYLOG("here being evicted");
   } else {
      #ifdef ENABLE_TRACK_SHARING_PREVCACHES
      SharedCacheBlockInfo* cache_block_info = getCacheBlockInfo(address);
MYLOG("here in state %c", CStateString(getCacheState(address)));
      assert(cache_block_info);
      PrevCacheIndex idx = m_master->m_prev_cache_cntlrs.find(core_id, mem_component);
      cache_block_info->clearCachedLoc(idx);
      #endif
   }
}

void
CacheCntlr::updateUsageBits(IntPtr address, CacheBlockInfo::BitsUsedType used)
{
   bool new_bits;
   {
      ScopedLock sl(getLock());
      SharedCacheBlockInfo* cache_block_info = getCacheBlockInfo(address);
      new_bits = cache_block_info->updateUsage(used);
   }
   if (new_bits && m_next_cache_cntlr && !m_perfect)
   {
      m_next_cache_cntlr->updateUsageBits(address, used);
   }
}

void
CacheCntlr::walkUsageBits()
{
   if (!m_next_cache_cntlr && Sim()->getConfig()->hasCacheEfficiencyCallbacks())
   {
      for(UInt32 set_index = 0; set_index < m_master->m_cache->getNumSets(); ++set_index)
      {
         for(UInt32 way = 0; way < m_master->m_cache->getAssociativity(); ++way)
         {
            CacheBlockInfo *block_info = m_master->m_cache->peekBlock(set_index, way);
            if (block_info->isValid() && !block_info->hasOption(CacheBlockInfo::WARMUP))
            {
               Sim()->getConfig()->getCacheEfficiencyCallbacks().call_notify_evict(true, block_info->getOwner(), 0, block_info->getUsage(), getCacheBlockSize() >> CacheBlockInfo::BitsUsedOffset);
            }
         }
      }
   }
}

boost::tuple<HitWhere::where_t, SubsecondTime>
CacheCntlr::accessDRAM(Core::mem_op_t mem_op_type, IntPtr address, bool isPrefetch, Byte* data_buf)
{
   ScopedLock sl(getLock()); // DRAM is shared and owned by m_master

   SubsecondTime t_issue = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);
   SubsecondTime dram_latency;
   HitWhere::where_t hit_where;

   switch (mem_op_type)
   {
      case Core::READ:
         boost::tie(dram_latency, hit_where) = m_master->m_dram_cntlr->getDataFromDram(address, m_core_id_master, data_buf, t_issue, m_shmem_perf);
         break;

      case Core::READ_EX:
      case Core::WRITE:
         boost::tie(dram_latency, hit_where) = m_master->m_dram_cntlr->putDataToDram(address, m_core_id_master, data_buf, t_issue);
         break;

      default:
         LOG_PRINT_ERROR("Unsupported Mem Op Type(%u)", mem_op_type);
   }

   return boost::tuple<HitWhere::where_t, SubsecondTime>(hit_where, dram_latency);
}

void
CacheCntlr::initiateDirectoryAccess(Core::mem_op_t mem_op_type, IntPtr address, bool isPrefetch, SubsecondTime t_issue)
{
   bool exclusive = false;

   switch (mem_op_type)
   {
      case Core::READ:
         exclusive = false;
         break;

      case Core::READ_EX:
      case Core::WRITE:
         exclusive = true;
         break;

      default:
         LOG_PRINT_ERROR("Unsupported Mem Op Type(%u)", mem_op_type);
   }

   bool first = false;
   {
      ScopedLock sl(getLock());
      CacheDirectoryWaiter* request = new CacheDirectoryWaiter(exclusive, isPrefetch, this, t_issue);
      m_master->m_directory_waiters.enqueue(address, request);
      if (m_master->m_directory_waiters.size(address) == 1)
         first = true;
   }

   if (first)
   {
      m_shmem_perf->reset(getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD), m_core_id);

      /* We're the first one to request this address, send the message to the directory now */
      if (exclusive)
      {
         SharedCacheBlockInfo* cache_block_info = getCacheBlockInfo(address);
         if (cache_block_info && (cache_block_info->getCState() == CacheState::SHARED))
         {
            processUpgradeReqToDirectory(address, m_shmem_perf, ShmemPerfModel::_USER_THREAD);
         }
         else
         {
            processExReqToDirectory(address);
         }
      }
      else
      {
         processShReqToDirectory(address);
      }
   }
   else
   {
      // Someone else is busy with this cache line, they'll do everything for us
      MYLOG("%u previous waiters", m_master->m_directory_waiters.size(address));
   }
}

void
CacheCntlr::processExReqToDirectory(IntPtr address)
{
   // We need to send a request to the Dram Directory Cache
   MYLOG("EX REQ>%d @ %lx", getHome(address) ,address);

   CacheState::cstate_t cstate = getCacheState(address);

   LOG_ASSERT_ERROR (cstate != CacheState::SHARED, "ExReq for a Cacheblock in S, should be a UpgradeReq");
   assert((cstate == CacheState::INVALID));

   getMemoryManager()->sendMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::EX_REQ,
         MemComponent::LAST_LEVEL_CACHE, MemComponent::TAG_DIR,
         m_core_id_master /* requester */,
         getHome(address) /* receiver */,
         address,
         NULL, 0,
         HitWhere::UNKNOWN, m_shmem_perf, ShmemPerfModel::_USER_THREAD);
}

void
CacheCntlr::processUpgradeReqToDirectory(IntPtr address, ShmemPerf *perf, ShmemPerfModel::Thread_t thread_num)
{
   // We need to send a request to the Dram Directory Cache
   MYLOG("UPGR REQ @ %lx", address);

   CacheState::cstate_t cstate = getCacheState(address);
   assert(cstate == CacheState::SHARED);
   setCacheState(address, CacheState::SHARED_UPGRADING);

   getMemoryManager()->sendMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::UPGRADE_REQ,
         MemComponent::LAST_LEVEL_CACHE, MemComponent::TAG_DIR,
         m_core_id_master /* requester */,
         getHome(address) /* receiver */,
         address,
         NULL, 0,
         HitWhere::UNKNOWN, perf, thread_num);
}

void
CacheCntlr::processShReqToDirectory(IntPtr address)
{
MYLOG("SH REQ @ %lx", address);
   getMemoryManager()->sendMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::SH_REQ,
         MemComponent::LAST_LEVEL_CACHE, MemComponent::TAG_DIR,
         m_core_id_master /* requester */,
         getHome(address) /* receiver */,
         address,
         NULL, 0,
         HitWhere::UNKNOWN, m_shmem_perf, ShmemPerfModel::_USER_THREAD);
}






/*****************************************************************************
 * internal operations called by cache on itself
 *****************************************************************************/

bool
CacheCntlr::operationPermissibleinCache(
      IntPtr address, Core::mem_op_t mem_op_type, CacheBlockInfo **cache_block_info)
{
   CacheBlockInfo *block_info = getCacheBlockInfo(address);
   if (cache_block_info != NULL)
      *cache_block_info = block_info;

   bool cache_hit = false;
   CacheState::cstate_t cstate = getCacheState(block_info);

   switch (mem_op_type)
   {
      case Core::READ:
         cache_hit = CacheState(cstate).readable();
         break;

      case Core::READ_EX:
      case Core::WRITE:
         cache_hit = CacheState(cstate).writable();
         break;

      default:
         LOG_PRINT_ERROR("Unsupported mem_op_type: %u", mem_op_type);
         break;
   }

   MYLOG("address %lx state %c: permissible %d", address, CStateString(cstate), cache_hit);
   return cache_hit;
}


void
CacheCntlr::accessCache(
      Core::mem_op_t mem_op_type, IntPtr ca_address, UInt32 offset,
      Byte* data_buf, UInt32 data_length, bool update_replacement, IntPtr ca_pc)
{
   switch (mem_op_type)
   {
      case Core::READ:
      case Core::READ_EX:
         m_master->m_cache->accessSingleLine(ca_address + offset, Cache::LOAD, data_buf, data_length,
                                             getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD), update_replacement, ca_pc);
         break;

      case Core::WRITE:
         m_master->m_cache->accessSingleLine(ca_address + offset, Cache::STORE, data_buf, data_length,
                                             getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD), update_replacement, ca_pc);
         // Write-through cache - Write the next level cache also
         if (m_cache_writethrough) {
            LOG_ASSERT_ERROR(m_next_cache_cntlr, "Writethrough enabled on last-level cache !?");
MYLOG("writethrough start");
            m_next_cache_cntlr->writeCacheBlock(ca_address, offset, data_buf, data_length, ShmemPerfModel::_USER_THREAD);
MYLOG("writethrough done");
         }
         break;

      default:
         LOG_PRINT_ERROR("Unsupported Mem Op Type: %u", mem_op_type);
         break;
   }
}




/*****************************************************************************
 * localized cache block operations
 *****************************************************************************/

SharedCacheBlockInfo*
CacheCntlr::getCacheBlockInfo(IntPtr address)
{
   return (SharedCacheBlockInfo*) m_master->m_cache->peekSingleLine(address);
}

CacheState::cstate_t
CacheCntlr::getCacheState(IntPtr address)
{
   SharedCacheBlockInfo* cache_block_info = getCacheBlockInfo(address);
   return getCacheState(cache_block_info);
}

CacheState::cstate_t
CacheCntlr::getCacheState(CacheBlockInfo *cache_block_info)
{
   return (cache_block_info == NULL) ? CacheState::INVALID : cache_block_info->getCState();
}

SharedCacheBlockInfo*
CacheCntlr::setCacheState(IntPtr address, CacheState::cstate_t cstate)
{
   SharedCacheBlockInfo* cache_block_info = getCacheBlockInfo(address);
   cache_block_info->setCState(cstate);
   return cache_block_info;
}

void
CacheCntlr::invalidateCacheBlock(IntPtr address)
{
   __attribute__((unused)) CacheState::cstate_t old_cstate = getCacheState(address);
   assert(old_cstate != CacheState::MODIFIED);
   assert(old_cstate != CacheState::INVALID);

   m_master->m_cache->invalidateSingleLine(address);

   if (m_next_cache_cntlr)
      m_next_cache_cntlr->notifyPrevLevelEvict(m_core_id_master, m_mem_component, address);

   MYLOG("%lx %c > %c", address, CStateString(old_cstate), CStateString(getCacheState(address)));
}

void
CacheCntlr::retrieveCacheBlock(IntPtr address, Byte* data_buf, ShmemPerfModel::Thread_t thread_num, bool update_replacement, IntPtr pc)
{
   __attribute__((unused)) SharedCacheBlockInfo* cache_block_info = (SharedCacheBlockInfo*) m_master->m_cache->accessSingleLine(
      address, Cache::LOAD, data_buf, getCacheBlockSize(), getShmemPerfModel()->getElapsedTime(thread_num), update_replacement, pc);
   LOG_ASSERT_ERROR(cache_block_info != NULL, "Expected block to be there but it wasn't");
}


/*****************************************************************************
 * cache block operations that update the previous level(s)
 *****************************************************************************/

SharedCacheBlockInfo*
CacheCntlr::insertCacheBlock(IntPtr address, IntPtr pc, CacheState::cstate_t cstate, Byte* data_buf, core_id_t requester, ShmemPerfModel::Thread_t thread_num)
{
MYLOG("insertCacheBlock l%d @ %lx as %c (now %c)", m_mem_component, address, CStateString(cstate), CStateString(getCacheState(address)));
   bool eviction;
   IntPtr evict_address;
   SharedCacheBlockInfo evict_block_info;
   Byte evict_buf[getCacheBlockSize()];

   LOG_ASSERT_ERROR(getCacheState(address) == CacheState::INVALID, "we already have this line, can't add it again");
   assert(this);
   m_master->m_cache->insertSingleLine(address, data_buf,
         &eviction, &evict_address, &evict_block_info, evict_buf,
         getShmemPerfModel()->getElapsedTime(thread_num), pc, this);
   SharedCacheBlockInfo* cache_block_info = setCacheState(address, cstate);

   if (Sim()->getInstrumentationMode() == InstMode::CACHE_ONLY)
      cache_block_info->setOption(CacheBlockInfo::WARMUP);

   if (Sim()->getConfig()->hasCacheEfficiencyCallbacks())
      cache_block_info->setOwner(Sim()->getConfig()->getCacheEfficiencyCallbacks().call_get_owner(requester, address));

   if (m_next_cache_cntlr && !m_perfect)
      m_next_cache_cntlr->notifyPrevLevelInsert(m_core_id_master, m_mem_component, address);
MYLOG("insertCacheBlock l%d local done", m_mem_component);


   if (eviction)
   {
MYLOG("evicting @%lx", evict_address);

      if (
         !m_next_cache_cntlr // Track at LLC
         && !evict_block_info.hasOption(CacheBlockInfo::WARMUP) // Ignore blocks allocated during warmup (we don't track usage then)
         && Sim()->getConfig()->hasCacheEfficiencyCallbacks()
      )
      {
         Sim()->getConfig()->getCacheEfficiencyCallbacks().call_notify_evict(false, evict_block_info.getOwner(), cache_block_info->getOwner(), evict_block_info.getUsage(), getCacheBlockSize() >> CacheBlockInfo::BitsUsedOffset);
      }

      CacheState::cstate_t old_state = evict_block_info.getCState();
      MYLOG("evicting @%lx (state %c)", evict_address, CStateString(old_state));
      {
         ScopedLock sl(getLock());
         transition(
            evict_address,
            Transition::EVICT,
            old_state,
            CacheState::INVALID
         );

         ++stats.evict[old_state];
         // Line was prefetched, but is evicted without ever being used
         if (evict_block_info.hasOption(CacheBlockInfo::PREFETCH))
            ++stats.evict_prefetch;
         if (evict_block_info.hasOption(CacheBlockInfo::WARMUP))
            ++stats.evict_warmup;
      }

      /* TODO: this part looks a lot like updateCacheBlock's dirty case, but with the eviction buffer
         instead of an address, and with a message to the directory at the end. Merge? */

      LOG_PRINT("Eviction: addr(0x%x)", evict_address);
      //std::cout << std::hex << evict_address << std::endl;
      //if (! m_master->m_prev_cache_cntlrs.empty() ) {
      if (! m_master->m_prev_cache_cntlrs.empty() && old_state == CacheState::MODIFIED) {
         ScopedLock sl(getLock());
         /* propagate the update to the previous levels. they will write modified data back to our evict buffer when needed */
         m_master->m_evicting_address = evict_address;
         m_master->m_evicting_buf = evict_buf;
            //std::cout << "Inval Prev levels " << m_mem_component << std::endl;
         SubsecondTime latency = SubsecondTime::Zero();
         for(CacheCntlrList::iterator it = m_master->m_prev_cache_cntlrs.begin(); it != m_master->m_prev_cache_cntlrs.end(); it++)
            latency = getMax<SubsecondTime>(latency, (*it)->updateCacheBlock(evict_address, CacheState::INVALID, Transition::BACK_INVAL, NULL, thread_num).first);
         getMemoryManager()->incrElapsedTime(latency, thread_num);
         atomic_add_subsecondtime(stats.snoop_latency, latency);

         m_master->m_evicting_address = 0;
         m_master->m_evicting_buf = NULL;
      }

      /* now properly get rid of the evicted line */

      if (m_perfect)
      {
         // Nothing to do in this case
      }
      else if (!m_coherent)
      {
         // Don't notify the next level, it may have already evicted the line itself and won't like our notifyPrevLevelEvict
         // Make sure the line wasn't modified though (unless we're writethrough), else data would have been lost
         if (!m_cache_writethrough)
            LOG_ASSERT_ERROR(evict_block_info.getCState() != CacheState::MODIFIED, "Non-coherent cache is throwing away dirty data");
      }
      else if (m_next_cache_cntlr)
      {
         if (m_cache_writethrough) {
            /* If we're a write-through cache the new data is in the next level already */
         } else {
            /* Send dirty block to next level cache. Probably we have an evict/victim buffer to do that when we're idle, so ignore timing */
            if (evict_block_info.getCState() == CacheState::MODIFIED)
               m_next_cache_cntlr->writeCacheBlock(evict_address, 0, evict_buf, getCacheBlockSize(), thread_num);
         }
         m_next_cache_cntlr->notifyPrevLevelEvict(m_core_id_master, m_mem_component, evict_address);
      }
      else if (m_master->m_dram_cntlr)
      {
         if (evict_block_info.getCState() == CacheState::MODIFIED)
         {
            SubsecondTime t_now = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);

            if (m_master->m_dram_outstanding_writebacks)
            {
               ScopedLock sl(getLock());
               // Delay if all evict buffers are full
               SubsecondTime t_issue = m_master->m_dram_outstanding_writebacks->getStartTime(t_now);
               getMemoryManager()->incrElapsedTime(t_issue - t_now, ShmemPerfModel::_USER_THREAD);
            }

            // Access DRAM
            SubsecondTime dram_latency;
            HitWhere::where_t hit_where;
            boost::tie<HitWhere::where_t, SubsecondTime>(hit_where, dram_latency) = accessDRAM(Core::WRITE, evict_address, false, evict_buf);

            // Occupy evict buffer
            if (m_master->m_dram_outstanding_writebacks)
            {
               ScopedLock sl(getLock());
               m_master->m_dram_outstanding_writebacks->getCompletionTime(t_now, dram_latency);
            }
         }
      }
      else
      {
         /* Send dirty block to directory */
         UInt32 home_node_id = getHome(evict_address);
         if (evict_block_info.getCState() == CacheState::MODIFIED)
         {
            // Send back the data also
MYLOG("evict FLUSH %lx", evict_address);
            getMemoryManager()->sendMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::FLUSH_REP,
                  MemComponent::LAST_LEVEL_CACHE, MemComponent::TAG_DIR,
                  m_core_id /* requester */,
                  home_node_id /* receiver */,
                  evict_address,
                  evict_buf, getCacheBlockSize(),
                  HitWhere::UNKNOWN, NULL, thread_num);
         }
         else
         {
MYLOG("evict INV %lx", evict_address);
            LOG_ASSERT_ERROR(evict_block_info.getCState() == CacheState::SHARED || evict_block_info.getCState() == CacheState::EXCLUSIVE,
                  "evict_address(0x%x), evict_state(%u)",
                  evict_address, evict_block_info.getCState());
            getMemoryManager()->sendMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::INV_REP,
                  MemComponent::LAST_LEVEL_CACHE, MemComponent::TAG_DIR,
                  m_core_id /* requester */,
                  home_node_id /* receiver */,
                  evict_address,
                  NULL, 0,
                  HitWhere::UNKNOWN, NULL, thread_num);
         }
      }

      LOG_ASSERT_ERROR(getCacheState(evict_address) == CacheState::INVALID, "Evicted address did not become invalid, now in state %s", CStateString(getCacheState(evict_address)));
      MYLOG("insertCacheBlock l%d evict done", m_mem_component);
   }

   MYLOG("insertCacheBlock l%d end", m_mem_component);
   return cache_block_info;
}

//AJ: Coherency updates, no need for replacement state to be updated
std::pair<SubsecondTime, bool>
CacheCntlr::updateCacheBlock(IntPtr address, CacheState::cstate_t new_cstate, Transition::reason_t reason, Byte* out_buf, ShmemPerfModel::Thread_t thread_num)
{
   MYLOG("updateCacheBlock");
   LOG_ASSERT_ERROR(new_cstate < CacheState::NUM_CSTATE_STATES, "Invalid new cstate %u", new_cstate);

   /* first, propagate the update to the previous levels. they will write modified data back to us when needed */
   // TODO: performance fix: should only query those we know have the line (in cache_block_info->m_cached_locs)
   /* TODO: increment time (access tags) on previous-level caches, either all (for a snooping protocol that's not keeping track
              of cache_block_info->m_cached_locs) or selective (snoop filter / directory) */
   SubsecondTime latency = SubsecondTime::Zero();
   bool sibling_hit = false;

   if (! m_master->m_prev_cache_cntlrs.empty())
   {
      for(CacheCntlrList::iterator it = m_master->m_prev_cache_cntlrs.begin(); it != m_master->m_prev_cache_cntlrs.end(); it++) {
         std::pair<SubsecondTime, bool> res = (*it)->updateCacheBlock(
            address, new_cstate, reason == Transition::EVICT ? Transition::BACK_INVAL : reason, NULL, thread_num);
         // writeback_time is for the complete stack, so only model it at the last level, ignore latencies returned by previous ones
         //latency = getMax<SubsecondTime>(latency, res.first);
         sibling_hit |= res.second;
      }
   }

   SharedCacheBlockInfo* cache_block_info = getCacheBlockInfo(address);
   __attribute__((unused)) CacheState::cstate_t old_cstate = cache_block_info ? cache_block_info->getCState() : CacheState::INVALID;

   bool buf_written = false, is_writeback = false;

   if (!cache_block_info)
   {
      /* We don't have the block, nothing to do */
   }
   else if (new_cstate == cache_block_info->getCState() && out_buf)
   {
      // We already have the right state, nothing to do except writing our data
      // in the out_buf if it is passed
         // someone (presumably the directory interfacing code) is waiting to consume the data
      retrieveCacheBlock(address, out_buf, thread_num, false);
      buf_written = true;
      is_writeback = true;
      sibling_hit = true;
   }
   else
   {
      {
         ScopedLock sl(getLock());
         transition(
            address,
            reason,
            getCacheState(address),
            new_cstate
         );
         if (reason == Transition::COHERENCY)
         {
            if (new_cstate == CacheState::SHARED)
               ++stats.coherency_downgrades;
            else if (cache_block_info->getCState() == CacheState::MODIFIED)
               ++stats.coherency_writebacks;
            else
               ++stats.coherency_invalidates;
            if (cache_block_info->hasOption(CacheBlockInfo::PREFETCH) && new_cstate == CacheState::INVALID)
               ++stats.invalidate_prefetch;
            if (cache_block_info->hasOption(CacheBlockInfo::WARMUP) && new_cstate == CacheState::INVALID)
               ++stats.invalidate_warmup;
         }
         if (reason == Transition::UPGRADE)
         {
            ++stats.coherency_upgrades;
         }
         else if (reason == Transition::BACK_INVAL)
         {
            ++stats.backinval[cache_block_info->getCState()];
         }
      }

      if (cache_block_info->getCState() == CacheState::MODIFIED) {
         /* data is modified, write it back */

         if (m_cache_writethrough) {
            /* next level already has the data */

         } else if (m_next_cache_cntlr) {
            /* write straight into the next level cache */
            Byte data_buf[getCacheBlockSize()];
            retrieveCacheBlock(address, data_buf, thread_num, false);
            m_next_cache_cntlr->writeCacheBlock(address, 0, data_buf, getCacheBlockSize(), thread_num);
            is_writeback = true;
            sibling_hit = true;

         } else if (out_buf) {
            /* someone (presumably the directory interfacing code) is waiting to consume the data */
            retrieveCacheBlock(address, out_buf, thread_num, false);
            buf_written = true;
            is_writeback = true;
            sibling_hit = true;

         } else {
            /* no-one will take my data !? */
            LOG_ASSERT_ERROR( cache_block_info->getCState() != CacheState::MODIFIED, "MODIFIED data is about to get lost!");

         }
         cache_block_info->setCState(CacheState::SHARED);
      }

      if (new_cstate == CacheState::INVALID)
      {
         if (out_buf)
         {
            retrieveCacheBlock(address, out_buf, thread_num, false);
            buf_written = true;
            is_writeback = true;
            sibling_hit = true;
         }
         if (m_coherent)
            invalidateCacheBlock(address);
      }
      else if (new_cstate == CacheState::SHARED)
      {
         if (out_buf)
         {
            retrieveCacheBlock(address, out_buf, thread_num, false);
            buf_written = true;
            is_writeback = true;
            sibling_hit = true;
         }

         cache_block_info->setCState(new_cstate);
      }
      else if (new_cstate == CacheState::MODIFIED)
      {
         cache_block_info->setCState(new_cstate);
      }
      else
      {
         LOG_ASSERT_ERROR(false, "Cannot change block status to %c", CStateString(new_cstate));
      }
   }

   MYLOG("@%lx  %c > %c (req: %c)", address, CStateString(old_cstate),
                                     CStateString(cache_block_info ? cache_block_info->getCState() : CacheState::INVALID),
                                     CStateString(new_cstate));

   LOG_ASSERT_ERROR((getCacheState(address) == CacheState::INVALID) || (getCacheState(address) == new_cstate) || !m_coherent,
         "state didn't change as we wanted: %c instead of %c", CStateString(getCacheState(address)), CStateString(new_cstate));

   CacheState::cstate_t current_cstate;
   current_cstate = (cache_block_info) ? cache_block_info->getCState(): CacheState::INVALID;
   LOG_ASSERT_ERROR((current_cstate == CacheState::INVALID) || (current_cstate == new_cstate) || !m_coherent,
         "state didn't change as we wanted: %c instead of %c", CStateString(current_cstate), CStateString(new_cstate));
   if (out_buf && !buf_written)
   {
      MYLOG("cache_block_info: %c", cache_block_info ? 'y' : 'n');
      MYLOG("@%lx  %c > %c (req: %c)", address, CStateString(old_cstate),
                                           CStateString(cache_block_info ? cache_block_info->getCState() : CacheState::INVALID),
                                           CStateString(new_cstate));
   }
   LOG_ASSERT_ERROR(out_buf ? buf_written : true, "out_buf passed in but never written to");
   /* Assume tag access caused by snooping is already accounted for in lower level cache access time,
      so only when we accessed data should we return any latency */
   if (is_writeback)
      latency += m_writeback_time.getLatency();
   return std::pair<SubsecondTime, bool>(latency, sibling_hit);
}

//Writebacks: Don't update the cache
void
CacheCntlr::writeCacheBlock(IntPtr address, UInt32 offset, Byte* data_buf, UInt32 data_length, ShmemPerfModel::Thread_t thread_num)
{
MYLOG(" ");

   // TODO: should we update access counter?

   if (m_master->m_evicting_buf && (address == m_master->m_evicting_address)) {
      MYLOG("writing to evict buffer %lx", address);
assert(offset==0);
assert(data_length==getCacheBlockSize());
      if (data_buf)
         memcpy(m_master->m_evicting_buf + offset, data_buf, data_length);
   } else {
      __attribute__((unused)) SharedCacheBlockInfo* cache_block_info = (SharedCacheBlockInfo*) m_master->m_cache->accessSingleLine(
         address + offset, Cache::STORE, data_buf, data_length, getShmemPerfModel()->getElapsedTime(thread_num), false, 0);
      LOG_ASSERT_ERROR(cache_block_info, "writethrough expected a hit at next-level cache but got miss");
      LOG_ASSERT_ERROR(cache_block_info->getCState() == CacheState::MODIFIED, "Got writeback for non-MODIFIED line");
   }

   if (m_cache_writethrough) {
      acquireStackLock(true);
      m_next_cache_cntlr->writeCacheBlock(address, offset, data_buf, data_length, thread_num);
      releaseStackLock(true);
   }
}

bool
CacheCntlr::isInLowerLevelCache(CacheBlockInfo *block_info)
{
   IntPtr address = m_master->m_cache->tagToAddress(block_info->getTag());
   for(CacheCntlrList::iterator it = m_master->m_prev_cache_cntlrs.begin(); it != m_master->m_prev_cache_cntlrs.end(); it++)
   {
      // All caches are inclusive, so there is no need to propagate further
      SharedCacheBlockInfo* block_info = (*it)->getCacheBlockInfo(address);
      if (block_info != NULL)
         return true;
   }
   return false;
}

void
CacheCntlr::incrementQBSLookupCost()
{
   SubsecondTime latency = m_writeback_time.getLatency();
   getMemoryManager()->incrElapsedTime(latency, ShmemPerfModel::_USER_THREAD);
   atomic_add_subsecondtime(stats.qbs_query_latency, latency);
}


/*****************************************************************************
 * handle messages from directory (in network thread)
 *****************************************************************************/

void
CacheCntlr::handleMsgFromDramDirectory(
      core_id_t sender, PrL1PrL2DramDirectoryMSI::ShmemMsg* shmem_msg)
{
   PrL1PrL2DramDirectoryMSI::ShmemMsg::msg_t shmem_msg_type = shmem_msg->getMsgType();
   IntPtr address = shmem_msg->getAddress();
   core_id_t requester = INVALID_CORE_ID;
   if ((shmem_msg_type == PrL1PrL2DramDirectoryMSI::ShmemMsg::EX_REP) || (shmem_msg_type == PrL1PrL2DramDirectoryMSI::ShmemMsg::SH_REP)
         || (shmem_msg_type == PrL1PrL2DramDirectoryMSI::ShmemMsg::UPGRADE_REP) )
   {
      ScopedLock sl(getLock()); // Keep lock when handling m_directory_waiters
      CacheDirectoryWaiter* request = m_master->m_directory_waiters.front(address);
      requester = request->cache_cntlr->m_core_id;
   }

   acquireStackLock(address);
MYLOG("begin");

   switch (shmem_msg_type)
   {
      case PrL1PrL2DramDirectoryMSI::ShmemMsg::EX_REP:
MYLOG("EX REP<%u @ %lx", sender, address);
         processExRepFromDramDirectory(sender, requester, shmem_msg);
         break;
      case PrL1PrL2DramDirectoryMSI::ShmemMsg::SH_REP:
MYLOG("SH REP<%u @ %lx", sender, address);
         processShRepFromDramDirectory(sender, requester, shmem_msg);
         break;
      case PrL1PrL2DramDirectoryMSI::ShmemMsg::UPGRADE_REP:
MYLOG("UPGR REP<%u @ %lx", sender, address);
         processUpgradeRepFromDramDirectory(sender, requester, shmem_msg);
         break;
      case PrL1PrL2DramDirectoryMSI::ShmemMsg::INV_REQ:
MYLOG("INV REQ<%u @ %lx", sender, address);
         processInvReqFromDramDirectory(sender, shmem_msg);
         break;
      case PrL1PrL2DramDirectoryMSI::ShmemMsg::FLUSH_REQ:
MYLOG("FLUSH REQ<%u @ %lx", sender, address);
         processFlushReqFromDramDirectory(sender, shmem_msg);
         break;
      case PrL1PrL2DramDirectoryMSI::ShmemMsg::WB_REQ:
MYLOG("WB REQ<%u @ %lx", sender, address);
         processWbReqFromDramDirectory(sender, shmem_msg);
         break;
      default:
         LOG_PRINT_ERROR("Unrecognized msg type: %u", shmem_msg_type);
         break;
   }

   if ((shmem_msg_type == PrL1PrL2DramDirectoryMSI::ShmemMsg::EX_REP) || (shmem_msg_type == PrL1PrL2DramDirectoryMSI::ShmemMsg::SH_REP)
         || (shmem_msg_type == PrL1PrL2DramDirectoryMSI::ShmemMsg::UPGRADE_REP) )
   {
      getLock().acquire(); // Keep lock when handling m_directory_waiters
      while(! m_master->m_directory_waiters.empty(address)) {
         CacheDirectoryWaiter* request = m_master->m_directory_waiters.front(address);
         getLock().release();

         request->cache_cntlr->m_shmem_perf->updateTime(getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD), ShmemPerf::PENDING_HIT);

         if (request->exclusive && (getCacheState(address) == CacheState::SHARED))
         {
            MYLOG("have SHARED, upgrading to MODIFIED for #%u", request->cache_cntlr->m_core_id);

            // We (the master cache) are sending the upgrade request in place of request->cache_cntlr,
            // so use their ShmemPerf* rather than ours
            processUpgradeReqToDirectory(address, request->cache_cntlr->m_shmem_perf, ShmemPerfModel::_SIM_THREAD);

            releaseStackLock(address);
            return;
         }

         if (request->isPrefetch)
            getCacheBlockInfo(address)->setOption(CacheBlockInfo::PREFETCH);

         // Set the Counters in the Shmem Perf model accordingly
         // Set the counter value in the USER thread to that in the SIM thread
         SubsecondTime t_core = request->cache_cntlr->getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD),
                       t_here = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD);
         if (t_here > t_core) {
MYLOG("adjusting time in #%u from %lu to %lu", request->cache_cntlr->m_core_id, t_core.getNS(), t_here.getNS());
            /* Unless the requesting core is already ahead of us (it may very well be if this cache's master thread's cpu
               is falling behind), update its time */
            // TODO: update master thread time in initiateDirectoryAccess ?
            request->cache_cntlr->getShmemPerfModel()->setElapsedTime(ShmemPerfModel::_USER_THREAD, t_here);
         }

MYLOG("wakeup user #%u", request->cache_cntlr->m_core_id);
         request->cache_cntlr->updateUncoreStatistics(shmem_msg->getWhere(), t_here);

         //releaseStackLock(address);
         // Pass stack lock through to user thread
         wakeUpUserThread(request->cache_cntlr->m_user_thread_sem);
         waitForUserThread(request->cache_cntlr->m_network_thread_sem);
         acquireStackLock(address);

         {
            ScopedLock sl(request->cache_cntlr->getLock());
            request->cache_cntlr->m_master->mshr[address] = make_mshr(request->t_issue, getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD));
            cleanupMshr();
         }

         getLock().acquire();
         MYLOG("about to dequeue request (%p) for address %lx", m_master->m_directory_waiters.front(address), address );
         m_master->m_directory_waiters.dequeue(address);
         delete request;
      }
      getLock().release();
MYLOG("woke up all");
   }

   releaseStackLock(address);

MYLOG("done");
}

void
CacheCntlr::processExRepFromDramDirectory(core_id_t sender, core_id_t requester, PrL1PrL2DramDirectoryMSI::ShmemMsg* shmem_msg)
{
   // Forward data from message to LLC, don't incur LLC data access time (writeback will be done asynchronously)
   //getMemoryManager()->incrElapsedTime(m_mem_component, CachePerfModel::ACCESS_CACHE_DATA_AND_TAGS);
MYLOG("processExRepFromDramDirectory l%d", m_mem_component);

   IntPtr address = shmem_msg->getAddress();
   Byte* data_buf = shmem_msg->getDataBuf();

   insertCacheBlock(address, 0, CacheState::EXCLUSIVE, data_buf, requester, ShmemPerfModel::_SIM_THREAD);
MYLOG("processExRepFromDramDirectory l%d end", m_mem_component);
}

void
CacheCntlr::processShRepFromDramDirectory(core_id_t sender, core_id_t requester, PrL1PrL2DramDirectoryMSI::ShmemMsg* shmem_msg)
{
   // Forward data from message to LLC, don't incur LLC data access time (writeback will be done asynchronously)
   //getMemoryManager()->incrElapsedTime(m_mem_component, CachePerfModel::ACCESS_CACHE_DATA_AND_TAGS);
MYLOG("processShRepFromDramDirectory l%d", m_mem_component);

   IntPtr address = shmem_msg->getAddress();
   Byte* data_buf = shmem_msg->getDataBuf();

   // Insert Cache Block in L2 Cache
   insertCacheBlock(address, 0, CacheState::SHARED, data_buf, requester, ShmemPerfModel::_SIM_THREAD);
}

void
CacheCntlr::processUpgradeRepFromDramDirectory(core_id_t sender, core_id_t requester, PrL1PrL2DramDirectoryMSI::ShmemMsg* shmem_msg)
{
MYLOG("processShRepFromDramDirectory l%d", m_mem_component);
   // We now have the only copy. Change to a writeable state.
   IntPtr address = shmem_msg->getAddress();
   CacheState::cstate_t cstate = getCacheState(address);

   if (cstate == CacheState::INVALID)
   {
      // I lost my copy because a concurrent UPGRADE REQ had INVed it, because the state
      // was Modified  when this request was processed, the data should be in the message
      // because it was FLUSHed (see dram_directory_cntlr.cc, MODIFIED case of the upgrade req)
      Byte* data_buf = shmem_msg->getDataBuf();
      LOG_ASSERT_ERROR(data_buf, "Trying to upgrade a block that is now INV and no data in the shmem_msg");

      updateCacheBlock(address, CacheState::MODIFIED, Transition::UPGRADE, data_buf, ShmemPerfModel::_SIM_THREAD);
   }
   else if  (cstate == CacheState::SHARED_UPGRADING)
   {
      // Last-Level Cache received a upgrade REP, but multiple private lower-level caches might
      // still have a shared copy. Should invalidate all except the ones from the core that initiated
      // the upgrade request (sender).

      updateCacheBlock(address, CacheState::MODIFIED, Transition::UPGRADE, NULL, ShmemPerfModel::_SIM_THREAD);
   }
   else
   {
      LOG_PRINT_ERROR("Trying to upgrade a block that is not SHARED_UPGRADING but %c (%lx)",CStateString(cstate), address);
   }
}

void
CacheCntlr::processInvReqFromDramDirectory(core_id_t sender, PrL1PrL2DramDirectoryMSI::ShmemMsg* shmem_msg)
{
   IntPtr address = shmem_msg->getAddress();
MYLOG("processInvReqFromDramDirectory l%d", m_mem_component);

   CacheState::cstate_t cstate = getCacheState(address);
   if (cstate != CacheState::INVALID)
   {
      if (cstate != CacheState::SHARED)
      {
        MYLOG("invalidating something else than SHARED: %c ", CStateString(cstate));
      }
      //assert(cstate == CacheState::SHARED);
      shmem_msg->getPerf()->updateTime(getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD));

      // Update Shared Mem perf counters for access to L2 Cache
      getMemoryManager()->incrElapsedTime(m_mem_component, CachePerfModel::ACCESS_CACHE_TAGS, ShmemPerfModel::_SIM_THREAD);

      updateCacheBlock(address, CacheState::INVALID, Transition::COHERENCY, NULL, ShmemPerfModel::_SIM_THREAD);

      shmem_msg->getPerf()->updateTime(getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD), ShmemPerf::REMOTE_CACHE_INV);

      getMemoryManager()->sendMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::INV_REP,
            MemComponent::LAST_LEVEL_CACHE, MemComponent::TAG_DIR,
            shmem_msg->getRequester() /* requester */,
            sender /* receiver */,
            address,
            NULL, 0,
            HitWhere::UNKNOWN, shmem_msg->getPerf(), ShmemPerfModel::_SIM_THREAD);
   }
   else
   {
      // Update Shared Mem perf counters for access to L2 Cache
      getMemoryManager()->incrElapsedTime(m_mem_component, CachePerfModel::ACCESS_CACHE_TAGS, ShmemPerfModel::_SIM_THREAD);
MYLOG("invalid @ %lx, hoping eviction message is underway", address);
   }
}

void
CacheCntlr::processFlushReqFromDramDirectory(core_id_t sender, PrL1PrL2DramDirectoryMSI::ShmemMsg* shmem_msg)
{
   IntPtr address = shmem_msg->getAddress();
MYLOG("processFlushReqFromDramDirectory l%d", m_mem_component);

   CacheState::cstate_t cstate = getCacheState(address);
   if (cstate != CacheState::INVALID)
   {
      //assert(cstate == CacheState::MODIFIED);
      shmem_msg->getPerf()->updateTime(getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD));

      // Update Shared Mem perf counters for access to L2 Cache
      getMemoryManager()->incrElapsedTime(m_mem_component, CachePerfModel::ACCESS_CACHE_DATA_AND_TAGS, ShmemPerfModel::_SIM_THREAD);

      // Flush the line
      Byte data_buf[getCacheBlockSize()];
      updateCacheBlock(address, CacheState::INVALID, Transition::COHERENCY, data_buf, ShmemPerfModel::_SIM_THREAD);

      shmem_msg->getPerf()->updateTime(getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD), ShmemPerf::REMOTE_CACHE_WB);

      getMemoryManager()->sendMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::FLUSH_REP,
            MemComponent::LAST_LEVEL_CACHE, MemComponent::TAG_DIR,
            shmem_msg->getRequester() /* requester */,
            sender /* receiver */,
            address,
            data_buf, getCacheBlockSize(),
            HitWhere::UNKNOWN, shmem_msg->getPerf(), ShmemPerfModel::_SIM_THREAD);
   }
   else
   {
      // Update Shared Mem perf counters for access to L2 Cache
      getMemoryManager()->incrElapsedTime(m_mem_component, CachePerfModel::ACCESS_CACHE_TAGS, ShmemPerfModel::_SIM_THREAD);
MYLOG("invalid @ %lx, hoping eviction message is underway", address);
   }
}

void
CacheCntlr::processWbReqFromDramDirectory(core_id_t sender, PrL1PrL2DramDirectoryMSI::ShmemMsg* shmem_msg)
{
   IntPtr address = shmem_msg->getAddress();
MYLOG("processWbReqFromDramDirectory l%d", m_mem_component);

   CacheState::cstate_t cstate = getCacheState(address);
   if (cstate != CacheState::INVALID)
   {
      //assert(cstate == CacheState::MODIFIED);
      shmem_msg->getPerf()->updateTime(getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD));

      // Update Shared Mem perf counters for access to L2 Cache
      getMemoryManager()->incrElapsedTime(m_mem_component, CachePerfModel::ACCESS_CACHE_DATA_AND_TAGS, ShmemPerfModel::_SIM_THREAD);

      // Write-Back the line
      Byte data_buf[getCacheBlockSize()];
      if (cstate != CacheState::SHARED_UPGRADING)
      {
         updateCacheBlock(address, CacheState::SHARED, Transition::COHERENCY, data_buf, ShmemPerfModel::_SIM_THREAD);
      }

      shmem_msg->getPerf()->updateTime(getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD), ShmemPerf::REMOTE_CACHE_FWD);

      getMemoryManager()->sendMsg(PrL1PrL2DramDirectoryMSI::ShmemMsg::WB_REP,
            MemComponent::LAST_LEVEL_CACHE, MemComponent::TAG_DIR,
            shmem_msg->getRequester() /* requester */,
            sender /* receiver */,
            address,
            data_buf, getCacheBlockSize(),
            HitWhere::UNKNOWN, shmem_msg->getPerf(), ShmemPerfModel::_SIM_THREAD);
   }
   else
   {
      // Update Shared Mem perf counters for access to L2 Cache
      getMemoryManager()->incrElapsedTime(m_mem_component, CachePerfModel::ACCESS_CACHE_TAGS, ShmemPerfModel::_SIM_THREAD);
      shmem_msg->getPerf()->updateTime(getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_SIM_THREAD), ShmemPerf::REMOTE_CACHE_WB);
MYLOG("invalid @ %lx, hoping eviction message is underway", address);
   }
}



/*****************************************************************************
 * statistics functions
 *****************************************************************************/

void
CacheCntlr::updateCounters(Core::mem_op_t mem_op_type, IntPtr address, bool cache_hit, CacheState::cstate_t state, Prefetch::prefetch_type_t isPrefetch)
{
   /* If another miss to this cache line is still in progress:
      operationPermissibleinCache() will think it's a hit (so cache_hit == true) since the processing
      of the previous miss was done instantaneously. But mshr[address] contains its completion time */
   SubsecondTime t_now = getShmemPerfModel()->getElapsedTime(ShmemPerfModel::_USER_THREAD);
   bool overlapping = m_master->mshr.count(address) && m_master->mshr[address].t_issue < t_now && m_master->mshr[address].t_complete > t_now;

   // ATD doesn't track state, so when reporting hit/miss to it we shouldn't either (i.e. write hit to shared line becomes hit, not miss)
   bool cache_data_hit = (state != CacheState::INVALID);
   m_master->accessATDs(mem_op_type, cache_data_hit, address, m_core_id - m_core_id_master);

   if (mem_op_type == Core::WRITE)
   {
      if (isPrefetch != Prefetch::NONE)
         stats.stores_prefetch++;
      if (isPrefetch != Prefetch::OWN)
      {
         stats.stores++;
         stats.stores_state[state]++;
         if (! cache_hit || overlapping) {
            stats.store_misses++;
            stats.store_misses_state[state]++;
            if (overlapping) stats.store_overlapping_misses++;
         }
      }
   }
   else
   {
      if (isPrefetch != Prefetch::NONE)
         stats.loads_prefetch++;
      if (isPrefetch != Prefetch::OWN)
      {
         stats.loads++;
         stats.loads_state[state]++;
         if (! cache_hit) {
            stats.load_misses++;
            stats.load_misses_state[state]++;
            if (overlapping) stats.load_overlapping_misses++;
         }
      }
   }

   cleanupMshr();

   #ifdef ENABLE_TRANSITIONS
   transition(
      address,
      mem_op_type == Core::WRITE ? Transition::CORE_WR : (mem_op_type == Core::READ_EX ? Transition::CORE_RDEX : Transition::CORE_RD),
      state,
      mem_op_type == Core::READ && state != CacheState::MODIFIED ? CacheState::SHARED : CacheState::MODIFIED
   );
   #endif
}

void
CacheCntlr::cleanupMshr()
{
   /* Keep only last 8 MSHR entries */
   while(m_master->mshr.size() > 8) {
      IntPtr address_min = 0;
      SubsecondTime time_min = SubsecondTime::MaxTime();
      for(Mshr::iterator it = m_master->mshr.begin(); it != m_master->mshr.end(); ++it) {
         if (it->second.t_complete < time_min) {
            address_min = it->first;
            time_min = it->second.t_complete;
         }
      }
      m_master->mshr.erase(address_min);
   }
}

void
CacheCntlr::transition(IntPtr address, Transition::reason_t reason, CacheState::cstate_t old_state, CacheState::cstate_t new_state)
{
#ifdef ENABLE_TRANSITIONS
   stats.transitions[old_state][new_state]++;
   if (old_state == CacheState::INVALID) {
      if (stats.seen.count(address) == 0)
         old_state = CacheState::INVALID_COLD;
      else if (stats.seen[address] == Transition::EVICT || stats.seen[address] == Transition::BACK_INVAL)
         old_state = CacheState::INVALID_EVICT;
      else if (stats.seen[address] == Transition::COHERENCY)
         old_state = CacheState::INVALID_COHERENCY;
   }
   stats.transition_reasons[reason][old_state][new_state]++;
   stats.seen[address] = reason;
#endif
}

void
CacheCntlr::updateUncoreStatistics(HitWhere::where_t hit_where, SubsecondTime now)
{
   // To be propagated through next-level refill
   m_last_remote_hit_where = hit_where;

   // Update ShmemPerf
   if (m_shmem_perf->getInitialTime() > SubsecondTime::Zero())
   {
      // We can't really be sure that there are not outstanding transations that still pass a pointer
      // around to our ShmemPerf structure. By settings its last time to MaxTime, we prevent anyone
      // from updating statistics while we're reading them.
      m_shmem_perf->disable();

      m_shmem_perf_global->add(m_shmem_perf);
      m_shmem_perf_totaltime += now - m_shmem_perf->getInitialTime();
      m_shmem_perf_numrequests ++;

      m_shmem_perf->reset(SubsecondTime::Zero(), INVALID_CORE_ID);
   }
}


/*****************************************************************************
 * utility functions
 *****************************************************************************/

/* Lock design:
   - we want to allow concurrent access for operations that only touch the first-level cache
   - we want concurrent access to different addresses as much as possible

   Master last-level cache contains one shared/exclusive-lock per set (according to the first-level cache's set size)
   - First-level cache transactions acquire the lock pertaining to the set they'll use in shared mode.
     Multiple first-level caches can do this simultaneously.
     Since only a single thread accesses each L1, there should be no extra per-cache lock needed
     (The above is not strictly true, but Core takes care of this since MemoryManager only has one cycle count anyway).
     On a miss, a lock upgrade is needed.
   - Other levels, or the first level on miss, acquire the lock in exclusive mode which locks out both L1-only and L2+ transactions.
   #ifdef PRIVATE_L2_OPTIMIZATION
   - (On Nehalem, the L2 is private so it is only the L3 (the first level with m_sharing_cores > 1) that takes the exclusive lock).
   #endif

   Additionally, for per-cache objects that are not private to a cache set, each cache controller has its own (normal) lock,
   use getLock() for this. This is required for statistics updates, the directory waiters queue, etc.
*/

void
CacheCntlr::acquireLock(UInt64 address)
{
MYLOG("cache lock acquire %u # %u @ %lx", m_mem_component, m_core_id, address);
   assert(isFirstLevel());
   // Lock this L1 cache for the set containing <address>.
   lastLevelCache()->m_master->getSetLock(address)->acquire_shared(m_core_id);
}

void
CacheCntlr::releaseLock(UInt64 address)
{
MYLOG("cache lock release %u # %u @ %lx", m_mem_component, m_core_id, address);
   assert(isFirstLevel());
   lastLevelCache()->m_master->getSetLock(address)->release_shared(m_core_id);
}

void
CacheCntlr::acquireStackLock(UInt64 address, bool this_is_locked)
{
MYLOG("stack lock acquire %u # %u @ %lx", m_mem_component, m_core_id, address);
   // Lock the complete stack for the set containing <address>
   if (this_is_locked)
      // If two threads decide to upgrade at the same time, we could deadlock.
      // Upgrade therefore internally releases the cache lock!
      lastLevelCache()->m_master->getSetLock(address)->upgrade(m_core_id);
   else
      lastLevelCache()->m_master->getSetLock(address)->acquire_exclusive();
}

void
CacheCntlr::releaseStackLock(UInt64 address, bool this_is_locked)
{
MYLOG("stack lock release %u # %u @ %lx", m_mem_component, m_core_id, address);
   if (this_is_locked)
      lastLevelCache()->m_master->getSetLock(address)->downgrade(m_core_id);
   else
      lastLevelCache()->m_master->getSetLock(address)->release_exclusive();
}


CacheCntlr*
CacheCntlr::lastLevelCache()
{
   if (! m_last_level) {
      /* Find last-level cache */
      CacheCntlr* last_level = this;
      while(last_level->m_next_cache_cntlr)
         last_level = last_level->m_next_cache_cntlr;
      m_last_level = last_level;
   }
   return m_last_level;
}

bool
CacheCntlr::isShared(core_id_t core_id)
{
   core_id_t core_id_master = core_id - core_id % m_shared_cores;
   return core_id_master == m_core_id_master;
}


/*** threads ***/

void
CacheCntlr::wakeUpUserThread(Semaphore* user_thread_sem)
{
   (user_thread_sem ? user_thread_sem : m_user_thread_sem)->signal();
}

void
CacheCntlr::waitForUserThread(Semaphore* network_thread_sem)
{
   (network_thread_sem ? network_thread_sem : m_network_thread_sem)->wait();
}

void
CacheCntlr::waitForNetworkThread()
{
   m_user_thread_sem->wait();
}

void
CacheCntlr::wakeUpNetworkThread()
{
   m_network_thread_sem->signal();
}

Semaphore*
CacheCntlr::getUserThreadSemaphore()
{
   return m_user_thread_sem;
}

Semaphore*
CacheCntlr::getNetworkThreadSemaphore()
{
   return m_network_thread_sem;
}

void
CacheCntlr::inform_tlb_eviction(IntPtr addr, UInt32 way)
{
    //return;
    if (m_master->m_prefetcher[m_core_id]) {
        std::vector<IntPtr> prefetchList = m_master->m_prefetcher[m_core_id]->inform_tlb_eviction(addr, way);
        //     std::cout << "Before tlb eviction prefetches"<<std::endl;
        //return;

        for(std::vector<IntPtr>::iterator it = prefetchList.begin(); it != prefetchList.end(); ++it)
        {
            // Keep at most PREFETCH_MAX_QUEUE_LENGTH entries in the prefetch queue
           // if (m_master->m_prefetch_list.size() > PREFETCH_MAX_QUEUE_LENGTH)
            //    break;
            if (!operationPermissibleinCache(*it, Core::READ))
            {
                m_master->m_prefetch_list.push_back(*it);
                m_master->m_prefetch_list_pc.push_back(0);
                m_master->m_prefetch_core_list.push_back(m_core_id);
            }
            else
                redundant_prefetch_buffer++; 
        }
        //      std::cout << "After tlb eviction prefetches"<<std::endl;
    }
}

#ifdef KNAPSACK
void
CacheCntlr::inform_L1_access(IntPtr pc, IntPtr address)
{
   assert(0);
    //if(pc == 0)
    //    return;
    L1timer++;
    address = ( address >> 6 ) << 6;
    UInt64 curr_quanta = L1timer/QUANTUM;
    //assert(pc);
    //Assume no prefetch
    L1_knapsack.add_access(address, curr_quanta, pc);
    bool prefetch_hit = false;
    bool prefetch_hit_but_cache = false;
    bool decision = false;

    if(L1_addr_history.find(address) != L1_addr_history.end()) // found in history
    {
        int last_quanta = L1_addr_history[address].last_quanta;
        int last_prefetch_quanta = L1_addr_history[address].last_prefetch_quanta;

        prefetch_hit = ((last_prefetch_quanta != -1) && (last_prefetch_quanta >= last_quanta));

        if(last_quanta != -1)
        {
            assert(L1_knapsack.access_history.size() > (unsigned int)last_quanta);
            L1_knapsack.access_history[last_quanta]--;
            if(prefetch_hit)
            {
                assert(last_prefetch_quanta != -1);
                //unsigned int savings_ratio = (double)(curr_quanta - last_quanta + 1)/(double)(curr_quanta - last_prefetch_quanta+1);
                assert(last_prefetch_quanta >= last_quanta);
                //Define as 0 wfor case where prefetches are never cached
                //#define SAVING_THRESHOLD 0
                // Even though the line is prefetch-friendly, let's cache it
                //if(savings_ratio <= SAVING_THRESHOLD)
                //if(!(L1_knapsack.is_contentious(last_prefetch_quanta, last_quanta)))
                //    if ( (!(L1_knapsack.is_contentious(last_prefetch_quanta, last_quanta))) && savings_ratio <= SAVING_THRESHOLD)
                //if ( (!(L1_knapsack.is_contentious(last_prefetch_quanta, last_quanta))) && savings_ratio <= SAVING_THRESHOLD)
                if(false)
                {
                    //Learn here
                    decision = L1_knapsack.should_cache(address, curr_quanta, last_quanta);
                    prefetch_hit_but_cache = true;
                    //demand_prefetch_friendly_cache++;
                }
                else
                {
                    decision = L1_knapsack.should_cache(address, curr_quanta, last_prefetch_quanta);
                    //    demand_prefetch_friendly_dontcache++;
                }

                if(decision)
                    L1_knapsack.access_history[last_prefetch_quanta]--;
                decision = false;
            }
            else
            {
                decision = L1_knapsack.should_cache(address, curr_quanta, last_quanta);
                //if(decision)
                //   demand_prefetch_unfriendly_cache++;
                //else
                //   demand_prefetch_unfriendly_dontcache++;
            }
        }
        else
        {
            assert(prefetch_hit);
            assert(last_prefetch_quanta != -1);
            decision = L1_knapsack.should_cache(address, curr_quanta, last_prefetch_quanta);
            if(decision)
                L1_knapsack.access_history[last_prefetch_quanta]--;
            decision = false;
        }

        IntPtr last_pc = L1_addr_history[address].PC;
        IntPtr key =last_pc;

        if(l1_pcxaddr_opt_accesses.find(std::make_pair(key, address)) == l1_pcxaddr_opt_accesses.end())
        {
            l1_pcxaddr_opt_accesses[std::make_pair(key, address)] = 0;
            l1_pcxaddr_opt_hits[std::make_pair(key, address)] = 0;
        }
        if(l1_pc_opt_accesses.find(key) == l1_pc_opt_accesses.end())
        {
            l1_pc_opt_accesses[key] = 0;
            l1_pc_opt_hits[key] = 0;
        }


        l1_pc_opt_accesses[key]++;
        l1_pcxaddr_opt_accesses[std::make_pair(key, address)]++;
        if(decision)
        {
            l1_pc_opt_hits[key]++;
            l1_pcxaddr_opt_hits[std::make_pair(key, address)]++;
        }

    }
    else
        L1_addr_history[address].init(curr_quanta);

    L1_addr_history[address].update(L1timer, curr_quanta, pc, decision);
    if(prefetch_hit)
    {
        if(!prefetch_hit_but_cache)
            num_ideal_prefetch_hits_L1++;
        L1_addr_history[address].update_prefetch_hit(curr_quanta);
    }
}

void
CacheCntlr::inform_L2_access(IntPtr pc, IntPtr address)
{
   assert(0);
    //if(pc == 0)
    //    return;
    mytimer++;
    address = ( address >> 6 ) << 6;
    UInt64 curr_quanta = mytimer/QUANTUM;
    //assert(pc);
    //Assume no prefetch
    knapsack.add_access(address, curr_quanta, pc);
    bool prefetch_hit = false;
    bool prefetch_hit_but_cache = false;
    bool decision = false;

    if(addr_history.find(address) != addr_history.end())
    {
        int last_quanta = addr_history[address].last_quanta;
        int last_prefetch_quanta = addr_history[address].last_prefetch_quanta;

        prefetch_hit = ((last_prefetch_quanta != -1) && (last_prefetch_quanta >= last_quanta));

        if(last_quanta != -1)
        {
            assert(knapsack.access_history.size() > (unsigned int)last_quanta);
            knapsack.access_history[last_quanta]--;
            if(prefetch_hit)
            {
                assert(last_prefetch_quanta != -1);
                //unsigned int savings_ratio = (double)(curr_quanta - last_quanta + 1)/(double)(curr_quanta - last_prefetch_quanta+1);
                assert(last_prefetch_quanta >= last_quanta);
                //Define as 0 wfor case where prefetches are never cached
                //#define SAVING_THRESHOLD 0
                // Even though the line is prefetch-friendly, let's cache it
                //if(savings_ratio <= SAVING_THRESHOLD)
                //if(!(knapsack.is_contentious(last_prefetch_quanta, last_quanta)))
                //    if ( (!(knapsack.is_contentious(last_prefetch_quanta, last_quanta))) && savings_ratio <= SAVING_THRESHOLD)
                //if ( (!(knapsack.is_contentious(last_prefetch_quanta, last_quanta))) && savings_ratio <= SAVING_THRESHOLD)
                if(false)
                {
                    //Learn here
                    decision = knapsack.should_cache(address, curr_quanta, last_quanta);
                    prefetch_hit_but_cache = true;
                    //demand_prefetch_friendly_cache++;
                }
                else
                {
                    decision = knapsack.should_cache(address, curr_quanta, last_prefetch_quanta);
                    //    demand_prefetch_friendly_dontcache++;
                }

                if(decision)
                    knapsack.access_history[last_prefetch_quanta]--;
                decision = false;
            }
            else
            {
                decision = knapsack.should_cache(address, curr_quanta, last_quanta);
                //if(decision)
                //   demand_prefetch_unfriendly_cache++;
                //else
                //   demand_prefetch_unfriendly_dontcache++;
            }
        }
        else
        {
            assert(prefetch_hit);
            assert(last_prefetch_quanta != -1);
            decision = knapsack.should_cache(address, curr_quanta, last_prefetch_quanta);
            if(decision)
                knapsack.access_history[last_prefetch_quanta]--;
            decision = false;
        }

        IntPtr last_pc = addr_history[address].PC;
        IntPtr key =last_pc;

        if(pcxaddr_opt_accesses.find(std::make_pair(key, address)) == pcxaddr_opt_accesses.end())
        {
            pcxaddr_opt_accesses[std::make_pair(key, address)] = 0;
            pcxaddr_opt_hits[std::make_pair(key, address)] = 0;
        }
        if(pc_opt_accesses.find(key) == pc_opt_accesses.end())
        {
            pc_opt_accesses[key] = 0;
            pc_opt_hits[key] = 0;
        }


        pc_opt_accesses[key]++;
        pcxaddr_opt_accesses[std::make_pair(key, address)]++;
        if(decision)
        {
            pc_opt_hits[key]++;
            pcxaddr_opt_hits[std::make_pair(key, address)]++;
        }

    }
    else
        addr_history[address].init(curr_quanta);

    addr_history[address].update(mytimer, curr_quanta, pc, decision);
    if(prefetch_hit)
    {
        if(!prefetch_hit_but_cache)
            num_ideal_prefetch_hits_L2++;
        addr_history[address].update_prefetch_hit(curr_quanta);
    }
}


void CacheCntlr::inform_prefetch(IntPtr pc, IntPtr address)
{
   assert(0);
    L1timer++;
    address = ( address >> 6 ) << 6;
    UInt64 curr_quanta = L1timer/QUANTUM;
    if(L1_addr_history.find(address) == L1_addr_history.end())
        L1_addr_history[address].init(curr_quanta);

    //if(addr_history[addr].last_prefetch_quanta == -1)
    //{
    L1_addr_history[address].update_prefetch(curr_quanta, pc);
    //}
    L1_knapsack.add_access(address, curr_quanta, pc);


    mytimer++;
    curr_quanta = mytimer/QUANTUM;
    if(addr_history.find(address) == addr_history.end())
        addr_history[address].init(curr_quanta);

    //if(addr_history[addr].last_prefetch_quanta == -1)
    //{
    addr_history[address].update_prefetch(curr_quanta, pc);
    //}
    knapsack.add_access(address, curr_quanta, pc);

}
#endif

}
