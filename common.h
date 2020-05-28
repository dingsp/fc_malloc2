#ifndef COMMON
#define COMMON

#include <thread>
#include <stdint.h>
#include <memory.h>
#include <stdlib.h>
#include <iostream>
#include <vector>
#include <assert.h>
#include <unistd.h>
#include <iostream>
#include <sstream>
#include <algorithm>

#define MIN_BLOCK_SIZE 8
#define HEDER_SIZE 8
#define CHUNK_SIZE (256 * 1024)
#define ALIGN_CHUNK_SIZE CHUNK_SIZE

#define SMALL_BIN_BITS 10
#define SMALL_BIN_CAPCITY (1<<SMALL_BIN_BITS)
#define SMALL_BIN_SIZE (SMALL_BIN_CAPCITY - HEDER_SIZE)
#define SMALL_BIN_CACHE_NUM 4

#define NUM_LARGE_BINS 56
#define NUM_SMALL_BINS 21
#define NUM_BINS (NUM_LARGE_BINS + NUM_SMALL_BINS)
#define SMALL_BLOCK 336
#define LARGE_BLOCK CHUNK_SIZE

#define QUEUE_SIZE 128

#define LIST_CACHE_NUM 4

#endif