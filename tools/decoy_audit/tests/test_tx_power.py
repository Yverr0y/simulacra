import os, re, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
MAIN = os.path.abspath(os.path.join(HERE, "..", "..", "..", "main"))


def src(name):
    with open(os.path.join(MAIN, name), encoding="utf-8", errors="replace") as f:
        return f.read()


class TxPowerSentinelDoesNotCollideWithZeroDbm(unittest.TestCase):
    """0 dBm is a real power level and must not mean "use maximum".

    identity_t.tx_power used 0 as the "controller default" sentinel. dither_tx()'s ladder contained
    0, so 1 in 6 decoys transmitted at controller MAXIMUM instead of 0 dBm -- and ble_device_sync
    set 0 for every persona, so ALL persona decoys ran at max. Personas are roughly a third of the
    crowd. That pinned a large coherent slice of the population to the loud end of the RSSI
    distribution and narrowed its spread, which is the opposite of what a real crowd looks like and
    the exact axis being measured when it was found (decoy median -58 dBm vs ambient -68 to -71).
    """

    def test_dedicated_sentinel_exists(self):
        i = src("identity.h")
        self.assertIn("IDENTITY_TX_DEFAULT", i,
                      "the tx_power sentinel is gone; if it reverts to 0 then 0 dBm silently means "
                      "maximum output again.")
        self.assertNotRegex(
            i, r"0\s*=\s*controller default",
            "the header still documents 0 as the sentinel, which is the collision itself")

    def test_churn_adv_guards_on_the_sentinel_not_zero(self):
        c = src("churn_adv.c")
        self.assertNotRegex(
            c, r"id->tx_power\s*!=\s*0\s*\)",
            "churn_adv compares tx_power against 0 again, so an identity legitimately set to 0 dBm "
            "is promoted to controller maximum.")
        self.assertIn("IDENTITY_TX_DEFAULT", c, "churn_adv no longer uses the dedicated sentinel")

    def test_personas_get_a_real_dithered_level(self):
        b = src("ble_devices.c")
        sync = b.split("int ble_device_sync", 1)[-1].split("\n}", 1)[0]
        self.assertNotRegex(
            sync, r"d->id\.tx_power\s*=\s*0\s*;",
            "every persona is back to the sentinel, i.e. maximum output. Personas are ~1/3 of the "
            "crowd; pinning them all to the loud end is a population-wide RSSI tell.")
        self.assertIn("esp_random", sync, "persona tx power must be dithered, not fixed")

    def test_dither_never_returns_the_sentinel(self):
        g = src("generate.c")
        body = g.split("static int8_t dither_tx", 1)[-1].split("\n}", 1)[0]
        self.assertIn("IDENTITY_TX_DEFAULT", body,
                      "dither_tx must not be able to hand back the sentinel; doing so would put "
                      "that identity at maximum output.")


class TxPowerSpreadIsLearned(unittest.TestCase):
    """Spread is drawn from observed ambient RSSI, not fixed.

    Measured: ambient across-device RSSI sd is a stable 12.3-14.6 across three decoy-free captures,
    while the decoy population sat at 9.90 -- identities clustering ~25-30% tighter than a real
    crowd, which is the "one emitter, many costumes" tell. The bench flattered it: ~8.5 dB of that
    9.90 came from three boards being physically apart with the sniffer among them, and at realistic
    distance they collapse toward one point leaving only dither_tx's own spread.

    Deliberately NOT a blanket widening. The 2026-08-25 baseline's ambient RSSI is CONCENTRATED
    (92% inside two 10 dB bins), so spreading hard would have been wrong there. The right spread
    depends on the room, which is the argument for learning it.
    """

    def test_dither_samples_the_model(self):
        g = src("generate.c")
        self.assertRegex(g, r"static int8_t dither_tx\(const rf_model_t \*m\)",
                         "dither_tx must receive the model to sample observed RSSI shape")
        body = g.split("static int8_t dither_tx", 1)[-1].split("\n}", 1)[0]
        self.assertIn("rssi_bins", body, "dither no longer samples the learned RSSI shape")

    def test_a_cold_start_spread_still_exists(self):
        """A board with no observations yet must still emit a plausible spread."""
        g = src("generate.c")
        body = g.split("static int8_t dither_tx", 1)[-1].split("\n}", 1)[0]
        self.assertRegex(body, r"else\s*\{[\s\S]*?esp_random",
                         "no cold-start branch: a fresh boot would emit one fixed power level")

    def test_range_stays_within_radio_capability(self):
        g = src("generate.c")
        lo = int(re.search(r"#define\s+TX_MIN_DBM\s+\((-?\d+)\)", g).group(1))
        hi = int(re.search(r"#define\s+TX_MAX_DBM\s+\((-?\d+)\)", g).group(1))
        self.assertGreaterEqual(lo, -30, "below ESP32 BLE TX capability")
        self.assertLessEqual(hi, 20, "above ESP32 BLE TX capability")
        self.assertGreater(hi - lo, 15,
                           "TX range is no wider than the original 15 dB ladder, which produced a "
                           "population ~25-30% tighter in RSSI than a real crowd")


if __name__ == "__main__":
    unittest.main()
