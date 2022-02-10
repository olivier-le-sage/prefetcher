/*
 * A sample prefetcher which does sequential one-block lookahead.
 * This means that the prefetcher fetches the next block _after_ the one that
 * was just accessed. It also ignores requests to blocks already in the cache.
 */

#include "interface.hh"

#define M_NUM_RPT_ENTRIES (1024)
#define M_INDEX_INVALID   (M_NUM_RPT_ENTRIES)

typedef enum {
    RPT_STATE_UNUSED = 0,
    RPT_STATE_INITIAL,
    RPT_STATE_TRANSIENT,
    RPT_STATE_STEADY,
    RPT_STATE_NO_PREDICTION,
} rpt_entry_state_t;

typedef struct {
    Addr tag; // address of the load/store instruction (PC value)
    Addr prev_addr; // last (operand) address that was referenced
    uint64_t stride; // difference between the last two addresses
    rpt_entry_state_t state; // state of past history
} reference_prediction_table_entry_t;

static reference_prediction_table_entry_t m_rpt[M_NUM_RPT_ENTRIES];

static uint32_t m_find_index_of_tag_in_rpt(Addr instruction_addr)
{
    uint32_t i = 0;

    while (m_rpt[i].tag != instruction_addr)
    {
        if (i >= M_INDEX_INVALID)
        {
            break;
        }

        i++;
    }

    return M_INDEX_INVALID;
}

static uint32_t m_get_next_unused_entry_in_rpt(void)
{
    for (uint32_t i = 0; i < M_NUM_RPT_ENTRIES; i++)
    {
        if (!m_rpt[i].state)
        {
            return i;
        }
    }

    // Table is full
    return M_INDEX_INVALID;
}

static void m_populate_unused_rpt_entry(uint32_t rpt_index, AccessStat stat)
{
    uint32_t i;

    if (rpt_index == M_INDEX_INVALID)
    {
        // not sure about replacement policy for the rpt
        i = 0;
    }
    else
    {
        i = rpt_index;
    }

    m_rpt[i].tag = stat.pc;
    m_rpt[i].prev_addr = stat.mem_addr;
    m_rpt[i].state = RPT_STATE_INITIAL;
    m_rpt[i].stride = 0;
}

static bool m_is_effective_address_correct(uint32_t rpt_index, Addr addr)
{
    return addr == (m_rpt[rpt_index].prev_addr + m_rpt[rpt_index].stride);
}

static void m_set_stride(uint32_t rpt_index, Addr addr, Addr prev_addr)
{
    if (prev_addr > addr)
    {
        m_rpt[rpt_index].stride = prev_addr - addr;
    }
    else
    {
        m_rpt[rpt_index].stride = addr - prev_addr;
    }
}

void prefetch_init(void)
{
    /* Called before any calls to prefetch_access. */
    /* This is the place to initialize data structures. */

    memset(m_rpt, 0, M_NUM_RPT_ENTRIES * sizeof(reference_prediction_table_entry_t));

    //DPRINTF(HWPrefetch, "Initialized sequential-on-access prefetcher\n");
}

void prefetch_access(AccessStat stat)
{
    Addr rpt_index =  m_find_index_of_tag_in_rpt(stat.pc);

    bool will_issue_prefetch = false;

    if (rpt_index == M_INDEX_INVALID)
    {
        // Addr of the instruction was not found in the RPT
        m_populate_unused_rpt_entry(m_get_next_unused_entry_in_rpt(), stat);
    }
    else
    {
        if (rpt_index != M_INDEX_INVALID)
        {
            switch(m_rpt[rpt_index].state)
            {
                case RPT_STATE_UNUSED:
                    m_populate_unused_rpt_entry(rpt_index, stat);
                    break;
                case RPT_STATE_INITIAL:
                {
                    m_rpt[rpt_index].state = RPT_STATE_TRANSIENT;
                    m_set_stride(rpt_index, stat.mem_addr, m_rpt[rpt_index].prev_addr);
                    will_issue_prefetch = true;
                    break;
                }
                case RPT_STATE_TRANSIENT:
                {
                    if (m_is_effective_address_correct(rpt_index, stat.mem_addr))
                    {
                        m_rpt[rpt_index].prev_addr = stat.mem_addr;
                        m_rpt[rpt_index].state = RPT_STATE_STEADY;
                    }
                    else
                    {
                        m_rpt[rpt_index].prev_addr = stat.mem_addr;
                        m_rpt[rpt_index].state = RPT_STATE_NO_PREDICTION;
                        m_set_stride(rpt_index, stat.mem_addr, m_rpt[rpt_index].prev_addr);
                    }

                    will_issue_prefetch = true;

                    break;
                }
                case RPT_STATE_STEADY:
                {
                    if (m_is_effective_address_correct(rpt_index, stat.mem_addr))
                    {
                        m_rpt[rpt_index].prev_addr = stat.mem_addr;
                    }
                    else
                    {
                        m_rpt[rpt_index].prev_addr = stat.mem_addr;
                        m_rpt[rpt_index].state = RPT_STATE_INITIAL;
                    }

                    will_issue_prefetch = true;
                    break;
                }
                case RPT_STATE_NO_PREDICTION:
                    if (m_is_effective_address_correct(rpt_index, stat.mem_addr))
                    {
                        m_rpt[rpt_index].prev_addr = stat.mem_addr;
                        m_rpt[rpt_index].state = RPT_STATE_TRANSIENT;
                    }
                    else
                    {
                        m_rpt[rpt_index].prev_addr = stat.mem_addr;
                        m_set_stride(rpt_index, stat.mem_addr, m_rpt[rpt_index].prev_addr);
                    }
                    break;
                default:
                    break;
            }
        }
    }

    if (will_issue_prefetch)
    {
        Addr data_block_addr = m_rpt[rpt_index].prev_addr + m_rpt[rpt_index].stride;

        if (!in_cache(data_block_addr))
        {
            issue_prefetch(data_block_addr);
        }
    }
}

void prefetch_complete(Addr addr) {
    /*
     * Called when a block requested by the prefetcher has been loaded.
     */
}
