#ifndef __HYBRID_PREFETCHER_H
#define __HYBRID_PREFETCHER_H

#include "prefetcher.h"
#include "isb_prefetcher.h"
#include "bo.h"


class HybridPrefetcher: public Prefetcher
{
    public:
        HybridPrefetcher(String configName, core_id_t core_id);
        ~HybridPrefetcher();

        std::vector<IntPtr> getNextAddress(IntPtr pc, IntPtr currentAddress, bool cache_hit, core_id_t core_id);

    private:
        IsbPrefetcher *isb_prefetcher; 
        BOPrefetcher *bo_prefetcher; 

        std::vector<IntPtr> prefetchList;
        
        /* Prefetcher decider variables */

        /* Performance measurement parameters */

    void hybrid_info_print()
    {

    }

};

#endif // __HYBRID_PREFETCHER_H

