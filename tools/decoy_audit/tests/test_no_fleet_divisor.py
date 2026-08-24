import os, re, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
MAIN = os.path.join(ROOT, "main")

# fleet_pop.{c,h} legitimately define these; no OTHER file may call them. Boards are additive as of
# 2026-08-24 -- a reintroduced divisor would silently halve the crowd per added board again, which
# is the behaviour the additive-fleet-population change exists to remove.
CALLERS_FORBIDDEN = ("coexist.c", "simulacra_main.c", "settings.c", "generate.c", "probe.c")
PATTERN = re.compile(r"\bfleet_pop_share(_k)?\s*\(")


class NoFleetDivisor(unittest.TestCase):
    def test_population_path_has_no_k_division(self):
        for name in CALLERS_FORBIDDEN:
            path = os.path.join(MAIN, name)
            if not os.path.exists(path):
                continue
            with open(path, encoding="utf-8", errors="replace") as f:
                for lineno, line in enumerate(f, 1):
                    if line.lstrip().startswith("//"):
                        continue
                    self.assertIsNone(
                        PATTERN.search(line),
                        f"{name}:{lineno} calls fleet_pop_share - population is additive, see "
                        f"docs/superpowers/specs/2026-08-24-additive-fleet-population-design.md")

    def test_fleet_pop_module_still_exists(self):
        # Retained deliberately (telemetry / future use), just uncalled. Not deleted.
        self.assertTrue(os.path.exists(os.path.join(MAIN, "fleet_pop.c")))
        self.assertTrue(os.path.exists(os.path.join(MAIN, "fleet_pop.h")))

    def test_retention_is_documented(self):
        # The header must say why it is uncalled, or a future session will "fix" the absence of
        # callers by adding one back. This is the DRIFT-1/DRIFT-2 guard.
        with open(os.path.join(MAIN, "fleet_pop.h"), encoding="utf-8", errors="replace") as f:
            head = f.read()
        self.assertIn("RETAINED BUT UNCALLED", head,
                      "fleet_pop.h must document that it is deliberately uncalled")


if __name__ == "__main__":
    unittest.main()
