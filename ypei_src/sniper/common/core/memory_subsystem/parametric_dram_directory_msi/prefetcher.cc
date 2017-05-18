#include "prefetcher.h"
#include "simulator.h"
#include "config.hpp"
#include "log.h"
#include "simple_prefetcher.h"
#include "ghb_prefetcher.h"
#include "ampm_prefetcher.h"
#include "pcdc_prefetcher.h"
#include "isb_prefetcher.h"
#include "bo.h"
#include "markov.h"
#include "sdm.h"
#include "hybrid_prefetcher.h"

Prefetcher* Prefetcher::createPrefetcher(String type, String configName, core_id_t core_id, UInt32 shared_cores)
{
   if (type == "none")
      return NULL;
   else if (type == "simple")
      return new SimplePrefetcher(configName, core_id, shared_cores);
   else if (type == "ghb")
      return new GhbPrefetcher(configName, core_id);
   else if (type == "ampm")
       return new AmpmPrefetcher(configName, core_id);
   else if (type == "pcdc")
       return new PcdcPrefetcher(configName, core_id);
   else if (type == "isb")
       return new IsbPrefetcher(configName, core_id);
   else if (type == "markov")
       return new MarkovPrefetcher(configName, core_id);
   else if (type == "sdm")
       return new SDMPrefetcher(configName, core_id);
   else if (type == "bo")
       return new BOPrefetcher(configName, core_id);
   else if (type == "hybrid")
       return new HybridPrefetcher(configName, core_id);

   LOG_PRINT_ERROR("Invalid prefetcher type %s", type.c_str());
}
