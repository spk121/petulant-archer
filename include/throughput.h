/*
 * File:   parch_throughput.h
 * Author: mike
 *
 * Created on August 21, 2013, 5:04 PM
 */

#ifndef PARCH_THROUGHPUT_H
#define	PARCH_THROUGHPUT_H


#ifdef	__cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <assert.h>
#include <stdint.h>

#define THROUGHPUT_INDEX_MIN 3        // 75 bits/s
#define THROUGHPUT_INDEX_MAX 45       // 2 Mbits/s
#define THROUGHPUT_INDEX_DEFAULT 13   // 64 kbits/s

typedef struct _throughput_negotiation {
    bool ok;
    uint8_t index;
} parch_throughput_negotiation_t;

bool
    parch_throughput_index_validate(uint8_t i);
uint8_t
    parch_throughput_index_apply_default(uint8_t i);
uint8_t
    parch_throughput_index_throttle(uint8_t request, uint8_t limit);
parch_throughput_negotiation_t
    parch_throughput_index_negotiate(uint8_t request, uint8_t current);
uint32_t
    parch_throughput_bps(uint8_t i);

#ifdef	__cplusplus
}
#endif

#endif	/* PARCH_THROUGHPUT_H */
