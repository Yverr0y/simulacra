import os, re, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
MAIN = os.path.abspath(os.path.join(HERE, "..", "..", "..", "main"))


def src(name):
    with open(os.path.join(MAIN, name), encoding="utf-8", errors="replace") as f:
        return f.read()


class LearnedShapesAreNotClonedFreely(unittest.TestCase):
    """A learned skeleton is copied from a REAL nearby device. Copies must be bounded.

    learn_strip keeps element order, lengths, and several fields verbatim (flags, service-uuid
    lists, tx power, appearance); rand_mask re-randomises only the instance bytes. So identity does
    not leak, but SHAPE does not vary either. With a uniform per-company pick, a fleet that had
    learned exactly one shape for a vendor rendered EVERY decoy of that vendor from that single
    skeleton -- an observer saw the original real device plus N shape-identical clones.

    Fine for a common handset, implausible for a rare device. And the promotion gate
    (LEARN_MIN_SIGHTINGS, counted within one sweep) filters slow advertisers rather than rare ones,
    so a rare-but-chatty device is precisely what gets learned.
    """

    def test_clone_budget_exists_and_is_bounded(self):
        g = src("generate.c")
        self.assertIn("learn_clone_budget", g,
                      "the per-shape clone budget is gone; one real device can be cloned across "
                      "the whole vendor share again.")
        m = re.search(r"#define\s+LEARN_CLONE_MAX\s+(\d+)", g)
        self.assertIsNotNone(m, "LEARN_CLONE_MAX missing")
        self.assertLessEqual(int(m.group(1)), 10,
                             "clone ceiling is too high to be plausible for an uncommon device")

    def test_budget_scales_with_how_often_the_shape_was_SEEN(self):
        """reinforce_count is what separates a common model from a rare one."""
        g = src("generate.c")
        body = g.split("static uint8_t learn_clone_budget", 1)[-1].split("\n}", 1)[0]
        self.assertIn("reinforce_count", body,
                      "the budget no longer depends on reinforce_count, so a shape seen once gets "
                      "the same crowd as a shape seen constantly.")

    def test_a_shape_seen_once_is_not_cloned(self):
        """Mirror the C arithmetic: budget(rc=0 or 1) must be exactly 1."""
        g = src("generate.c")
        body = g.split("static uint8_t learn_clone_budget", 1)[-1].split("\n}", 1)[0]
        m = re.search(r"1u?\s*\+\s*\(uint32_t\)lt->reinforce_count\s*/\s*(\d+)u?", body)
        self.assertIsNotNone(m, "clone budget formula not recognised")
        div = int(m.group(1))
        for rc in (0, 1):
            self.assertEqual(1 + rc // div, 1,
                             f"a shape seen {rc} time(s) would get {1 + rc // div} copies")

    def test_budget_is_consumed_and_reset_per_build(self):
        g = src("generate.c")
        self.assertIn("s_clone_used", g, "per-shape usage tracking is gone")
        self.assertRegex(g, r"generate_roster\([^)]*\)\s*\{\s*\n\s*generate_reset_clone_budget\(\)",
                         "the clone budget must reset at the start of each roster build, or it "
                         "leaks across builds and eventually blocks every learned shape.")
        self.assertRegex(g, r"s_clone_used\[pick\]\+\+|s_clone_used\[pick\]\s*\+=",
                         "the budget is never consumed, so the cap does nothing")


if __name__ == "__main__":
    unittest.main()
