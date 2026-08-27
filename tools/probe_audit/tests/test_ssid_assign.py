import os, subprocess, unittest

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "probe_dump.exe" if os.name == "nt" else "probe_dump")
AGENT_SSID_MAX = 3


def burst(seed, n=16, bursts=60):
    out = subprocess.check_output([EXE, "--ssidburst", str(seed), str(n), str(bursts)], text=True)
    return [tuple(int(x) for x in ln.split()) for ln in out.splitlines()]   # (agent, ssid_n, named)


def stable(seed, n=16):
    out = subprocess.check_output([EXE, "--ssidstable", str(seed), str(n)], text=True)
    before, after = {}, {}
    for ln in out.splitlines():
        p = ln.split()
        tag, i, ssid_n = p[0], int(p[1]), int(p[2])
        idx = tuple(int(x) for x in p[3:3 + ssid_n])
        mac = p[3 + ssid_n]
        (before if tag == "B" else after)[i] = (ssid_n, idx, mac)
    return before, after


def pool_count():
    return int(subprocess.check_output([EXE, "--ssidpool"], text=True).splitlines()[0])


@unittest.skipUnless(os.path.exists(EXE), "probe_dump not built")
class SsidAssign(unittest.TestCase):
    def test_assignment_fraction_near_calibration(self):
        # Pool many agents across seeds; ~21% should have ssid_n>0 (SSID_ASSIGN_PCT).
        # Recalibrated 2026-08-26 from 62% to the censused 21.2% of real devices that ever name a
        # network. Real crowds split sharply into "never names" and "names almost every burst";
        # 62% modelled the middle of that distribution, a shape no real crowd exhibits.
        rows = [r for s in range(1, 9) for r in burst(s)]
        frac = sum(1 for _, ssid_n, _ in rows if ssid_n > 0) / len(rows)
        self.assertGreater(frac, 0.14, f"assigned fraction {frac:.2f} too low")
        self.assertLess(frac, 0.29, f"assigned fraction {frac:.2f} too high")

    def test_assigned_count_bounded(self):
        for _, ssid_n, _ in burst(2):
            self.assertGreaterEqual(ssid_n, 0)
            self.assertLessEqual(ssid_n, AGENT_SSID_MAX)

    def test_assigned_indices_valid_and_distinct(self):
        pc = pool_count()
        before, _ = stable(5)
        checked = 0
        for i, (ssid_n, idx, _mac) in before.items():
            self.assertEqual(len(idx), ssid_n, f"agent {i} idx count mismatch")
            self.assertEqual(len(set(idx)), len(idx), f"agent {i} has duplicate SSID indices")
            for x in idx:
                self.assertTrue(0 <= x < pc, f"agent {i} index {x} out of pool range")
            checked += ssid_n
        self.assertGreater(checked, 0, "no assigned indices to validate")

    def test_unassigned_never_names_assigned_sometimes(self):
        rows = burst(4, bursts=80)
        for _, ssid_n, named in rows:
            if ssid_n == 0:
                self.assertEqual(named, 0, "wildcard-only agent emitted a named probe")
        assigned = [named for _, ssid_n, named in rows if ssid_n > 0]
        self.assertTrue(assigned, "no assigned agents to check")
        self.assertTrue(any(nm > 0 for nm in assigned), "assigned agents never named over 80 bursts")

    def test_assignment_redrawn_on_mac_rotation(self):
        """A persona's saved-network set must NOT survive its MAC rotation.

        This test asserted the OPPOSITE until 2026-08-26, per the 2026-07-22 directed-probe design:
        a real phone's saved-network list is a property of the device, not of a MAC rotation, so
        carrying it across was the realistic choice.

        It was reversed under the no-persistent-identifiers rule. A saved-network set that outlives
        a rotation IS a persistent identifier, and a strong one: it is the standard way MAC
        randomisation gets defeated in practice. Two MACs probing for the same set are one device.

        The realism cost is close to zero, and the reason is worth stating because it does not
        generalise: the "device keeps its saved networks" property is only observable to someone who
        has ALREADY linked the two MACs -- and the SSID set is what does the linking. Remove it and
        there is no vantage point from which the change is visible. The tell erases itself.

        Residual, known and accepted: aggregate SSID-to-MAC multiplicity. A real crowd shows one
        SSID probed by several MACs over an hour; redrawing every rotation pushes the fleet toward
        one MAC per SSID. The 38-name pool blunts this -- with far more personas than names,
        collisions occur naturally -- but it does not erase it.
        """
        had = kept = rotated = 0
        for seed in range(1, 25):
            before, after = stable(seed)
            for i, (bn, bidx, bmac) in before.items():
                an, aidx, amac = after[i]
                if amac == bmac:
                    continue                       # no rotation, nothing to assert
                rotated += 1
                if bn > 0:
                    had += 1
                    if (an, aidx) == (bn, bidx):
                        kept += 1
        self.assertGreater(rotated, 0, "no agent rotated its MAC (test exercised nothing)")
        self.assertGreater(had, 8, "too few assigned agents rotated to measure carry-over")
        # Independent redraw at SSID_ASSIGN_PCT=21 over a 38-name pool keeps an identical set only
        # by coincidence: 0.21 * (1/38) ~= 0.6%. Carry-over would pin this at 100%. Anything above
        # a third means the set is following the persona across the rotation again.
        self.assertLess(kept / had, 0.34,
                        f"{kept}/{had} personas kept their SSID set across a MAC rotation -- the set "
                        f"is linking the two MACs, which is the handle the rotation exists to break")


if __name__ == "__main__":
    unittest.main()
