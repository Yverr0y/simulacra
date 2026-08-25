import os, re, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
MAIN = os.path.join(ROOT, "main")


def define(path, name):
    """Read a simple '#define NAME <int>' out of a header."""
    with open(os.path.join(MAIN, path), encoding="utf-8", errors="replace") as f:
        m = re.search(rf"^#define\s+{name}\s+(\d+)", f.read(), re.M)
    assert m, f"{name} not found in {path}"
    return int(m.group(1))


class FleetMacCapacity(unittest.TestCase):
    """The fleet self-exclusion table must be able to hold what an additive fleet actually emits.

    Boards became additive on 2026-08-24: each one sizes its own crowd rather than running 1/K of a
    shared one, so a single board now emits far more identities than when these constants were set.
    When the table is too small, peer MACs fall out of it and get counted as REAL ambient devices,
    which inflates the density estimate AUTO sizes the crowd from - a feedback loop that saturated
    the bench fleet at its ceiling within an hour.
    """

    def test_table_holds_a_multi_board_fleet(self):
        per_board = define("ble_devices.h", "BLE_DEVICES_MAX") + \
                    define("probe_agents.h", "PROBE_AGENTS_MAX")
        cap = define("fleet.h", "FLEET_MAC_CAP")
        # At least 3 peers' worth: the in-hand fleet is 3 decoys + a Vigil, so any decoy hears 2
        # peers today, and headroom matters because rotation churns the live set.
        self.assertGreaterEqual(
            cap, 3 * per_board,
            f"FLEET_MAC_CAP ({cap}) cannot hold 3 peers x {per_board} identities. Unexcluded "
            f"fleetmate MACs are counted as real ambient devices and inflate the AUTO crowd.")

    def test_broadcast_chunk_fits_an_espnow_frame(self):
        # [uint8 count][count*6] plus the 32-byte seal overhead must stay inside 250.
        n = define("fleet.h", "FLEET_BCAST_MACS_MAX")
        self.assertLessEqual(1 + n * 6 + 32, 250,
                             f"FLEET_BCAST_MACS_MAX ({n}) overflows the 250-byte ESP-NOW frame")

    def test_broadcast_is_chunked_not_truncated(self):
        """A board emits more identities than one frame holds, so the sender MUST loop.

        The single-frame version silently dropped everything past the first 32 - and because BLE
        was packed first, a full BLE crowd meant ZERO Wi-Fi probe MACs were ever advertised.
        """
        per_board = define("ble_devices.h", "BLE_DEVICES_MAX") + \
                    define("probe_agents.h", "PROBE_AGENTS_MAX")
        chunk = define("fleet.h", "FLEET_BCAST_MACS_MAX")
        self.assertGreater(per_board, chunk,
                           "premise check: a board should exceed one frame, else chunking is moot")
        with open(os.path.join(MAIN, "esp_now_link.c"), encoding="utf-8", errors="replace") as f:
            src = f.read()
        self.assertIn("fleet_macs_send_chunk", src,
                      "broadcast_fleet_macs must chunk; a single frame truncates at "
                      f"{chunk} of {per_board} identities")
        # The old truncating loop guarded the fill with 'n < FLEET_BCAST_MACS_MAX' as a STOP
        # condition. Chunking flushes instead, so that guard must be gone from the collectors.
        self.assertNotIn("&& n < FLEET_BCAST_MACS_MAX", src,
                         "collector still stops at one frame instead of flushing a chunk")


if __name__ == "__main__":
    unittest.main()
