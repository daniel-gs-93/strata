"""The differential corpus against the built strata_cli.

Skipped unless the environment variable STRATA_CLI_PATH names the executable.
"""

from __future__ import annotations

import contextlib
import io
import os
import pathlib
import re
import tempfile
import unittest

import differential

CLI_PATH = os.environ.get("STRATA_CLI_PATH", "")
CORPUS_PATH = pathlib.Path(__file__).with_name("corpus.sql")


def marker_lines(text: str) -> list[int]:
    """Indexes of the `-- differs:` lines of a corpus text."""
    return [
        index
        for index, line in enumerate(text.splitlines())
        if line.startswith("-- differs:")
    ]


SUMMARY_LINE = re.compile(
    r"differential: (\d+) statements, (\d+) expected differences, 0 unexpected"
)


def run_main(corpus_text: str) -> tuple[int, list[str]]:
    """Runs main() as the command line does and returns its status and lines."""
    with tempfile.TemporaryDirectory(prefix="strata-corpus-") as work:
        corpus_file = pathlib.Path(work) / "corpus.sql"
        corpus_file.write_text(corpus_text, encoding="utf-8")
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            status = differential.main(
                ["--cli", CLI_PATH, "--corpus", str(corpus_file)]
            )
    return status, output.getvalue().splitlines()


@unittest.skipUnless(CLI_PATH, "STRATA_CLI_PATH is not set")
class EndToEndTest(unittest.TestCase):
    def test_end_to_end_corpus_agrees_with_sqlite(self) -> None:
        text = CORPUS_PATH.read_text(encoding="utf-8")
        statements = differential.parse_corpus(text)
        status, lines = run_main(text)
        self.assertEqual(status, 0, msg="\n".join(lines))
        summary = SUMMARY_LINE.fullmatch(lines[-1])
        self.assertIsNotNone(summary, msg=lines[-1])
        if summary is not None:
            self.assertEqual(int(summary.group(1)), len(statements))
            self.assertEqual(int(summary.group(2)), len(marker_lines(text)))
        areas = {statement.differs for statement in statements if statement.differs}
        self.assertEqual(areas, set(differential.AREAS))

    def test_end_to_end_corpus_covers_indexed_reads(self) -> None:
        text = CORPUS_PATH.read_text(encoding="utf-8")
        statements = differential.parse_corpus(text)
        key_reads = [
            statement
            for statement in statements
            if re.search(r"\bFROM keyed WHERE id\b", statement.text)
        ]
        self.assertGreaterEqual(len(key_reads), 12)
        self.assertEqual(
            [statement for statement in key_reads if statement.differs], []
        )
        self.assertEqual(len(marker_lines(text)), 23)
        status, lines = run_main(text)
        self.assertEqual(status, 0, msg="\n".join(lines))
        summary = SUMMARY_LINE.fullmatch(lines[-1])
        self.assertIsNotNone(summary, msg=lines[-1])
        if summary is not None:
            self.assertEqual(int(summary.group(2)), 23)

    def test_end_to_end_markers_are_enforced(self) -> None:
        lines = CORPUS_PATH.read_text(encoding="utf-8").splitlines()
        first_marker = marker_lines("\n".join(lines))[0]

        # A deleted marker: the statement now differs without being marked.
        without = lines[:first_marker] + lines[first_marker + 1 :]
        status, printed = run_main("\n".join(without))
        self.assertEqual(status, 1)
        problems = [line for line in printed if line.startswith("unexpected: line ")]
        self.assertEqual(len(problems), 1)
        self.assertTrue(
            any("strata answered" in line for line in printed), msg="\n".join(printed)
        )

        # An added marker on a statement that agrees with SQLite.
        target = next(
            index
            for index, line in enumerate(lines)
            if line.startswith("SELECT * FROM dept;")
        )
        added = lines[:target] + ["-- differs: Limit -1"] + lines[target:]
        status, printed = run_main("\n".join(added))
        self.assertEqual(status, 1)
        problems = [line for line in printed if line.startswith("unexpected: line ")]
        self.assertEqual(len(problems), 1)
        self.assertTrue(
            any("marked as differing (limit -1)" in line for line in printed),
            msg="\n".join(printed),
        )


if __name__ == "__main__":
    unittest.main()
