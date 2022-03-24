/*
 * A sample prefetcher which does sequential one-block lookahead.
 * This means that the prefetcher fetches the next block _after_ the one that
 * was just accessed. It also ignores requests to blocks already in the cache.
 */

#include "interface.hh"
uint64_t log2_int(int number);

#define TABLE_SIZE 1048576 

#define N_INDEX_BITS log2_int(TABLE_SIZE)

#define PREFETCH_DEGREE (1)

typedef struct mht_1 {
  uint8_t valid;
  Addr tag;
  Addr prediction;
} mht_1;

typedef struct mht_2 {
  uint8_t valid;
  Addr tag1;
  Addr tag2;
  Addr prediction;
} mht_2;

int8_t PREF_INITIALIZED = 0;

Addr premiss = 0;
Addr pre2miss = 0;

static mht_1 miss_history_1[TABLE_SIZE] = {0};
static mht_2 miss_history_2[TABLE_SIZE] = {0};

void prefetch_init(void) {
  /* Called before any calls to prefetch_access. */
  /* This is the place to initialize data structures. */

  if (!PREF_INITIALIZED) {
    for (int i = 0; i < TABLE_SIZE; i++) {
      miss_history_1[i].valid = 0;
      miss_history_2[i].valid = 0;
    }
    PREF_INITIALIZED = 1;
    DPRINTF(HWPrefetch, "Initialized domino\n");
    DPRINTF(HWPrefetch, "Index size: %d\n", N_INDEX_BITS);
  }
}

uint64_t get_mht_index(Addr addr) {
  uint64_t mask = 1 << N_INDEX_BITS;
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

uint8_t is_triggering_action(AccessStat stat) {
  return stat.miss || (!stat.miss && get_prefetch_bit(stat.mem_addr));
}

void prefetch_access(AccessStat stat) {
  if (is_triggering_action(stat)) {
    // Prefetch next prediction address if in table
    uint64_t mht_1_index = get_mht_index(stat.mem_addr);
    Addr working_addr = stat.mem_addr ^ premiss;
    uint64_t mht_2_index = get_mht_index(working_addr);
    mht_2 mht_2_entry = miss_history_2[mht_2_index];
    mht_1 mht_1_entry = miss_history_1[mht_1_index];
    // Check mh2 first, then mh1
    if (mht_2_entry.valid && mht_2_entry.tag1 == stat.mem_addr &&
        mht_2_entry.tag2 == premiss) {

      issue_prefetch(mht_2_entry.prediction);
      Addr next_working_addr = stat.mem_addr ^ mht_2_entry.prediction;
      Addr latest_prediction = mht_2_entry.prediction;

      for (uint8_t i = 1; i < PREFETCH_DEGREE; i++) {
        uint64_t next_mht_2_index = get_mht_index(next_working_addr);
        mht_2 next_mht_2_entry = miss_history_2[next_mht_2_index];
        Addr next_predicted_addr = next_mht_2_entry.prediction;
        next_working_addr = latest_prediction ^ next_mht_2_entry.prediction;

        if (!next_mht_2_entry.valid) {
          break;
        }

        issue_prefetch(next_predicted_addr);
        latest_prediction = next_predicted_addr;
      }
    } else if (mht_1_entry.valid && mht_1_entry.tag == stat.mem_addr) {
      issue_prefetch(mht_1_entry.prediction);
    }
    mht_1_index = get_mht_index(premiss);
    working_addr = premiss ^ pre2miss;
    mht_2_index = get_mht_index(working_addr);

    // Update MHT-tables
    if (premiss) {
      miss_history_1[mht_1_index].valid = 1;
      miss_history_1[mht_1_index].tag = premiss;
      miss_history_1[mht_1_index].prediction = stat.mem_addr;
    }

    if (premiss && pre2miss) {
      miss_history_2[mht_2_index].valid = 1;
      miss_history_2[mht_2_index].tag1 = premiss;
      miss_history_2[mht_2_index].tag2 = pre2miss;
      miss_history_2[mht_2_index].prediction = stat.mem_addr;
    }

    pre2miss = premiss;
    premiss = stat.mem_addr;
  }
}

void prefetch_complete(Addr addr) {
  /*
   * Called when a block requested by the prefetcher has been loaded.
   */
  if (!get_prefetch_bit(addr))
    set_prefetch_bit(addr);
}
