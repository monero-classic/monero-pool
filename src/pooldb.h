#ifndef POOLDB_H
#define POOLDB_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

bool add_share_to_db(uint64_t height, uint64_t difficulty, const char* address, uint64_t timestamp);

bool batch_sql();

bool add_block_to_db(uint64_t height, const char* hash, const char* prevhash, uint64_t difficulty, uint32_t status, uint64_t reward, uint64_t timestamp);

#ifdef __cplusplus
}
#endif

#endif
