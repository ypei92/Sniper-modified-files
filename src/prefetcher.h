#ifndef PREFETCHER_H
#define PREFETCHER_H

#include "fixed_types.h"

#include <vector>

class Prefetcher
{
   public:
      Prefetcher() : lookahead(1), degree(1) {}
      virtual ~Prefetcher() {}
      static Prefetcher* createPrefetcher(String type, String configName, core_id_t core_id, UInt32 shared_cores);

      virtual std::vector<IntPtr> getNextAddress(IntPtr pc, IntPtr current_address, bool cache_hit, core_id_t core_id) = 0;

      // For most prefetchers just do nothing. It is only used for ISB prefetcher.
      virtual std::vector<IntPtr> inform_tlb_eviction(UInt64, UInt32) {return std::vector<IntPtr>();}

      // TODO IMplement lookahead control
      virtual unsigned get_lookahead() {
          return lookahead;
      }

      virtual unsigned get_degree() {
          return degree;
      }

   private:
      unsigned lookahead;
      unsigned degree;

};

#endif // PREFETCHER_H
