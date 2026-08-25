#include "radar_retx.h"
#include <string.h>

#define GAP_SPAN (RADAR_RETX_MAX_GAP_MS - RADAR_RETX_MIN_GAP_MS + 1)

void radar_retx_arm(radar_retx_t *r, const uint8_t *frame, size_t len,
                    uint8_t repeats, uint32_t now_ms)
{
    if (len > RADAR_FRAME_MAX) { r->left = 0; return; }
    memcpy(r->frame, frame, len);
    r->len = len;
    r->left = repeats;
    r->next_ms = now_ms;                       // first send is immediate
}

bool radar_retx_due(radar_retx_t *r, uint32_t now_ms, uint32_t jitter)
{
    if (r->left == 0) return false;
    if ((int32_t)(now_ms - r->next_ms) < 0) return false;
    r->left--;
    if (r->left) r->next_ms = now_ms + RADAR_RETX_MIN_GAP_MS + (jitter % GAP_SPAN);
    return true;
}

uint8_t radar_retx_adapt(uint8_t cur, bool all_answered)
{
    if (!all_answered) return RADAR_RETX_MAX_REPEATS;
    return cur > RADAR_RETX_MIN_REPEATS ? (uint8_t)(cur - 1) : RADAR_RETX_MIN_REPEATS;
}
