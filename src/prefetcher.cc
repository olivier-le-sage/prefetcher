/*
 * A sample prefetcher which does sequential one-block lookahead.
 * This means that the prefetcher fetches the next block _after_ the one that
 * was just accessed. It also ignores requests to blocks already in the cache.
 */

#include "interface.hh"
#include <stdio.h>

#define ACC_TABLE_SIZE 64
#define FILTER_TABLE_SIZE 64
#define PHT_SIZE 1024

#define N_REGION_BLOCKS 64

typedef uint32_t bitmap_t;

#define BITS_PER_WORD (sizeof(bitmap_t) * 8)
#define WORD_OFFSET(b) ((b) / BITS_PER_WORD)
#define BIT_OFFSET(b) ((b) % BITS_PER_WORD)

struct filter_table_row {
  Addr tag;
  Addr pc;
  uint64_t offset;
};

struct acc_table_row {
  Addr tag;
  Addr pc;
  uint64_t offset;
  bitmap_t pattern[N_REGION_BLOCKS / BITS_PER_WORD];
};

struct pht_row {
  Addr pc;
  uint64_t offset;
  bitmap_t pattern[N_REGION_BLOCKS / BITS_PER_WORD];
};

int acc_index = 0;

struct acc_table_row acc_table[ACC_TABLE_SIZE] = {0};
struct filter_table_row filter_table[FILTER_TABLE_SIZE] = {0};
struct pht_row pht[PHT_SIZE] = {0};

void set_bit(bitmap_t *bitmap, int bit) {
  bitmap[WORD_OFFSET(bit)] |= 1 << BIT_OFFSET(bit);
}

void copy_bitmap(bitmap_t *dest, bitmap_t *src) {
  for (int i = 0; i < N_REGION_BLOCKS / BITS_PER_WORD; i++) {
    dest[i] = src[i];
  }
}

int get_bit(bitmap_t *bitmap, int bit) {
  int current_bit = (int)(bitmap[WORD_OFFSET(bit)] & (1 << BIT_OFFSET(bit)));
  return current_bit != 0;
}

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

// Returns the offset from region base in number of cache blocks
uint64_t get_block_offset(Addr addr) {
  int region_size = N_REGION_BLOCKS * BLOCK_SIZE;
  int region_offset = addr % region_size;
  return region_offset / BLOCK_SIZE;
}

int get_pht_index(AccessStat stat) {
  int index = -1;
  uint64_t offset = get_block_offset(stat.mem_addr);
  for (int i = 0; i < PHT_SIZE; i++) {
    if (pht[i].offset == offset && pht[i].pc == stat.pc)
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

void prefetch_init(void) {
  /* Called before any calls to prefetch_access. */
  /* This is the place to initialize data structures. */

  // DPRINTF(HWPrefetch, "Initialized sequential-memory-stream prefetcher\n");
}

void assign_to_filter_entry(struct filter_table_row *row, Addr tag, Addr pc,
                            uint64_t offset) {
  row->tag = tag;
  row->pc = pc;
  row->offset = offset;
}

void assign_to_acc_entry(struct acc_table_row *row, Addr tag, Addr pc,
                         uint64_t offset, bitmap_t *pattern) {
  row->tag = tag;
  row->pc = pc;
  row->offset = offset;
  copy_bitmap(acc_table->pattern, pattern);
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
  // DPRINTF(HWPrefetch, "Added to pattern table\n");
  // printf("Added to pattern table addr: %lx pc: %lx offset: %lu \n",
  // acc_row->tag, acc_row->pc, acc_row->offset);
  Addr prediction_tag = acc_row->pc ^ acc_row->offset;
  int index = get_index(prediction_tag, log2_int(PHT_SIZE));
  pht_row *pht_row = &pht[index];
  pht_row->pc = acc_row->pc;
  pht_row->offset = acc_row->offset;
  copy_bitmap(pht_row->pattern, acc_row->pattern);
}

void add_to_accumulation_table(struct filter_table_row *filter_row,
                               AccessStat stat) {
  int index = -1;
  for (int i = 0; i < ACC_TABLE_SIZE; i++) {
    if (acc_table[i].tag == 0 && acc_table[i].pc == 0)
      index = i;
  }
  // If full, start at beginning
  if (index == -1) {
    // printf("Acc table full\n");
    add_to_pattern_table(&acc_table[acc_index]);
    index = acc_index;
    acc_index = acc_index == ACC_TABLE_SIZE - 1 ? 0 : acc_index + 1;
  }
  bitmap_t pattern[N_REGION_BLOCKS / BITS_PER_WORD] = {0};
  // printf("Original offset: %lu New offset: %lu\n", filter_row->offset,
  //    get_block_offset(stat.mem_addr));
  set_bit(pattern, filter_row->offset);
  set_bit(pattern, get_block_offset(stat.mem_addr));
  assign_to_acc_entry(&acc_table[index], filter_row->tag, filter_row->pc,
                      filter_row->offset, pattern);
}

// Since the simulator does not tell us when block is evicted, we need
// to check on every access
void handle_evictions(AccessStat stat) {
  for (int i = 0; i < FILTER_TABLE_SIZE; i++) {
    struct filter_table_row *row = &filter_table[i];
    if (row->tag == get_region_base(stat.mem_addr))
      continue;
    // Only check of non-empty entries
    if (row->tag == 0 && row->pc == 0 && row->offset == 0)
      continue;
    // Clear rows that has been evicted
    if (!in_cache(row->tag + row->offset * BLOCK_SIZE) &&
        !in_mshr_queue(row->tag + row->offset * BLOCK_SIZE)) {
      // printf("Removed from filter table: %lx pc: %lx offset: %lx \n",
      // row->tag,
      // row->pc, row->offset);
      assign_to_filter_entry(row, 0, 0, 0);
    }
  }
  for (int i = 0; i < ACC_TABLE_SIZE; i++) {
    struct acc_table_row *row = &acc_table[i];
    // Only check of non-empty entries
    if (row->tag == 0 && row->pc == 0 && row->offset == 0)
      continue;
    if (row->tag != get_region_base(stat.mem_addr))
      continue;
    for (int j = 0; j < N_REGION_BLOCKS; j++) {
      int curr_offset = get_bit(row->pattern, j);
      Addr block_addr = row->tag + j * BLOCK_SIZE;
      if (!curr_offset)
        continue;
      // printf("Possible evection %lx offset: %d in cache: %d in queue: %d\n",
      // block_addr, j, in_cache(block_addr), in_mshr_queue(block_addr));
      if (!in_cache(block_addr) && !in_mshr_queue(block_addr)) {
        // printf("Eviction %lx in cache: %d in queue: %d\n", block_addr,
        // in_cache(block_addr), in_mshr_queue(block_addr));
        add_to_pattern_table(row);
        bitmap_t empty_bitmap[N_REGION_BLOCKS / BITS_PER_WORD] = {0};
        assign_to_acc_entry(row, 0, 0, 0, empty_bitmap);
        break;
      }
    }
  }
}

void train_unit(AccessStat stat) {
  handle_evictions(stat);
  int index = get_acc_table_index(stat.mem_addr);
  // There is an entry in the accumulation table
  if (index != -1) {
    int block_offset = get_block_offset(stat.mem_addr);
    // Update pattern
    // DPRINTF(HWPrefetch, "Update pattern\n");
    // printf("Added bit %d to pattern table addr: %lx pc: %lx offset: %lx \n",
    //     block_offset, acc_table[index].tag, acc_table[index].pc,
    //    acc_table[index].offset);
    set_bit(acc_table[index].pattern, block_offset);
    return;
  }

  index = get_filter_table_index(stat.mem_addr);
  // No match found in filter_table
  if (index == -1) {
    // Create entry in filter table
    // printf("Add to filter table addr: %lx, base: %lx\n", stat.mem_addr,
    //   get_region_base(stat.mem_addr));
    add_to_filter_table(stat);
    return;
  }
  struct filter_table_row *row = &filter_table[index];
  // If same block, do nothing
  if (row->offset == get_block_offset(stat.mem_addr))
    return;
  // printf("Move to acc table base: %lx pc: %lx \n", row->tag, row->pc);
  add_to_accumulation_table(row, stat);
  // Clear row in filter table
  assign_to_filter_entry(row, 0, 0, 0);
}

void prediction_unit(AccessStat stat) {
  if (!stat.miss)
    return;
  Addr base = get_region_base(stat.mem_addr);
  int index = get_pht_index(stat);
  pht_row *row = &pht[index];
  if (index == -1)
    return;
  // DPRINTF(HWPrefetch, "PREFETCH\n");
  // printf("Base: %lx\n", base);
  // printf("Trigger: %lx\n", stat.mem_addr);
  // printf("Pattern: ");
  for (int i = 0; i < N_REGION_BLOCKS / BITS_PER_WORD; i++) {
    // printf("%x", row->pattern[i]);
  }
  // printf("\n");
  int max_stream_len = 8;
  int n_prefetches = 0;
  for (int i = 0; i < N_REGION_BLOCKS; i++) {
    Addr prefetch_candiate = base + i * BLOCK_SIZE;

    // printf("i: %d, in pattern %d\n", i, get_bit(row->pattern, i));
    if (get_bit(row->pattern, i) && !in_cache(prefetch_candiate) &&
        !in_mshr_queue(prefetch_candiate) && max_stream_len) {
      // printf("Candidate: %lx\n", prefetch_candiate);
      issue_prefetch(prefetch_candiate);
      max_stream_len--;
      n_prefetches++;
    }
  }
  // printf("N prefetches: %d\n", n_prefetches);
}

void prefetch_access(AccessStat stat) {
  // printf("Access to: %lx from pc: %lx\n", stat.mem_addr, stat.pc);
  train_unit(stat);
  prediction_unit(stat);
  // if (get_prefetch_bit(stat.mem_addr))
  // printf("Successful prefetch at addr: %lx\n", stat.mem_addr);
}

void prefetch_complete(Addr addr) {
  /*
   * Called when a block requested by the prefetcher has been loaded.
   */
  if (!get_prefetch_bit(addr))
    set_prefetch_bit(addr);
}
