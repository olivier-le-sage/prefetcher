/*
 * A sample prefetcher which does sequential one-block lookahead.
 * This means that the prefetcher fetches the next block _after_ the one that
 * was just accessed. It also ignores requests to blocks already in the cache.
 */

#include "interface.hh"

#define TRAIN_SIZE 65536

typedef struct training_line {
  Addr context;
  Addr last_addr;
  Addr stride;
} training_line;

uint64_t get_index(Addr addr, int n_index_bits) {
  uint64_t mask = 1 << n_index_bits;
  mask -= 1;
  return addr & mask;
}

int log2_int(int number) {
  int index = number;
  int targetlevel = 0;
  while (index >>= 1)
    ++targetlevel;
  return targetlevel;
}

static training_line training_unit[TRAIN_SIZE] = {0};

void prefetch_init(void) {
  /* Called before any calls to prefetch_access. */
  /* This is the place to initialize data structures. */

  // DPRINTF(HWPrefetch, "Initialized ISB prefetcher\n");
}

void prefetch_access(AccessStat stat) {
  Addr pc_index = get_index(stat.pc, log2_int(TRAIN_SIZE));
  if (training_unit[pc_index].context == stat.pc &&
      training_unit[pc_index].last_addr != stat.mem_addr) {
    int stride = stat.mem_addr - training_unit[pc_index].last_addr;
    if (stride == training_unit[pc_index].stride)
      issue_prefetch(stat.mem_addr + stride);

    training_unit[pc_index].stride = stride;
    training_unit[pc_index].last_addr = stat.mem_addr;
  } else if (training_unit[pc_index].context != stat.pc) {
    training_unit[pc_index].context = stat.pc;
    training_unit[pc_index].last_addr = stat.mem_addr;
    training_unit[pc_index].stride = 0;
  }
}

void prefetch_complete(Addr addr) {
  /*
   * Called when a block requested by the prefetcher has been loaded.
   */
  if (!get_prefetch_bit(addr))
    set_prefetch_bit(addr);
}
