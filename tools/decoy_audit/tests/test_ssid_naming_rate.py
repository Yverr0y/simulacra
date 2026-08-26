import os, re, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
MAIN = os.path.abspath(os.path.join(HERE, "..", "..", "..", "main"))


def src(name):
    with open(os.path.join(MAIN, name), encoding="utf-8", errors="replace") as f:
        return f.read()


def define(name, path="probe_agents.c"):
    m = re.search(rf"^#define\s+{name}\s+(\d+)", src(path), re.M)
    assert m, f"{name} not found"
    return int(m.group(1))


class NamedProbeRateMatchesAmbient(unittest.TestCase):
    """How OFTEN the fleet names a network, measured against a decoy-free capture.

    877 probing devices / 2229 probe requests gave: 21.2% of devices ever name anything, a namer's
    probes are 78.4% directed, and a namer holds 1.05 distinct networks. The shipped values had the
    shape inverted -- 62% of personas naming, only 60% of their bursts, with 1..3 networks each --
    which models the middle of a distribution that is actually bimodal: real devices either never
    name, or name almost every time and look for exactly one network.
    """

    def test_share_of_personas_that_name_matches_measurement(self):
        v = define("SSID_ASSIGN_PCT")
        self.assertLessEqual(abs(v - 21), 5,
                             f"SSID_ASSIGN_PCT is {v}; measured share of devices that ever name a "
                             "network is 21.2%.")

    def test_a_namer_names_most_of_the_time(self):
        v = define("SSID_BURST_NAMED_PCT")
        self.assertGreaterEqual(v, 70,
                                f"SSID_BURST_NAMED_PCT is {v}; a real naming device sends 78.4% of "
                                "its probes directed. Naming rarely is not what namers do.")

    def test_saved_network_set_is_essentially_one(self):
        a = src("probe_agents.c")
        self.assertNotRegex(
            a, r"want\s*=\s*1\s*\+\s*\(int\)\(esp_random\(\)\s*%\s*\(uint32_t\)AGENT_SSID_MAX\)",
            "the flat 1..AGENT_SSID_MAX draw is back (mean 2). Measured mean is 1.05 distinct "
            "networks per naming device.")
        self.assertIn("SSID_SECOND_NET_PCT", a, "the rare-second-network knob is gone")
        self.assertLessEqual(define("SSID_SECOND_NET_PCT"), 15,
                             "a second saved network should be rare (measured mean 1.05)")


class NothingSurvivesAMacRotation(unittest.TestCase):
    """Anything carried across a rotation links the old MAC to the new one.

    The sequence counter was already re-randomised on rotation for exactly this reason. The
    saved-network set was not, so an agent kept probing the same pool name plus its stable per-agent
    suffix ("spectrumsetup-a3") -- letting an observer stitch the two identities together through
    the SSID string and defeating the rotation for any agent that names a network.

    Real phones DO carry saved networks across a MAC rotation. That is the well-known randomisation
    defeat this project exists not to reproduce; realism loses to unlinkability, as it did for the
    persistent identity band.
    """

    def _rotation_branch(self):
        a = src("probe_agents.c")
        i = a.index("next_mac_rotate_ms) >= 0")
        return a[i:a.index("rotated++", i)]

    def test_mac_is_redrawn(self):
        self.assertIn("probe_random_mac", self._rotation_branch(),
                      "rotation no longer draws a new MAC")

    def test_sequence_counter_is_redrawn(self):
        self.assertIn("a->seq", self._rotation_branch(),
                      "the 802.11 sequence counter survives rotation, which links the two MACs")

    def test_saved_network_set_is_redrawn(self):
        self.assertIn("assign_ssids", self._rotation_branch(),
                      "the SSID set survives rotation. A directed probe carries a stable "
                      "per-agent suffixed name, so an observer links old MAC to new through it.")


class SsidPoolStaysGeneric(unittest.TestCase):
    """The pool may never be sourced from a capture. Real probed SSIDs are people's networks."""

    def test_pool_is_wider_than_the_original_22(self):
        n = len(re.findall(r'^\s*\{\s*"[^"]+",\s*\d+,\s*SSID_SFX_', src("ssid_pool.c"), re.M))
        self.assertGreaterEqual(n, 30,
                                f"pool has {n} entries. A small pool with published weights lets a "
                                "fleet's aggregate probe distribution be read out of this repo.")

    def test_pool_carries_no_capture_sourced_name(self):
        """Guard the one local-business name the census surfaced, and the shape of such names."""
        p = src("ssid_pool.c")
        names = re.findall(r'^\s*\{\s*"([^"]+)"', p, re.M)
        for n in names:
            self.assertNotIn("'", n, f"'{n}' looks possessive -- personal networks are forbidden")
            self.assertLessEqual(
                len(n.split()), 2,
                f"'{n}' reads like a business or personal SSID; the pool is manufacturer/ISP "
                "defaults and generic words only.")


if __name__ == "__main__":
    unittest.main()
