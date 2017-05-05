#include "stdio.h"
#include "stdlib.h"

#include "hybrid_prefetcher.h"
#include "prefetcher.h"
#include "simulator.h"
#include "config.hpp"

HybridPrefetcher::HybridPrefetcher(String configName, core_id_t core_id) {
    isb_prefetcher = new IsbPrefetcher(configName, core_id);
    bo_prefetcher = new BOPrefetcher(configName, core_id);
}

HybridPrefetcher::~HybridPrefetcher() {
    delete isb_prefetcher;
    delete bo_prefetcher;
}

std::vector<IntPtr> HybridPrefetcher::getNextAddress(IntPtr pc, IntPtr currentAddress, bool cache_hit, core_id_t core_id) {
    prefetchList.clear();

    std::vector<IntPtr> isb_prefetchList = isb_prefetcher->getNextAddress(pc, currentAddress, cache_hit, core_id);
    std::vector<IntPtr> bo_prefetchList = bo_prefetcher->getNextAddress(pc, currentAddress, cache_hit, core_id);

    prefetchList.insert(prefetchList.end(), isb_prefetchList.begin(), isb_prefetchList.end());
    prefetchList.insert(prefetchList.end(), bo_prefetchList.begin(), bo_prefetchList.end());

    /*
    IntPtr tmp;
    for (tmp = bo_prefetchList().begin; tmp != bo_prefetchList().end; tmp ++ ) {
        if (find(prefetchList.start(), prefetchList.end(), tmp) != prefetchList.end()) { 
            prefetchList.push_back(tmp);
        }
    }
    */

    return prefetchList;
}
