/* =============================================================================
 * IoT Gateway - Week 10
 * File   : timing.c
 * Purpose: Cycle-accurate latency measurement using the Cortex-A9 PMU

 * The ARM Cortex-A9 has a Performance Monitor Unit built into each core.
 *
 * To use the cycle counter you need to:
 *   1. Enable the PMU by setting bit 0 of the PMCR register
 *   2. Enable the cycle counter specifically by setting bit 31 of PMCNTENSET
 *   3. Reset the cycle counter by setting bit 2 of PMCR (optional but clean)
 *
 * All PMU registers are accessed via the ARM coprocessor CP15 using the
 * MCR (Move to Coprocessor) and MRC (Move from Coprocessor) instructions.
 * The __asm__ volatile keyword tells the compiler to emit these instructions
 * exactly as written without optimizing them away.
 *
 * Why "volatile":
 * The compiler does not know that reading the same register twice gives
 * different values. Without volatile it might cache the first read and
 * return it for the second, giving elapsed time = 0. The volatile keyword
 * forces a real hardware read every time.
 * =============================================================================
 */

#include "timing.h"

/* ---------------------------------------------------------------------------
 * timing_init
 * Configure and start the PMU cycle counter.
 *
 * PMCR  (Performance Monitor Control Register) - CP15 c9, c12, 0
 *   bit 0 = E: enable all counters
 *   bit 1 = P: reset all event counters
 *   bit 2 = C: reset cycle counter to 0
 *
 * PMCNTENSET (Count Enable Set Register) - CP15 c9, c12, 1
 *   bit 31 = enable the cycle counter (CCNT)
 * --------------------------------------------------------------------------- */
void timing_init(void)
{
    u32 pmcr;

    /* Read current PMCR value */
    __asm__ volatile("MRC p15, 0, %0, c9, c12, 0" : "=r"(pmcr));

    /* Set bit0 (enable), bit2 (reset cycle counter) */
    pmcr |= 0x5U;
    __asm__ volatile("MCR p15, 0, %0, c9, c12, 0" : : "r"(pmcr));

    /* Enable cycle counter in PMCNTENSET - bit 31 = CCNT */
    __asm__ volatile("MCR p15, 0, %0, c9, c12, 1" : : "r"(0x80000000U));
}

/* ---------------------------------------------------------------------------
 * timing_now
 * Read the current cycle counter value from PMCCNTR.
 *
 * PMCCNTR (Cycle Count Register) - CP15 c9, c13, 0
 * Returns a 32-bit count that wraps after ~6.4 seconds at 667 MHz.
 * --------------------------------------------------------------------------- */
u32 timing_now(void)
{
    u32 count;
    __asm__ volatile("MRC p15, 0, %0, c9, c13, 0" : "=r"(count));
    return count;
}

/* ---------------------------------------------------------------------------
 * timing_cycles_to_us
 * Convert cycles to microseconds using integer arithmetic.
 *
 * Formula: us = cycles / (CPU_FREQ_HZ / 1000000)
 *        = cycles / 666.666...
 *        ~ cycles * 3 / 2000  (close approximation using integers)
 *
 * More accurate: cycles / 667 (divides by MHz value directly)
 * --------------------------------------------------------------------------- */
u32 timing_cycles_to_us(u32 cycles)
{
    /* Divide by CPU frequency in MHz (667) to get microseconds.
     * Integer division truncates - result is always slightly low.
     * For values > 1 us this error is less than 0.15%. */
    return cycles / 667U;
}

/* ---------------------------------------------------------------------------
 * timing_cycles_to_ns
 * Convert cycles to nanoseconds
 * --------------------------------------------------------------------------- */
u32 timing_cycles_to_ns(u32 cycles)
{
    return (cycles * 1000U) / 667U;
}
