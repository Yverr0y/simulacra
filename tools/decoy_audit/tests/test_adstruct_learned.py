import os, re, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
MAIN = os.path.join(ROOT, "main")


def src(name):
    with open(os.path.join(MAIN, name), encoding="utf-8", errors="replace") as f:
        return f.read()


class AdStructureIsLearnedNotHardcoded(unittest.TestCase):
    """The last generation axis the model could not express.

    interval_distribution (0.001-0.007) and vendor_histogram (0.000-0.087) stay closed on every
    capture tested, because both sample rf_model. AD structure did not: pick_no_mfg_template
    hardcoded ~62% flags-only / ~24% flags+uuid16, fitted in 2026-07-13 to one capture where
    flags-only advertisers were 52.7% of devices. The same share measured 6.7% and 0.0% in two
    other stationary captures. ad_structure scored 0.153 on the capture it was tuned to and
    0.27-0.93 on unseen ones -- the worst number on the scorecard.

    Learned 2026-08-26. Cross-validated ad_structure went 0.153/0.269/0.689/0.925 ->
    0.083/0.225/0.691/0.791, and the capture it was ORIGINALLY tuned to improved too, which is what
    replacing a fitted constant with something that tracks should do.
    """

    def test_model_carries_an_adstruct_histogram(self):
        h = src("rf_model.h")
        self.assertIn("adstruct_bins", h,
                      "rf_model_t lost its AD-structure histogram; structure cannot be learned "
                      "and pick_no_mfg_template is back to a constant fitted to one capture.")
        self.assertIn("RF_ADSTRUCT_BINS", h)

    def test_model_version_was_bumped_for_the_new_field(self):
        """A v1 blob has no adstruct_bins; loading one as v2 would read garbage weights."""
        m = re.search(r"#define\s+RF_MODEL_VERSION\s+(\d+)", src("rf_model.h"))
        self.assertIsNotNone(m)
        self.assertGreaterEqual(int(m.group(1)), 2,
                                "RF_MODEL_VERSION must advance when rf_model_t gains a field, or a "
                                "stored v1 blob is misread as containing structure counts.")

    def test_generator_samples_the_model(self):
        g = src("generate.c")
        self.assertIn("rf_adstruct_sample", g,
                      "pick_no_mfg_template no longer samples the learned mix")
        self.assertRegex(g, r"pick_no_mfg_template\(const rf_model_t \*m\)",
                         "pick_no_mfg_template must receive the model to sample it")

    def test_hardcoded_mix_survives_only_as_cold_start(self):
        """The old literals are still correct as a first guess -- but only until data arrives."""
        g = src("generate.c")
        body = g.split("static const device_template_t *pick_no_mfg_template", 1)[-1]
        body = body.split("\n}", 1)[0]
        self.assertIn("r < 62", body, "cold-start default is gone; a fresh boot must emit something")
        # ...and it must sit on the branch taken when the model has nothing, not unconditionally.
        idx_sample = body.index("rf_adstruct_sample")
        idx_hard = body.index("r < 62")
        self.assertLess(idx_sample, idx_hard,
                        "the hardcoded mix runs BEFORE the learned draw, so learning never applies")

    def test_observation_is_no_mfg_only(self):
        """An advert with mfg data takes its shape from that vendor's template, not this mix."""
        o = src("observe.c")
        self.assertRegex(o, r"if\s*\(!has_mfg\)\s*\n\s*rf_model_observe_adstruct",
                         "adstruct must be folded only for no-mfg adverts; including vendor "
                         "adverts blurs the mix pick_no_mfg_template needs.")

    def test_unrepresentable_shapes_are_not_substituted(self):
        r = src("rf_model.c")
        body = r.split("bool rf_adstruct_sample", 1)[-1].split("\n}", 1)[0]
        self.assertIn("RF_ADS_OTHER", body,
                      "the draw must exclude RF_ADS_OTHER (name-only/empty), which has no template")
        self.assertRegex(body, r"for\s*\(size_t b = 0; b < RF_ADS_OTHER; b\+\+\)",
                         "OTHER's weight must be redistributed across emittable bins, not spent on "
                         "a substitute shape the environment may not contain")

    def test_structure_decays_with_everything_else(self):
        r = src("rf_model.c")
        decay = r.split("void rf_model_decay", 1)[-1].split("\n}", 1)[0]
        self.assertIn("adstruct_bins", decay,
                      "structure must age on the rolling window; a mix that never decays "
                      "reproduces the fixed-constant defect more slowly.")



class MfgStructureIsLearnedToo(unittest.TestCase):
    """The same problem one layer down, found once the no-mfg mix was learned.

    enc_vendor_mfg emitted exactly one shape, flags+mfg ("01,ff"). Its real share across four
    decoy-free captures: 100.0% / 50.0% / 15.6% / 0.0% -- the same collapse that made the hardcoded
    no-mfg mix a single-capture overfit. Learned rather than replaced with a fixed varied set,
    because a fixed set is precisely the mistake being corrected.

    Cross-validated ad_structure: 0.153/0.269/0.689/0.925 (hardcoded) -> 0.083/0.225/0.691/0.791
    (no-mfg learned) -> 0.088/0.093/0.381/0.342 (both learned).
    """

    def test_model_carries_a_mfgstruct_histogram(self):
        h = src("rf_model.h")
        self.assertIn("mfgstruct_bins", h, "rf_model_t lost the mfg-structure histogram")
        self.assertIn("RF_MFGS_MFG_ONLY", h,
                      "the bare-'ff' bucket is gone; a device with NO flags element is a real and "
                      "common shape (43.1% of one capture's vendor devices).")

    def test_generator_samples_it(self):
        g = src("generate.c")
        self.assertIn("rf_mfgstruct_sample", g, "the mfg variant is no longer drawn from the model")
        self.assertIn("template_apply_mfg_variant", g, "the drawn variant is never applied")

    def test_variant_applies_only_to_mfg_bearing_payloads(self):
        """Beacons and terse advertisers have their own structure and must not be reshaped."""
        g = src("generate.c")
        self.assertRegex(
            g, r"company\s*!=\s*RF_VENDOR_UNKNOWN[\s\S]{0,200}?rf_mfgstruct_sample",
            "the mfg variant must be gated on the payload actually carrying mfg data; applying it "
            "to the no-mfg mass would reshape a population it does not describe.")

    def test_variant_is_appended_not_set_via_fields(self):
        """NimBLE's fixed field order would emit 01,19,ff where real devices emit 01,ff,19."""
        t = src("templates.c")
        body = t.split("int template_apply_mfg_variant", 1)[-1]
        self.assertIn("0x19", body, "appearance variant missing")
        self.assertIn("0x0A", body, "tx-power variant missing")
        self.assertRegex(body, r"payload\[n\+\+\]",
                         "variants must be APPENDED for exact element order; setting adv_fields "
                         "members puts appearance before mfg and the host serializer supports "
                         "neither field at all.")

    def test_mfg_only_strips_the_flags_element(self):
        t = src("templates.c")
        body = t.split("int template_apply_mfg_variant", 1)[-1]
        self.assertRegex(body, r"payload\[0\]\s*==\s*0x02\s*&&\s*payload\[1\]\s*==\s*0x01",
                         "RF_MFGS_MFG_ONLY must remove the leading flags element, or bare 'ff' "
                         "cannot be emitted at all.")

if __name__ == "__main__":
    unittest.main()
