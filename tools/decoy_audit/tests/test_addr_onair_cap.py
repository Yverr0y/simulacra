import os, re, sys, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
MAIN = os.path.join(ROOT, "main")


def src(name):
    with open(os.path.join(MAIN, name), encoding="utf-8", errors="replace") as f:
        return f.read()


def define(path, name):
    m = re.search(rf"^#define\s+{name}\s+(\d+)", src(path), re.M)
    assert m, f"{name} not found in {path}"
    return int(m.group(1))


class NoPersistentIdentifiers(unittest.TestCase):
    """No identifier this project emits may outlive the thing it is meant to cover.

    A decoy holding one address for hours, on a board carried by the operator, is a BETTER tracking
    handle than the phone it covers - phones rotate their RPA every ~15 min. That inverts the whole
    point, so the persistent/"infrastructure" role was removed on 2026-08-26 and a hard ceiling
    added. These tests exist because the pressure that created the persistent role is still there:
    presence_duration is a scored audit axis, and re-adding a long-lived band is the obvious way to
    close it. It must not be closed that way again.
    """

    def test_persistent_role_is_gone(self):
        for f in ("ble_devices.h", "ble_devices.c"):
            self.assertNotIn(
                "BLE_ROLE_PERSISTENT", src(f),
                f"{f} reintroduces BLE_ROLE_PERSISTENT. A decoy that holds one address for hours "
                "is a tracking handle, not cover.")

    def test_static_life_is_capped_to_the_onair_ceiling(self):
        c = src("ble_devices.c")
        self.assertIn("ADDR_MAX_ONAIR_MS", c, "the on-air ceiling is gone")
        # STATIC never rotates, so its address is on air for its entire life. The cap must be
        # applied to static lifetimes or the ceiling is decorative.
        self.assertRegex(
            c, r"BLE_ATYPE_STATIC\s*&&\s*d->life_ms\s*>\s*ADDR_MAX_ONAIR_MS",
            "static lifetimes are no longer clamped to ADDR_MAX_ONAIR_MS. STATIC never rotates "
            "(next_rotate_ms = 0), so an unclamped static life puts one address on air for its "
            "whole duration.")

    def test_ceiling_is_at_most_phone_rpa_rotation(self):
        cap = define("ble_devices.c", "ADDR_MAX_ONAIR_MS")
        self.assertLessEqual(cap, 900000,
                             f"ADDR_MAX_ONAIR_MS ({cap} ms) exceeds 15 min. Real phones rotate "
                             "their RPA at ~15 min; a decoy identity must not outlive that.")

    def test_rotation_bands_respect_the_ceiling(self):
        """Rotating subtypes are bounded by cadence, so every cadence must sit under the cap."""
        c = src("ble_devices.c")
        cap = define("ble_devices.c", "ADDR_MAX_ONAIR_MS")
        for name in ("NRPA_ROT_MAX_MS", "PERSONA_RPA_ROT_MAX_MS"):
            m = re.search(rf"^#define\s+{name}\s+(\d+)", c, re.M)
            if m:
                self.assertLessEqual(
                    int(m.group(1)), cap,
                    f"{name} ({m.group(1)}) exceeds ADDR_MAX_ONAIR_MS ({cap}); an address would "
                    "stay on air past the ceiling before rotating.")
        # RPA_ROT_MAX_MS is defined AS the cap, so assert the binding rather than a literal.
        self.assertRegex(c, r"#define\s+RPA_ROT_MAX_MS\s+ADDR_MAX_ONAIR_MS",
                         "RPA_ROT_MAX_MS must stay pinned to ADDR_MAX_ONAIR_MS")


class NoFixedPayloadConstants(unittest.TestCase):
    """Instance fields must be unique per spawn. A constant byte is a population-wide filter."""

    def test_beacon_tx_power_is_not_hardcoded(self):
        t = src("templates.c")
        body = t.split("static uint8_t rnd_tx_ref", 1)[-1]
        # 0xC5 may still appear in the explanatory comment; it must not appear as an assignment.
        for pat in (r"mfg\[24\]\s*=\s*0x[0-9A-Fa-f]{2}\s*;",
                    r"sd\[3\]\s*=\s*0x[0-9A-Fa-f]{2}\s*;",
                    r"sd\[n\+\+\]\s*=\s*0xC5\s*;"):
            self.assertIsNone(
                re.search(pat, body),
                "a beacon encoder hardcodes its tx-power byte again. Every beacon then shares one "
                "byte, which filters the whole population in a single rule.")
        self.assertGreaterEqual(body.count("rnd_tx_ref()"), 3,
                                "all three beacon encoders must draw tx power per instance")

    def test_eddystone_url_space_is_not_tiny(self):
        t = src("templates.c")
        m = re.search(r"static const char \*hosts\[\]\s*=\s*\{(.*?)\}", t, re.S)
        self.assertIsNotNone(m, "eddystone-url host list not found")
        hosts = [h for h in re.findall(r'"([^"]+)"', m.group(1))]
        self.assertGreaterEqual(
            len(hosts), 12,
            f"only {len(hosts)} eddystone-url hosts. With a fixed scheme and TLD this family "
            "emitted 4 distinct payloads in total, repeated forever - a persistent fingerprint "
            "underneath the churn.")


class TemplateLibraryCoverage(unittest.TestCase):
    """The library bounds which vendors can ever be expressed, whatever the model learns."""

    def test_library_covers_the_common_measured_vendors(self):
        t = src("templates.c")
        # Company ids measured as present in the operator's decoy-free captures, in rank order.
        # 0x0006 and 0x0040 were entirely absent from the shipped library despite being the 3rd and
        # 4th commonest vendors on air.
        for cid in (0x004C, 0x0075, 0x0006, 0x0040):
            self.assertRegex(t, r"0x%04X" % cid,
                             f"no template for company 0x{cid:04X}, which the captures show is "
                             "common. A vendor with no template cannot be expressed at all.")

    def test_library_is_substantially_larger_than_the_original_twelve(self):
        t = src("templates.c")
        n = len(re.findall(r'^\s*\{\s*"[^"]+",\s*FMT_', t, re.M))
        self.assertGreaterEqual(n, 20,
                                f"library is {n} templates. 12 shapes was itself a fingerprint: "
                                "every decoy the project emitted was one of twelve.")


if __name__ == "__main__":
    unittest.main()
