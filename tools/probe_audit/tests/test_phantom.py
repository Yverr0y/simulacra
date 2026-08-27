import os, subprocess, unittest
from collections import Counter

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "probe_dump.exe" if os.name == "nt" else "probe_dump")

# Must match phantom.h enum order.
FAM = {0: "samsung", 1: "google", 2: "apple", 3: "generic"}
# family -> (expected arch idx, expected BLE company).
# ARCH_R_VS=0, R_EC15=1, R_HE=2, R_BARE=3, R_HTONLY=4.
#
# The archetype half is a STABLE ARBITRARY assignment, not a vendor claim: archetypes are captured
# IE structures, and a capture can only say a layout exists and how common it is -- never which
# handset emitted it. What this test pins is that the map is total and FIXED, because a persona's
# BLE company id and its Wi-Fi structure must not drift apart inside one life.
# PF_GENERIC moved 3 -> 4 on 2026-08-26 when ARCH_R_BARE was inserted ahead of ARCH_R_HTONLY.
EXPECT = {0: (1, 0x0075), 1: (2, 0x00E0), 2: (0, 0x0000), 3: (4, 0x0000)}


def phantoms(seed, n=12, ticks=4000, tick_ms=1000):
    out = subprocess.check_output([EXE, "--phantoms", str(seed), str(n), str(ticks), str(tick_ms)], text=True)
    rows = []
    for ln in out.splitlines():
        p = ln.split()
        if len(p) == 7 and p[0] == "P":
            # P <t> <idx> <family> <arch> <company> <generation> (generation ignored here)
            rows.append((int(p[1]), int(p[2]), int(p[3]), int(p[4]), int(p[5], 16)))
    return rows


@unittest.skipUnless(os.path.exists(EXE), "probe_dump not built")
class Phantom(unittest.TestCase):
    def test_family_maps_to_arch_and_company(self):
        rows = phantoms(1)
        self.assertTrue(rows, "no phantom events")
        for _t, _i, fam, arch, comp in rows:
            exp_arch, exp_comp = EXPECT[fam]
            self.assertEqual(arch, exp_arch, f"family {FAM[fam]} wrong arch")
            self.assertEqual(comp, exp_comp, f"family {FAM[fam]} wrong company")

    def test_all_families_appear_over_time(self):
        fams = Counter(r[2] for r in phantoms(3))   # r[2] = family
        # over thousands of reincarnations the weighted draw should hit every family
        self.assertEqual(set(fams), {0, 1, 2, 3}, f"some family never drawn: {dict(fams)}")


if __name__ == "__main__":
    unittest.main()
