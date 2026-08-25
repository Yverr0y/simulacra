#include "radar_pad.h"
#include "radar_wire.h"        // RADAR_NONCE_LEN, RADAR_TAG_LEN, RADAR_FRAME_MAX

#define PAD_OVERHEAD (RADAR_NONCE_LEN + RADAR_TAG_LEN)     // 28

// Total frame sizes, ascending. The last must stay <= RADAR_FRAME_MAX (the ESP-NOW v1 cap these
// buffers are sized for).
static const size_t BUCKETS[] = { 64, 128, 250 };
#define N_BUCKETS (sizeof BUCKETS / sizeof BUCKETS[0])

size_t radar_pad_plaintext_len(size_t payload_len)
{
    size_t need = payload_len + RADAR_PAD_HDR + PAD_OVERHEAD;   // total frame this payload needs
    for (size_t i = 0; i < N_BUCKETS; i++)
        if (need <= BUCKETS[i]) return BUCKETS[i] - PAD_OVERHEAD;
    return 0;                                                   // does not fit any bucket
}

size_t radar_pad_max_payload(void)
{
    return BUCKETS[N_BUCKETS - 1] - PAD_OVERHEAD - RADAR_PAD_HDR;
}
