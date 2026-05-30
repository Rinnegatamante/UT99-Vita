#include <vitasdk.h>
#include "robin_hood.h"

robin_hood::unordered_map<uint64_t, const char *> filecache;

extern "C" {
#include "libc_bridge.h"

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
