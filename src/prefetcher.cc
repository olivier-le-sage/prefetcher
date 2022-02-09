/*
 * A sample prefetcher which does sequential one-block lookahead.
 * This means that the prefetcher fetches the next block _after_ the one that
 * was just accessed. It also ignores requests to blocks already in the cache.
 */

#include "interface.hh"

#define ACC_TABLE_SIZE 64
#define FILTER_TABLE_SIZE 64
#define PHT_SIZE 1024

#define N_REGION_BLOCKS 1

struct filter_table_row {
  Addr tag;
  Addr pc;
  uint32_t offset;
};

struct acc_table_row {
  Addr tag;
  Addr pc;
  uint32_t offset;
  uint64_t pattern;
};

struct pht_row {
  Addr tag;
  uint64_t pattern;
};

struct acc_table_row acc_table[ACC_TABLE_SIZE] = {0};
struct filter_table_row filter_table[FILTER_TABLE_SIZE] = {0};
struct pht_row pht[PHT_SIZE] = {0};

uint64_t get_index(Addr addr, int n_index_bits) {
  uint64_t mask = 1 << n_index_bits;
  mask -= 1;
  return addr & mask;
}

uint64_t log2_int(int number) {
  int index = number;
  int targetlevel = 0;
  while (index >>= 1)
    ++targetlevel;
  return targetlevel;
}

Addr get_region_base(Addr addr) {
  int region_size = N_REGION_BLOCKS * BLOCK_SIZE;
  int region_offset = addr % region_size;
  return addr - region_offset;
}

int get_acc_table_index(Addr addr) {
  int index = -1;
  Addr tag = get_region_base(addr);
  for (int i = 0; i < ACC_TABLE_SIZE; i++) {
    if (acc_table[i].tag == tag)
      index = i;
  }
  return index;
}

int get_filter_table_index(Addr addr) {
  int index = -1;
  Addr tag = get_region_base(addr);
  for (int i = 0; i < FILTER_TABLE_SIZE; i++) {
    if (filter_table[i].tag == tag)
      index = i;
  }
  return index;
}

// Returns the offset from region base in number of cache blocks
uint32_t get_block_offset(Addr addr) {
  int region_size = N_REGION_BLOCKS * BLOCK_SIZE;
  int region_offset = addr % region_size;
  return region_offset;
}

void prefetch_init(void) {
  /* Called before any calls to prefetch_access. */
  /* This is the place to initialize data structures. */

  DPRINTF(HWPrefetch, "Initialized sequential-memory-stream prefetcher\n");
}

void assign_to_filter_entry(struct filter_table_row *row, Addr tag, Addr pc,
                            uint32_t offset) {
  row->tag = tag;
  row->pc = pc;
  row->offset = offset;
}

void assign_to_acc_entry(struct acc_table_row *row, Addr tag, Addr pc,
                         uint32_t offset, uint64_t pattern) {
  row->tag = tag;
  row->pc = pc;
  row->offset = offset;
  row->pattern = pattern;
}

// Add entry to filter table
void add_to_filter_table(AccessStat stat) {
  int index = -1;
  for (int i = 0; i < FILTER_TABLE_SIZE; i++) {
    if (filter_table[i].tag == 0 && filter_table[i].pc == 0 &&
        filter_table[i].offset == 0)
      index = i;
  }
  if (index == -1)
    index = 0;
  assign_to_filter_entry(&filter_table[index], get_region_base(stat.mem_addr),
                         stat.pc, get_block_offset(stat.mem_addr));
}

void add_to_pattern_table(struct acc_table_row *acc_row) {
  DPRINTF(HWPrefetch, "Added to pattern table\n");
  Addr prediction_tag = acc_row->pc ^ acc_row->offset;
  int index = get_index(prediction_tag, log2_int(PHT_SIZE));
  pht_row *pht_row = &pht[index];
  pht_row->tag = prediction_tag;
  pht_row->pattern = acc_row->pattern;
}

void add_to_accumulation_table(struct filter_table_row *filter_row,
                               AccessStat stat) {
  int index = -1;
  for (int i = 0; i < ACC_TABLE_SIZE; i++) {
    if (acc_table[i].pattern == 0)
      index = i;
  }
  // If full, start at beginning
  if (index == -1) {
    add_to_pattern_table(&acc_table[0]);
    index = 0;
  }
  uint64_t pattern =
      (1 << filter_row->offset | 1 << get_block_offset(stat.mem_addr));
  assign_to_acc_entry(&acc_table[index], filter_row->tag, filter_row->pc,
                      filter_row->offset, pattern);
}

// Since the simulator does not tell us when block is evicted, we need
// to check on every access
void handle_evictions() {
  for (int i = 0; i < FILTER_TABLE_SIZE; i++) {
    struct filter_table_row *row = &filter_table[i];
    // Only check of non-empty entries
    if (row->tag == 0 && row->pc == 0 && row->offset == 0)
      continue;
    // Clear rows that has been evicted
    if (!in_cache(row->tag + row->offset))
      assign_to_filter_entry(row, 0, 0, 0);
  }
  for (int i = 0; i < ACC_TABLE_SIZE; i++) {
    struct acc_table_row *row = &acc_table[i];
    // Only check of non-empty entries
    if (row->pattern == 0)
      continue;
    for (int j = 0; j < 64; j++) {
      uint64_t curr_offset = (1 << j) && row->pattern;
      if (curr_offset && !in_cache(row->tag + j)) {
        add_to_pattern_table(row);
        assign_to_acc_entry(row, 0, 0, 0, 0);
      }
    }
  }
}

void train_unit(AccessStat stat) {
  handle_evictions();
  int index = get_acc_table_index(stat.mem_addr);
  // There is an entry in the accumulation table
  if (index != -1) {
    int block_offset = get_block_offset(stat.mem_addr);
    // Update pattern
    DPRINTF(HWPrefetch, "Update pattern\n");
    acc_table[index].pattern |= 1 << block_offset;
    return;
  }

  index = get_filter_table_index(stat.mem_addr);
  // No match found in filter_table
  if (index == -1) {
    // Create entry in filter table
    add_to_filter_table(stat);
    return;
  }
  struct filter_table_row *row = &filter_table[index];
  // If same block, do nothing
  if (row->offset == get_block_offset(stat.mem_addr))
    return;
  add_to_accumulation_table(row, stat);
  // Clear row in filter table
  assign_to_filter_entry(row, 0, 0, 0);
}

void prediction_unit(AccessStat stat) {
  uint32_t offset = get_block_offset(stat.mem_addr);
  Addr base = get_region_base(stat.mem_addr);
  Addr prediction_tag = stat.pc ^ offset;
  int index = get_index(prediction_tag, log2_int(PHT_SIZE));
  pht_row *row = &pht[index];
  if (row->tag != prediction_tag || !row->pattern)
    return;
  DPRINTF(HWPrefetch, "PREFETCH\n");
  uint64_t pattern = row->pattern;
  for (int i = 0; i < 64; i++) {
    if ((1 << i) & pattern) {
      issue_prefetch(base + i);
    }
  }
}

void prefetch_access(AccessStat stat) {
  train_unit(stat);
  prediction_unit(stat);
}

void prefetch_complete(Addr addr) {
  /*
   * Called when a block requested by the prefetcher has been loaded.
   */
}
