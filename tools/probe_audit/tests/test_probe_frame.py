import os, subprocess, unittest
from collections import Counter

HERE = os.path.dirname(__file__)
TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "probe_dump.exe" if os.name == "nt" else "probe_dump")


def build_arch(idx, ch, b5, ssid=None):
    args = [EXE, str(idx), str(ch), str(b5)]
    if ssid is not None:
        args.append(ssid)
    out = subprocess.check_output(args, text=True).strip()
    return bytes.fromhex(out)


def ies(frame):
    """{id: value_bytes} walking the IE body after the 24-byte MAC header (which includes seq)."""
    body, i, out = frame[24:], 0, {}
    while i + 2 <= len(body):
        eid, ln = body[i], body[i + 1]
        out.setdefault(eid, body[i + 2:i + 2 + ln])
        i += 2 + ln
    return out


def fixture(name):
    p = os.path.join(TOOL, "fixtures", name)
    with open(p) as fh:
        line = [l for l in fh if not l.startswith("#")][0].strip()
    return bytes.fromhex(line)


# (name, arch_idx, channel, band5, fixture_file)
#
# Capture-derived archetypes (2026-08-26). The previous entries were MODELLED from documentation,
# and a census of 877 real probing devices found NONE of those eight tails present even once -- so
# every probe the fleet emitted carried a structure existing nowhere in ambient, which classifies
# the fleet regardless of how well the source MAC is randomised. These are the modal byte content
# of the most common real structures instead.
#
# ARCH_R_BARE and ARCH_R_HTONLY are 2.4-ONLY by design: they model devices with no 5 GHz radio,
# which exist in quantity and which an invented table never produces.
CASES = [
    ("r-vs",     0, 6, 0, "r-vs_24.hex"),     ("r-vs",   0, 36, 1, "r-vs_5.hex"),
    ("r-ec15",   1, 6, 0, "r-ec15_24.hex"),   ("r-ec15", 1, 36, 1, "r-ec15_5.hex"),
    ("r-he",     2, 6, 0, "r-he_24.hex"),     ("r-he",   2, 36, 1, "r-he_5.hex"),
    ("r-bare",   3, 6, 0, "r-bare_24.hex"),
    ("r-htonly", 4, 6, 0, "r-htonly_24.hex"),
]


@unittest.skipUnless(os.path.exists(EXE), "probe_dump not built")
class ProbeFrame(unittest.TestCase):
    def test_valid_probe_and_law3(self):
        for name, idx, ch, b5, _ in CASES:
            f = build_arch(idx, ch, b5)
            self.assertEqual(f[0:2], b"\x40\x00", f"{name} FC probe-req")
            self.assertEqual(f[4:10], b"\xff" * 6, f"{name} DA broadcast")
            self.assertEqual(f[16:22], b"\xff" * 6, f"{name} BSSID broadcast")
            d = ies(f)
            self.assertIn(0x00, d, f"{name} SSID present")
            self.assertEqual(len(d[0x00]), 0, f"{name} SSID must be wildcard (Law 3)")
            # DS Parameter Set is OPTIONAL, not universal. Half the captured structures omit it
            # entirely, which is what real devices do -- and omitting it is arguably better than
            # carrying one, since a stale DS channel contradicts the channel actually transmitted
            # on. Where it IS present it must be patched to the TX channel.
            if 0x03 in d:
                self.assertEqual(d[0x03], bytes([ch]), f"{name} DS channel patched")
            self.assertLessEqual(len(f), 256, f"{name} within PROBE_FRAME_MAX")

    def test_capability_ies_show_diversity(self):
        """Capability IEs must VARY across archetypes, not be present in all of them.

        The old assertion was "HT everywhere", which held only because every modelled tail was
        IE-heavy. Real crowds are not: the single commonest structure on air is rates plus one
        vendor IE, and several carry no HT at all. Uniform richness was itself the tell, so the
        property worth pinning is spread -- some archetypes rich, some terse.
        """
        seen_ht = seen_he = seen_vht = seen_terse = 0
        for name, idx, ch, b5, _ in CASES:
            d = ies(build_arch(idx, ch, b5))
            if 0x2d in d: seen_ht += 1
            if 0xff in d: seen_he += 1
            if 0xbf in d: seen_vht += 1
            if 0x2d not in d and 0xff not in d: seen_terse += 1
        self.assertGreater(seen_ht, 0, "no archetype carries HT caps")
        self.assertGreater(seen_he, 0, "no archetype carries HE")
        self.assertGreater(seen_vht, 0, "no archetype carries VHT on 5 GHz")
        self.assertGreater(seen_terse, 0,
                           "every archetype is IE-rich. The commonest real structure is rates plus "
                           "one vendor IE; a uniformly rich crowd is the shape that got the "
                           "modelled tails caught.")
        self.assertLess(seen_ht, len(CASES), "HT in EVERY archetype is the old uniform-richness tell")

    def test_directed_ssid_element_present(self):
        f = build_arch(0, 6, 0, "xfinitywifi")           # iphone 2.4, named
        d = ies(f)
        self.assertIn(0x00, d, "SSID element present")
        self.assertEqual(d[0x00], b"xfinitywifi", "directed SSID bytes emitted")
        self.assertEqual(d[0x03], bytes([6]), "DS channel still patched with a directed SSID")
        self.assertLessEqual(len(f), 256)

    def test_directed_body_matches_wildcard_after_ssid(self):
        # Everything after the SSID element must be identical to the wildcard frame's body-after-SSID.
        wild = build_arch(0, 6, 0)
        named = build_arch(0, 6, 0, "attwifi")
        # wildcard SSID element is 2 bytes (0x00,0x00) at body offset 0; directed is 2+len(name).
        wild_after  = wild[24 + 2:]
        named_after = named[24 + 2 + len("attwifi"):]
        self.assertEqual(named_after, wild_after, "IE body after SSID diverged")

    def test_matches_fixture(self):
        for name, idx, ch, b5, fx in CASES:
            self.assertEqual(build_arch(idx, ch, b5), fixture(fx), f"{name} byte-exact fixture")

    def test_weighted_pick_distribution(self):
        # Weights track measured prevalence in the 877-device census, not a guess:
        #   r-vs 30, r-ec15 25, r-he 25, r-bare 12, r-htonly 8  (sum 100, so weight == percent).
        # The two 2.4-only structures matter disproportionately: an invented archetype table never
        # produces a device with no 5 GHz radio, and real crowds are full of them.
        EXPECT = {0: 0.30, 1: 0.25, 2: 0.25, 3: 0.12, 4: 0.08}
        out = subprocess.check_output([EXE, "--pick", "7", "4000"], text=True).split()
        c = Counter(int(x) for x in out); n = len(out)
        self.assertEqual(set(c) - set(EXPECT), set(), "only valid archetype indices")
        self.assertEqual(len(c), len(EXPECT), "every archetype appears")
        for idx, want in EXPECT.items():
            got = c[idx] / n
            self.assertAlmostEqual(got, want, delta=0.04,
                                   msg=f"arch {idx} drew {got:.3f}, weight says {want:.2f}")
        self.assertGreater(c[0], c[1], "r-vs is the plurality (commonest real structure)")


if __name__ == "__main__":
    unittest.main()
