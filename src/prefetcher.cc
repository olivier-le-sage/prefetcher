/*
 * A sample prefetcher which does sequential one-block lookahead.
 * This means that the prefetcher fetches the next block _after_ the one that
 * was just accessed. It also ignores requests to blocks already in the cache.
 */

#include "interface.hh"

#define CHUNK_SIZE 256

#define SPM_SIZE 16384 * 256

#define TRAIN_SIZE 65536 * 256

#define PSM_SIZE 16384 * 256

#define PREFETCH_DEGREE 8

typedef struct training_line {
  Addr context;
  Addr last_addr;
  Addr stride;
} training_line;

typedef struct psm_line {
  Addr tag;
  uint8_t valid;
  Addr structural_address;
  uint8_t cntr;
} psm_line;

typedef struct spm_line {
  Addr tag;
  uint8_t valid;
  Addr physical_address;
} spm_line;

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

static uint32_t isb_counter = 0;

static spm_line spm[SPM_SIZE] = {0};
static psm_line psm[PSM_SIZE] = {0};

static training_line training_unit[TRAIN_SIZE] = {0};

void prefetch_init(void) {
  /* Called before any calls to prefetch_access. */
  /* This is the place to initialize data structures. */

  DPRINTF(HWPrefetch, "Initialized ISB prefetcher\n");
}

void prefetch_access(AccessStat stat) {
  // Training
  Addr pc_index = get_index(stat.pc, log2_int(TRAIN_SIZE));
  int cntr = 0;
  DPRINTF(HWPrefetch, "Current address: %x\n", stat.mem_addr);
  if (training_unit[pc_index].context == stat.pc &&
      training_unit[pc_index].last_addr != stat.mem_addr) {
    int stride = stat.mem_addr - training_unit[pc_index].last_addr;
    if (stride == training_unit[pc_index].stride)
      issue_prefetch(stat.mem_addr + stride);
    training_unit[pc_index].stride = stride;

    // Correlated pair observed, check psm
    Addr a = training_unit[pc_index].last_addr;
    Addr b = stat.mem_addr;
    psm_line a_line = psm[get_index(a, log2_int(PSM_SIZE))];
    psm_line b_line = psm[get_index(b, log2_int(PSM_SIZE))];
    if (a_line.valid && a_line.tag == a && b_line.valid &&
        b_line.tag == stat.mem_addr) {
      if (b_line.structural_address - a_line.structural_address == 1) {
        // They already have consecutive addresses
        psm[get_index(b, log2_int(PSM_SIZE))].cntr++;
        DPRINTF(HWPrefetch, "PC: %x Current address: %x\n", stat.pc, b);
        DPRINTF(HWPrefetch, "Prev address: %x\n", a);
        DPRINTF(HWPrefetch, "Current address struct: %x\n",
                b_line.structural_address);
        DPRINTF(HWPrefetch, "Prev address struct: %x\n",
                a_line.structural_address);
        DPRINTF(HWPrefetch, "Counter increased to: %d \n",
                psm[get_index(b, log2_int(PSM_SIZE))].cntr);
        cntr = 1;
      } else {
        if (!--psm[get_index(b, log2_int(PSM_SIZE))].cntr) {
          // Update psm
          psm[get_index(b, log2_int(PSM_SIZE))].structural_address =
              a_line.structural_address + 1;
          psm[get_index(b, log2_int(PSM_SIZE))].tag = b;
          psm[get_index(b, log2_int(PSM_SIZE))].valid = 1;
          psm[get_index(b, log2_int(PSM_SIZE))].cntr = 1;

          // update spm
          spm[get_index(a_line.structural_address + 1, log2_int(SPM_SIZE))]
              .tag = a_line.structural_address;
          spm[get_index(a_line.structural_address + 1, log2_int(SPM_SIZE))]
              .valid = 1;
          spm[get_index(a_line.structural_address + 1, log2_int(SPM_SIZE))]
              .physical_address = b;
        }
      }
    } else if (a_line.valid && a_line.tag == a) {
      DPRINTF(HWPrefetch, "Update subsequent line\n");

      // Update psm
      psm[get_index(b, log2_int(PSM_SIZE))].structural_address =
          a_line.structural_address + 1;
      psm[get_index(b, log2_int(PSM_SIZE))].tag = b;
      psm[get_index(b, log2_int(PSM_SIZE))].valid = 1;
      psm[get_index(b, log2_int(PSM_SIZE))].cntr = 1;

      // update spm
      spm[get_index(a_line.structural_address + 1, log2_int(SPM_SIZE))].tag =
          a_line.structural_address;
      spm[get_index(a_line.structural_address + 1, log2_int(SPM_SIZE))].valid =
          1;
      spm[get_index(a_line.structural_address + 1, log2_int(SPM_SIZE))]
          .physical_address = b;

    } else {
      DPRINTF(HWPrefetch, "New struct mem assigned\n");

      // A or B have no valid structural_address
      psm[(get_index(a, log2_int(PSM_SIZE)))].tag = a;
      psm[(get_index(a, log2_int(PSM_SIZE)))].valid = 1;
      psm[(get_index(a, log2_int(PSM_SIZE)))].structural_address = isb_counter;
      psm[(get_index(a, log2_int(PSM_SIZE)))].cntr = 1;

      spm[get_index(isb_counter, log2_int(PSM_SIZE))].tag = isb_counter;
      spm[get_index(isb_counter, log2_int(PSM_SIZE))].valid = 1;
      spm[get_index(isb_counter, log2_int(PSM_SIZE))].physical_address = a;

      psm[(get_index(b, log2_int(PSM_SIZE)))].tag = b;
      psm[(get_index(b, log2_int(PSM_SIZE)))].valid = 1;
      psm[(get_index(b, log2_int(PSM_SIZE)))].structural_address =
          isb_counter + 1;
      psm[(get_index(b, log2_int(PSM_SIZE)))].cntr = 1;

      spm[get_index(isb_counter + 1, log2_int(PSM_SIZE))].tag = isb_counter + 1;
      spm[get_index(isb_counter + 1, log2_int(PSM_SIZE))].valid = 1;
      spm[get_index(isb_counter + 1, log2_int(PSM_SIZE))].physical_address = b;
      isb_counter += 256;
    }
  }
  training_unit[pc_index].context = stat.pc;
  training_unit[pc_index].last_addr = stat.mem_addr;
  training_unit[pc_index].stride = 0;

  // Prefetching
  // TODO implement prefetching for degree > 1
  psm_line trigger_line = psm[get_index(stat.mem_addr, log2_int(PSM_SIZE))];
  if (trigger_line.tag != stat.mem_addr || !trigger_line.valid)
    return;
  // DPRINTF(HWPrefetch, "foo \n");
  for (int i = 0; i < PREFETCH_DEGREE; i++) {
    Addr structural_prefetch = trigger_line.structural_address + 1 + i;
    spm_line spm_trigger_line =
        spm[get_index(structural_prefetch, log2_int(SPM_SIZE))];
    if (cntr) {
      DPRINTF(HWPrefetch, "Valid: %d \n", spm_trigger_line.valid);
      DPRINTF(HWPrefetch, "Line: %x \n", spm_trigger_line.tag);
      DPRINTF(HWPrefetch, "Prefetch: %x \n", structural_prefetch);
      DPRINTF(HWPrefetch, "Physical: %x \n", spm_trigger_line.physical_address);
    }
    if (spm_trigger_line.tag != structural_prefetch || !spm_trigger_line.valid)
      return;
    Addr physical_prefetch = spm_trigger_line.physical_address;
    DPRINTF(HWPrefetch, "PREFETCH \n");
    issue_prefetch(physical_prefetch);
  }
}

void prefetch_complete(Addr addr) {
  /*
   * Called when a block requested by the prefetcher has been loaded.
   */
  if (!get_prefetch_bit(addr))
    set_prefetch_bit(addr);
}
