#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Dreams-Recomp contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check that the normative Dreams docs stay host-independent and indexed.

python3 -I port/tools/doc_lint.py            # from $REPO/port, or from $REPO as port/tools/...
Exit 0 with no violations, 2 with violations (one `file:line:rule` line each).

Only the explicit NORMATIVE_DOCS allowlist is scanned. HANDOFF.md and WORKLOG.md are historical
logs that may name the machine, and raw log files are evidence; neither is scanned.

Rules:

  absolute-home-path   `/home/<user>/...` or `~/Documents/Programming/Dreams-Recomp`: use $REPO
  host-username        the reference machine's user name: use "the user"
  host-hardware        a CPU/GPU model of the reference machine (TEST_HARDWARE.md has the list)
  host-system          a distribution-version or desktop-compositor name of that machine
  index-missing        a normative doc that INDEX.md's table does not list

TEST_HARDWARE.md is exempt from host-hardware and host-system: it is the one place that describes
the machine. Any other line may opt out explicitly with the marker `doc-lint:allow`, or for a
block with `doc-lint:allow-begin` ... `doc-lint:allow-end` (for example a distro file path).
"""
import re
import sys
from pathlib import Path

DOCS = Path(__file__).resolve().parents[1] / "docs" / "dreams"

# Normative docs: kept current, host-independent, must pass this lint and be listed in INDEX.md.
NORMATIVE_DOCS = (
    "ACCURACY.md",
    "AUDIT.md",
    "AGENT_RUNS.md",
    "AI_DEVELOPMENT.md",
    "ARCHITECTURE.md",
    "BUILD.md",
    "CAPTURE.md",
    "CONVENTIONS.md",
    "FPS.md",
    "GPU_HANG.md",
    "IMPORTS_LOG.md",
    "INDEX.md",
    "INPUT.md",
    "PERF.md",
    "RUNNING.md",
    "SCENE_TESTS.md",
    "STATUS.md",
    "STUBS.md",
    "TEST_HARDWARE.md",
    "VCORE.md",
    "VERSIONS.md",
)

# The one place that may describe the reference machine.
HARDWARE_DOC = "TEST_HARDWARE.md"

ALLOW_LINE = "doc-lint:allow"
ALLOW_BEGIN = "doc-lint:allow-begin"
ALLOW_END = "doc-lint:allow-end"

# CPU/GPU models of the reference machine (TEST_HARDWARE.md). Keep model names, not vendors:
# "AMD"/"Mesa"/"RADV"/"NVIDIA" alone are generic enough for normative prose.
HOST_HARDWARE = re.compile(
    r"\b(?:RX\s?7800\s?XT|7800\s?XT|NAVI\s?32|Ryzen\s+7\s+5700X|5700X|Mesa\s+26\.2\.6)\b",
    re.IGNORECASE,
)

# Distribution version / desktop compositor that only one host has.
HOST_SYSTEM = re.compile(r"\b(?:Fedora\s+44|Hyprland)\b", re.IGNORECASE)

ABSOLUTE_HOME = re.compile(r"(?:/home/[A-Za-z0-9._-]+/|~/Documents/Programming/Dreams-Recomp)")
# The reference machine's user name, as it appears in old absolute paths and logs.
HOST_USERNAME_TEXT = "braden"
HOST_USERNAME = re.compile(r"\b" + HOST_USERNAME_TEXT + r"\b", re.IGNORECASE)


def violations_for(path, hardware_doc):
    """Yield (line_number, rule, line) for one document; never raises on odd content."""
    allowed = False
    text = path.read_text(encoding="utf-8", errors="replace")
    for number, line in enumerate(text.splitlines(), 1):
        if ALLOW_BEGIN in line:
            allowed = True
            continue
        if ALLOW_END in line:
            allowed = False
            continue
        if allowed or ALLOW_LINE in line:
            continue
        if ABSOLUTE_HOME.search(line):
            yield number, "absolute-home-path", line
        if HOST_USERNAME.search(line):
            yield number, "host-username", line
        if hardware_doc:
            continue
        if HOST_HARDWARE.search(line):
            yield number, "host-hardware", line
        if HOST_SYSTEM.search(line):
            yield number, "host-system", line


def index_lists(table_text, doc):
    """True when INDEX.md names DOC, as a markdown link or in a code span of its table."""
    return re.search(r"(?:\]\(|`)" + re.escape(doc) + r"(?:\)|`)", table_text) is not None


def check(docs=None):
    """Return a list of `file:line:rule` strings for the docs tree DOCS (or DOCS override)."""
    docs = DOCS if docs is None else docs
    problems = []
    index = docs / "INDEX.md"
    table_text = index.read_text(encoding="utf-8", errors="replace") if index.is_file() else ""
    for doc in NORMATIVE_DOCS:
        path = docs / doc
        if not path.is_file():
            problems.append(f"{doc}:0:missing-doc")
            continue
        if not index_lists(table_text, doc):
            problems.append(f"{doc}:1:index-missing")
        for number, rule, line in violations_for(path, doc == HARDWARE_DOC):
            problems.append(f"{doc}:{number}:{rule}: {line.strip()}")
    return problems


def main():
    problems = check()
    for problem in problems:
        print(problem)
    if problems:
        print(f"doc_lint: {len(problems)} violation(s)", file=sys.stderr)
        return 2
    print(f"doc_lint: {len(NORMATIVE_DOCS)} normative docs clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
