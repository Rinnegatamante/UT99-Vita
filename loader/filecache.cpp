#include <vitasdk.h>
#include "robin_hood.h"
#include "config.h"

#ifdef HAVE_FILEPATH_CACHE
robin_hood::unordered_map<uint64_t, const char *> filecache;

extern "C" {
#ifdef USE_SCELIBC_BRIDGE
#include "libc_bridge.h"
#endif

FILE *cache_lookup(uint64_t hash) {
	auto cacheFind = filecache.find(hash);
    if (cacheFind == filecache.end()) {
        return nullptr;
    }
	return sceLibcBridge_fopen(cacheFind->second, "rb");
}

void cache_insert(uint64_t hash, const char *fname) {
	filecache[hash] = fname;
}

}
#endif
