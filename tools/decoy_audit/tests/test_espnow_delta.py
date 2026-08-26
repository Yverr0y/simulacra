import os, re, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
MAIN = os.path.abspath(os.path.join(HERE, "..", "..", "..", "main"))


def src(name):
    with open(os.path.join(MAIN, name), encoding="utf-8", errors="replace") as f:
        return f.read()


def define(path, name):
    m = re.search(rf"^#define\s+{name}\s+(\d+)", src(path), re.M)
    assert m, f"{name} not found in {path}"
    return int(m.group(1))


class DeltaBroadcastTtlContract(unittest.TestCase):
    """Delta broadcasts only work if the TTL outlives the RESYNC period, not the sweep period.

    Sending the whole MAC table every 20-30 s was 88% of the fleet's ESP-NOW traffic, and the
    2026-08-25 capture showed each board emitting ~25 vendor action frames/min against a median of
    0.000 from 206 ambient devices. Deltas cut that, but they change the TTL contract: an unchanged
    MAC is no longer re-sent, so it must survive on the full-resync cadence instead. Get this
    relationship wrong and live fleetmate MACs expire mid-life, which is the population feedback
    loop (32 -> 65 -> 33 -> 42, ambient provably flat).
    """

    def _resync_bounds_ms(self):
        c = src("esp_now_link.c")
        m = re.search(r"resync_period\s*=\s*(\d+)\s*\+\s*esp_random\(\)\s*%\s*(\d+)", c)
        self.assertIsNotNone(m, "resync period not found in esp_now_link.c")
        base, span = int(m.group(1)), int(m.group(2))
        return base, base + span - 1

    def test_mac_ttl_covers_resync_with_3x_margin(self):
        ttl = define("fleet.h", "FLEET_MAC_TTL_MS")
        _lo, hi = self._resync_bounds_ms()
        self.assertGreaterEqual(
            ttl, 3 * hi,
            f"FLEET_MAC_TTL_MS ({ttl} ms) must be >= 3x the worst-case resync period ({hi} ms). "
            "Deltas do not refresh unchanged MACs, so the resync is the only thing keeping a live "
            "fleetmate MAC in a peer's table.")

    def test_node_ttl_is_separate_and_responsive(self):
        node = define("fleet.h", "FLEET_NODE_TTL_MS")
        mac = define("fleet.h", "FLEET_MAC_TTL_MS")
        self.assertLess(node, mac,
                        "node liveness must expire faster than MAC exclusion; sharing the raised "
                        "MAC TTL would make a departed peer look present for minutes")
        self.assertLessEqual(node, 180000, f"FLEET_NODE_TTL_MS ({node}) is too slow to notice a "
                                           "board leaving")

    def test_node_table_does_not_use_the_mac_ttl(self):
        f = src("fleet.c")
        node_half = f.split("fleet_note_peer_node", 1)[-1]
        self.assertNotIn("FLEET_MAC_TTL_MS", node_half,
                         "the node table still ages entries on FLEET_MAC_TTL_MS")


class DeltaAndPacing(unittest.TestCase):
    def test_broadcast_is_delta_capable(self):
        c = src("esp_now_link.c")
        self.assertRegex(c, r"broadcast_fleet_macs\(\s*bool\s+full\s*\)",
                         "broadcast_fleet_macs must take a full/delta flag")
        self.assertRegex(c, r"broadcast_fleet_macs\(false\)",
                         "nothing ever sends a delta; every sweep is a full table again")
        self.assertRegex(c, r"broadcast_fleet_macs\(true\)",
                         "nothing ever resyncs; peers would expire MACs that never change")

    def test_chunks_are_paced_not_back_to_back(self):
        """The old loop emitted every chunk with no delay, a fixed ~20 ms machine-timed cadence."""
        c = src("esp_now_link.c")
        self.assertIn("fleet_macs_pump", c, "the paced chunk pump is gone")
        # The pump must release AT MOST one chunk per call and schedule the next with jitter.
        pump = c.split("static void fleet_macs_pump", 1)[-1].split("\n}", 1)[0]
        self.assertIn("esp_random()", pump, "chunk spacing must be jittered, not fixed")
        self.assertNotRegex(pump, r"for\s*\(|while\s*\(",
                            "fleet_macs_pump loops, so it still emits a burst; it must release one "
                            "chunk per call")

    def test_delta_baseline_is_dropped_when_unkeyed(self):
        """After re-enrolment no peer shares our baseline, so the next sweep must be full."""
        c = src("esp_now_link.c")
        unkeyed = c.split("if (!fleet_key_have())", 1)[-1].split("continue;", 1)[0]
        self.assertIn("s_sent_n", unkeyed,
                      "the delta baseline is not reset while unkeyed; the first sweep after "
                      "enrolment would send a delta against state no peer has.")

    def test_identity_buffer_holds_a_full_board(self):
        c = src("esp_now_link.c")
        self.assertRegex(
            c, r"#define\s+FLEET_IDENT_MAX\s+\(\s*2\s*\*\s*BLE_DEVICES_MAX\s*\+\s*PROBE_AGENTS_MAX\s*\)",
            "FLEET_IDENT_MAX must cover live BLE addrs + pre-drawn next addrs + probe agents")


if __name__ == "__main__":
    unittest.main()
