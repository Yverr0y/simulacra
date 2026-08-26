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


if __name__ == "__main__":
    unittest.main()
