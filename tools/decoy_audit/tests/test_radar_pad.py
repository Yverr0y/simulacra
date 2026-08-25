import os, subprocess, unittest

HERE = os.path.dirname(os.path.abspath(__file__)); TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "synth_dump.exe" if os.name == "nt" else "synth_dump")

FRAME_OVERHEAD = 28          # nonce(12) + tag(16)
BUCKETS = (64, 128, 250)     # total frame sizes


def padded(payload_len):
    out = subprocess.check_output([EXE, "--padbucket", str(payload_len)], text=True)
    return int(out.strip())


@unittest.skipUnless(os.path.exists(EXE), "synth_dump not built")
class RadarPad(unittest.TestCase):
    """Frame-length bucketing (wire v4).

    Hiding the plaintext type byte is close to cosmetic while REQUEST/CONFIG/STATUS remain
    ~32/95/206 bytes -- the lengths classify the traffic on their own. These pin the bucket
    arithmetic that breaks that mapping.
    """

    def test_every_result_is_a_real_bucket(self):
        for n in (0, 1, 4, 33, 34, 67, 97, 98, 174, 193, 219):
            frame = padded(n) + FRAME_OVERHEAD
            self.assertIn(frame, BUCKETS, f"payload {n} -> frame {frame}, not a bucket")

    def test_picks_the_smallest_bucket_that_fits(self):
        self.assertEqual(padded(4) + FRAME_OVERHEAD, 64)     # REQUEST
        self.assertEqual(padded(33) + FRAME_OVERHEAD, 64)    # exactly fills the small bucket
        self.assertEqual(padded(34) + FRAME_OVERHEAD, 128)   # one byte over -> next bucket
        self.assertEqual(padded(67) + FRAME_OVERHEAD, 128)   # CONFIG
        self.assertEqual(padded(97) + FRAME_OVERHEAD, 128)   # exactly fills the middle bucket
        self.assertEqual(padded(98) + FRAME_OVERHEAD, 250)
        self.assertEqual(padded(219) + FRAME_OVERHEAD, 250)  # exactly fills the largest

    def test_monotonic(self):
        # a bigger payload never yields a smaller frame
        prev = 0
        for n in range(0, 220):
            cur = padded(n)
            self.assertGreaterEqual(cur, prev, f"payload {n} shrank the frame")
            prev = cur

    def test_oversized_payload_is_refused(self):
        # 220 cannot fit: 220 + 3 + 28 = 251 > RADAR_FRAME_MAX
        self.assertEqual(padded(220), 0)
        self.assertEqual(padded(1000), 0)

    def test_padding_never_exceeds_the_frame_cap(self):
        for n in range(0, 220):
            self.assertLessEqual(padded(n) + FRAME_OVERHEAD, 250)


if __name__ == "__main__":
    unittest.main()
