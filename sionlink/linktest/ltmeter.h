// Link Test: per-second link measurements, shared by the Sion and the DS.

#ifndef LTMETER_H
#define LTMETER_H

#include <stdbool.h>
#include <stdint.h>

#include "ltproto.h"

typedef struct {
    bool have;
    uint32_t last_seq, last_ms, sec_start;
    uint32_t got, lost, worst, rtt_sum, rtt_n, rtt_max;
    lt_stats done;              // the last full second
    uint64_t total_got, total_lost;
    uint32_t total_worst, total_rtt_max;
} lt_meter;

// A packet with sequence number seq arrived at now (ms). Returns false for a
// duplicate or one older than what we already have.
static inline bool lt_meter_rx(lt_meter *m, uint32_t seq, uint32_t now)
{
    if (m->have)
    {
        if ((int32_t)(seq - m->last_seq) <= 0)
            return false;
        uint32_t missed = seq - m->last_seq - 1;
        if (missed > 1000)
            missed = 0; // the sender restarted
        m->lost += missed;
        uint32_t gap = now - m->last_ms;
        if (gap > m->worst)
            m->worst = gap;
    }
    else
    {
        m->sec_start = now;
    }
    m->have = true;
    m->last_seq = seq;
    m->last_ms = now;
    m->got++;
    return true;
}

static inline void lt_meter_rtt(lt_meter *m, uint32_t rtt)
{
    m->rtt_sum += rtt;
    m->rtt_n++;
    if (rtt > m->rtt_max)
        m->rtt_max = rtt;
}

static inline uint16_t lt_clip16(uint32_t v)
{
    return v > 0xFFFF ? 0xFFFF : (uint16_t)v;
}

// Call every frame. Returns true when a second has just completed.
static inline bool lt_meter_tick(lt_meter *m, uint32_t now)
{
    if (!m->have || now - m->sec_start < 1000)
        return false;
    // A sender that went quiet still counts as a gap
    uint32_t quiet = now - m->last_ms;
    if (quiet > m->worst)
        m->worst = quiet;
    m->done.got = lt_clip16(m->got);
    m->done.lost = lt_clip16(m->lost);
    m->done.worst_gap = lt_clip16(m->worst);
    m->done.rtt_avg = m->rtt_n ? lt_clip16(m->rtt_sum / m->rtt_n) : 0;
    m->done.rtt_max = lt_clip16(m->rtt_max);
    m->total_got += m->got;
    m->total_lost += m->lost;
    if (m->worst > m->total_worst)
        m->total_worst = m->worst;
    if (m->rtt_max > m->total_rtt_max)
        m->total_rtt_max = m->rtt_max;
    m->got = m->lost = m->worst = m->rtt_sum = m->rtt_n = m->rtt_max = 0;
    m->sec_start += 1000;
    if (now - m->sec_start >= 1000)
        m->sec_start = now;
    return true;
}

#endif
