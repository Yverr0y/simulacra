import os, re, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
MAIN = os.path.abspath(os.path.join(HERE, "..", "..", "..", "main"))


def src(name):
    with open(os.path.join(MAIN, name), encoding="utf-8", errors="replace") as f:
        return f.read()


class ProbeArchetypesAreCaptureDerived(unittest.TestCase):
    """Wi-Fi probe structure was the fleet's cleanest give-away.

    The shipped archetypes were MODELED from documentation, and a census of a decoy-free capture
    (877 probing devices, 159 distinct IE structures) found NONE of the eight tails present even
    once. Every probe the fleet emitted therefore carried a structure existing nowhere in ambient,
    which classifies the fleet perfectly no matter how well the source MAC is randomised. Replaced
    2026-08-26 with modal byte content from that capture; all eight now match real devices.
    """

    def test_modeled_stand_ins_are_gone(self):
        p = src("probe_frame.c")
        for macro in ("RATES_24", "RATES_5", "HT_A", "HT_B", "EXTCAP_APPLE", "VS_APPLE",
                      "VS_BROADCOM", "VS_WPS"):
            self.assertNotIn(
                "#define " + macro, p,
                f"{macro} is back: the modeled building blocks produced structures that appear in "
                "no real capture. Being modeled was the defect, so there is nothing to restore.")
        for name in ("IPHONE_24", "GALAXY_24", "PIXEL_24", "ANDROID_24"):
            self.assertNotIn(name + "[]", p, f"modeled archetype {name} reintroduced")

    def test_enough_archetypes_to_stop_being_a_fixed_set(self):
        h = src("probe_frame.h")
        n = len(re.findall(r"^\s*ARCH_\w+,", h, re.M))
        self.assertGreaterEqual(n, 5,
                                f"only {n} probe archetypes. Four fixed IE layouts, published in "
                                "this repo, is a fingerprint on its own.")

    def test_some_archetypes_are_24_only(self):
        """Devices without a 5 GHz radio exist in quantity; the old table modeled none."""
        p = src("probe_frame.c")
        rows = re.findall(r"\[ARCH_\w+\]\s*=\s*\{[^}]*?\}", p, re.S)
        self.assertTrue(rows, "archetype table not parsed")
        only24 = [r for r in rows if re.search(r"NULL,\s*0", r)]
        self.assertTrue(only24,
                        "every archetype claims both bands. A crowd where no device is 2.4-only "
                        "does not exist, and it was that way because the tails were invented.")

    def test_wps_vendor_ie_is_not_embedded(self):
        """WPS (00:50:f2 subtype 04) can carry a device name or UUID -- never embed a captured one."""
        p = src("probe_frame.c")
        self.assertIsNone(
            re.search(r"0x00,\s*0x50,\s*0xf2,\s*0x04", p, re.I),
            "a WPS vendor IE is embedded in an archetype tail. WPS can carry a device name or "
            "UUID, so capture-derived tails must exclude it.")


if __name__ == "__main__":
    unittest.main()
