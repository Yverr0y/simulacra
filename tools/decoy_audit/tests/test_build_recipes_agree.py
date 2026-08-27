import os, re, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.abspath(os.path.join(HERE, ".."))
TOOLS = os.path.abspath(os.path.join(TOOL, ".."))
ROOT = os.path.abspath(os.path.join(TOOLS, ".."))


def read(*parts):
    with open(os.path.join(*parts), encoding="utf-8", errors="replace") as f:
        return f.read()


def c_basenames(text):
    return set(re.findall(r"([A-Za-z0-9_]+)\.c\b", text))


class BuildRecipesAgree(unittest.TestCase):
    """The synth_dump harness has TWO build recipes and they must list the same sources.

    tools/decoy_audit/Makefile is what CI runs; run.ps1's cl invocation is what the bench runs.
    Adding a source to only one produces a link failure the other build never sees -- which is
    precisely what happened on 2026-08-26: rf_model.c and the sig_* sources went into run.ps1 for
    the learned AD-structure work, the Makefile was missed, everything passed locally, and CI failed
    on an undefined rf_adstruct_sample at push time.

    This project has hit the same trap before (a run.ps1 edit that silently did not match, found
    only by grepping every build file afterwards). Asserting it is cheaper than remembering it.
    """

    def test_makefile_and_powershell_list_the_same_sources(self):
        mk = read(TOOL, "Makefile")
        ps = read(TOOL, "run.ps1")
        mk_src = c_basenames(mk.split("SRC :=", 1)[1].split("INC :=", 1)[0])
        ps_src = c_basenames(ps.split("cl /nologo", 1)[1].split("/Fe:", 1)[0])
        self.assertEqual(
            mk_src, ps_src,
            "build recipes disagree.\n"
            "  only in Makefile: %s\n"
            "  only in run.ps1 : %s\n"
            "Add the source to BOTH, or CI and the bench build different programs."
            % (sorted(mk_src - ps_src) or "-", sorted(ps_src - mk_src) or "-"))

    def test_every_listed_source_exists(self):
        """A typo'd path fails the same way a missing source does, just later and less clearly."""
        mk = read(TOOL, "Makefile")
        for rel in re.findall(r"\$\(ROOT\)/([A-Za-z0-9_/]+\.c)", mk):
            self.assertTrue(os.path.exists(os.path.join(ROOT, rel)),
                            "Makefile lists %s, which does not exist" % rel)

    def test_host_nvs_guard_is_passed_where_rf_model_is_linked(self):
        """rf_model.c is linked whole; its NVS pair must be compiled out on the host.

        Without the guard those two functions collide with roster_stub.c's, and the harness fails to
        link with duplicate symbols rather than anything that names the cause.
        """
        mk = read(TOOL, "Makefile")
        ps = read(TOOL, "run.ps1")
        if "rf_model.c" in mk:
            self.assertIn("SIMULACRA_HOST_NO_NVS", mk,
                          "Makefile links rf_model.c without -DSIMULACRA_HOST_NO_NVS")
        if "rf_model.c" in ps:
            self.assertIn("SIMULACRA_HOST_NO_NVS", ps,
                          "run.ps1 links rf_model.c without /DSIMULACRA_HOST_NO_NVS")


class LearnDependenciesAreCarriedEverywhere(unittest.TestCase):
    """learn.c depends on sig_store, so every harness linking it needs those sources.

    learn_strip() refuses to adopt a shape matching a seeded threat signature, so a real tracker in
    the room cannot be learned and cloned. That dependency reaches tools/pcap_learn as well, which
    links learn.c for its harness -- CI stopped at the first failure and never reached it.
    """

    def test_pcap_learn_harness_carries_sig_sources(self):
        mk = read(TOOLS, "pcap_learn", "Makefile")
        harness_src = mk.split("SRC :=", 1)[1].split("SCAN_SRC :=", 1)[0]
        if "learn.c" not in harness_src:
            self.skipTest("pcap_learn no longer links learn.c")
        for need in ("sig_store.c", "sig_match.c", "sig_seed.c"):
            self.assertIn(need, harness_src,
                          "pcap_learn harness links learn.c but not %s; learn_strip's threat gate "
                          "will not resolve." % need)


if __name__ == "__main__":
    unittest.main()
