"""Regenerate fixtures/<arch>_<band>.hex from the built dumper.

Run once after intentionally changing an archetype's IE bytes:
    pwsh ../run.ps1 -Rebuild
    python make_fixtures.py

The test suite then pins these bytes byte-for-byte, so regenerating is a deliberate act: it means
"the archetype changed on purpose", not "the test was in the way".

REBUILD FIRST, ALWAYS. On 2026-08-26 this suite passed against a probe_dump binary nine days older
than its sources -- the fixtures matched a program nobody was shipping any more, and ten real
failures only appeared once the binary was rebuilt. `run.ps1 -Rebuild` is not optional here.

The source MAC in every fixture is the dumper's fixed synthetic 02:11:22:33:44:55 (locally
administered), never a captured address -- these files are committed to a public repo.
"""
import os, subprocess
HERE = os.path.dirname(__file__)
TOOL = os.path.dirname(HERE)
EXE  = os.path.join(TOOL, "probe_dump.exe" if os.name == "nt" else "probe_dump")
FIX  = os.path.join(TOOL, "fixtures")
os.makedirs(FIX, exist_ok=True)

# (arch_idx, name, channel, band5) -- capture-derived archetypes, 2026-08-26.
# ARCH_R_BARE and ARCH_R_HTONLY are 2.4-ONLY (NULL 5 GHz tail), so they have no 5 GHz fixture:
# probe_build_request returns 2 for that band and probe.c skips, which models a device with no
# 5 GHz radio. Real crowds contain those; the previous modelled table did not.
CASES = [
    (0, "r-vs",     6, 0), (0, "r-vs",     36, 1),
    (1, "r-ec15",   6, 0), (1, "r-ec15",   36, 1),
    (2, "r-he",     6, 0), (2, "r-he",     36, 1),
    (3, "r-bare",   6, 0),
    (4, "r-htonly", 6, 0),
]

for idx, name, ch, b5 in CASES:
    hexline = subprocess.check_output([EXE, str(idx), str(ch), str(b5)], text=True).strip()
    tag = "5" if b5 else "24"
    path = os.path.join(FIX, f"{name}_{tag}.hex")
    with open(path, "w") as fh:
        fh.write(f"# {name} @ ch{ch} {'5' if b5 else '2.4'} GHz -- IE structure derived from a real\n"
                 "# capture (2026-08-26 census of 877 probing devices), emitted by probe_dump with a\n"
                 "# fixed synthetic SA of 02:11:22:33:44:55. Regenerate with make_fixtures.py.\n")
        fh.write(hexline + "\n")
    print("wrote", os.path.basename(path))
