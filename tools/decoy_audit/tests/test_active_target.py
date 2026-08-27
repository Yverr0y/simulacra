import os, subprocess, unittest

HERE = os.path.dirname(os.path.abspath(__file__)); TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "synth_dump.exe" if os.name == "nt" else "synth_dump")


def target(pop):
    out = subprocess.check_output([EXE, "--acttarget", str(pop)], text=True)
    return int(out.strip())


@unittest.skipUnless(os.path.exists(EXE), "synth_dump not built")
class ActiveTarget(unittest.TestCase):
    """Ambient-derived crowd size, post-2026-08-24 additive-population change.

    The host build compiles without CONFIG_IDF_TARGET_ESP32C5, so it takes the #else
    (Shade) profile: GEN_FACTOR 1.1x, GEN_FLOOR 4. Assertions below are written to hold
    for either profile so the suite is not silently target-specific.
    """

    def test_floor_holds_in_empty_room(self):
        # pop 0 must not yield 0 devices; GEN_FLOOR is 6 on C5 / 4 on C6
        self.assertGreaterEqual(target(0), 4)

    def test_scales_with_density(self):
        # the whole point: a denser room yields a bigger crowd
        self.assertGreater(target(24), target(8))

    def test_exceeds_the_old_16_clamp(self):
        # regression guard: CHURN_ACTIVE_SET (16) and GEN_CEILING must no longer bind
        self.assertGreater(target(40), 16)

    def test_saturates_at_hardware_max(self):
        # BLE_DEVICES_MAX is 32; nothing may exceed the static array size
        self.assertLessEqual(target(500), 32)
        self.assertEqual(target(500), 32)


if __name__ == "__main__":
    unittest.main()
