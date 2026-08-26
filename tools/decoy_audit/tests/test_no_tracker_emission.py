import os, re, subprocess, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.abspath(os.path.join(HERE, ".."))
ROOT = os.path.abspath(os.path.join(TOOL, "..", ".."))
MAIN = os.path.join(ROOT, "main")
RADAR = os.path.join(ROOT, "components", "simulacra_radar")
EXE = os.path.join(TOOL, "synth_dump.exe")


def src(path, name):
    with open(os.path.join(path, name), encoding="utf-8", errors="replace") as f:
        return f.read()


def seeded_threat_keys():
    """(company_ids, svc_uuids) the project seeds into its OWN detector as threats."""
    s = src(RADAR, "sig_seed.c")
    comps = {int(m, 16) for m in re.findall(r"\.company_id\s*=\s*0x([0-9A-Fa-f]{4})", s)}
    uuids = {int(m, 16) for m in re.findall(r"\.svc_uuid16\s*=\s*0x([0-9A-Fa-f]{4})", s)}
    comps.discard(0xFFFF); uuids.discard(0x0000)
    return comps, uuids


class NeverEmitAThreatSignature(unittest.TestCase):
    """A decoy must never emit something this project's own detector calls a tracker.

    If it does, nearby phones running tracker detection warn their owners that an unknown tracker is
    travelling with them. An anti-tracking tool that triggers stalking alerts on strangers' phones is
    inverted at the human level, not just the technical one. Detecting a tracker is the point;
    impersonating one never is.
    """

    def test_no_template_emits_a_seeded_threat_service_uuid(self):
        _comps, uuids = seeded_threat_keys()
        t = src(MAIN, "templates.c")
        rows = re.findall(r'^\s*\{\s*"([^"]+)",\s*FMT_\w+,\s*(0x[0-9A-Fa-f]+|0),\s*(0x[0-9A-Fa-f]+|0)',
                          t, re.M)
        self.assertTrue(rows, "template table not parsed")
        for name, _company, svc in rows:
            self.assertNotIn(
                int(svc, 16) if svc.startswith("0x") else 0, uuids,
                f"template '{name}' advertises service UUID {svc}, which sig_seed.c matches as a "
                "tracker. Every decoy built from it is a guaranteed tracker hit on bystanders' "
                "phones.")

    def test_apple_model_byte_cannot_roll_a_forbidden_subtype(self):
        """Under 4C 00 the next byte selects the Continuity/Find My subtype.

        0x07/0x0F raise pairing pop-ups (Law 3); 0x12 is Find My, which is ALSO the seeded AirTag
        signature. A uniform random draw hits one 3 times in 768.
        """
        t = src(MAIN, "templates.c")
        body = t.split("static void enc_vendor_mfg", 1)[-1].split("\n}", 1)[0]
        self.assertRegex(
            body, r"company_id\s*==\s*0x004C",
            "enc_vendor_mfg no longer special-cases Apple, so its random model byte can roll "
            "0x07/0x0F/0x12 straight after the 4C 00 company id.")
        for b in ("0x07", "0x0F", "0x12"):
            self.assertIn(b, body, f"the Apple re-roll no longer excludes {b}")

    def test_generate_gates_template_output_on_law3(self):
        g = src(MAIN, "generate.c")
        self.assertIn("law3_forbidden", g,
                      "generate.c no longer gates template output. learn.c re-rolls forbidden "
                      "bytes; the template path needs the same backstop.")

    def test_learn_refuses_to_adopt_threat_shaped_advertisers(self):
        """learn_strip KEEPS service-UUID bytes, so a learned tracker shape still matches."""
        l = src(MAIN, "learn.c")
        self.assertIn("shape_matches_threat", l,
                      "learn.c no longer refuses threat-shaped shapes; a real AirTag/Tile in the "
                      "room would be adopted as a template and cloned.")
        strip = l.split("bool learn_strip", 1)[-1].split("\n}", 1)[0]
        self.assertIn("shape_matches_threat", strip,
                      "the threat check must run inside learn_strip, before the shape is stored")


@unittest.skipUnless(os.path.exists(EXE), "synth_dump not built")
class GeneratedCrowdIsClean(unittest.TestCase):
    def test_no_generated_decoy_carries_a_tracker_uuid_or_apple_popup(self):
        """End-to-end over a large crowd: the byte patterns must not appear on air."""
        rows = subprocess.run([EXE, "7", "256"], capture_output=True, text=True,
                              check=True).stdout.splitlines()
        self.assertGreater(len(rows), 200, "expected a large synthetic crowd")
        import json
        bad_uuid, bad_apple = [], []
        for ln in rows:
            if not ln.strip():
                continue
            d = json.loads(ln)
            ad = d.get("ad_hex") or d.get("payload") or ""
            hx = ad.lower().replace(" ", "")
            # Tile 0xFEED and SmartTag 0xFD5A appear little-endian in service data.
            if "edfe" in hx or "5afd" in hx:
                bad_uuid.append(hx)
            for sub in ("4c0007", "4c000f", "4c0012"):
                if sub in hx:
                    bad_apple.append(hx)
        self.assertFalse(bad_uuid, f"{len(bad_uuid)} decoys carry a seeded tracker service UUID")
        self.assertFalse(bad_apple, f"{len(bad_apple)} decoys carry an Apple pop-up/Find My subtype")


if __name__ == "__main__":
    unittest.main()
