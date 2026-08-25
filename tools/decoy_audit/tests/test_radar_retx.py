import os, subprocess, unittest

HERE = os.path.dirname(os.path.abspath(__file__)); TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "synth_dump.exe" if os.name == "nt" else "synth_dump")


def fire_times(repeats, seed, horizon_ms=5000):
    """ms timestamps at which radar_retx_due fired, ticking 1 ms at a time."""
    out = subprocess.check_output(
        [EXE, "--retx", str(repeats), str(seed), str(horizon_ms)], text=True)
    return [int(x) for x in out.split()]


def adapt(start, sequence):
    """sequence: string of '1' (all answered) / '0' (someone missed)."""
    out = subprocess.check_output([EXE, "--retxadapt", str(start), sequence], text=True)
    return [int(x) for x in out.split()]


@unittest.skipUnless(os.path.exists(EXE), "synth_dump not built")
class RadarRetx(unittest.TestCase):
    def test_fires_exactly_the_requested_number_of_times(self):
        for n in (1, 2, 3, 4):
            self.assertEqual(len(fire_times(n, 1)), n, f"expected {n} sends")

    def test_first_send_is_immediate(self):
        self.assertEqual(fire_times(4, 1)[0], 0, "first repeat should go out at once")

    def test_repeats_are_spread_not_back_to_back(self):
        # The whole point: the old path sent 4 identical frames inside ~20 ms, a recognizable
        # retransmit train that survives encryption and length bucketing alike.
        for seed in range(1, 6):
            t = fire_times(4, seed)
            gaps = [b - a for a, b in zip(t, t[1:])]
            self.assertTrue(all(g >= 40 for g in gaps), f"seed {seed}: gaps {gaps}")
            self.assertTrue(all(g <= 120 for g in gaps), f"seed {seed}: gaps {gaps}")

    def test_gaps_are_jittered_not_constant(self):
        # A fixed 80 ms spacing would just be a slower metronome.
        seen = set()
        for seed in range(1, 12):
            t = fire_times(4, seed)
            seen.update(b - a for a, b in zip(t, t[1:]))
        self.assertGreater(len(seen), 3, f"gaps barely varied: {sorted(seen)}")

    def test_zero_repeats_never_fires(self):
        self.assertEqual(fire_times(0, 1), [])


@unittest.skipUnless(os.path.exists(EXE), "synth_dump not built")
class RetxAdapt(unittest.TestCase):
    def test_relaxes_one_step_per_clean_cycle(self):
        self.assertEqual(adapt(4, "111"), [3, 2, 1])

    def test_never_relaxes_below_one(self):
        self.assertEqual(adapt(4, "111111"), [3, 2, 1, 1, 1, 1])

    def test_any_miss_resets_to_max_immediately(self):
        # Asymmetric on purpose: slow to relax, immediate to recover. An unheard REQUEST costs a
        # stale console; an extra frame costs only exposure.
        self.assertEqual(adapt(4, "1110"), [3, 2, 1, 4])

    def test_recovers_from_the_floor(self):
        self.assertEqual(adapt(1, "0"), [4])


if __name__ == "__main__":
    unittest.main()
