"""Both engines through a multi-leaf index, against each other and a model.

A seeded random script of batch inserts, point and range deletes, updates that
grow a row, duplicate inserts and selects runs on the flatfile and on the paged
engine through strata_cli. The outputs must be identical, the final aggregates
must equal a Python model of the table, and the paged index must pass its check
with the model's key count before and after the database is opened again.

Skipped unless the environment variable STRATA_CLI_PATH names the executable.
"""

from __future__ import annotations

import os
import random
import re
import string
import subprocess
import tempfile
import unittest

CLI_PATH = os.environ.get("STRATA_CLI_PATH", "")
SEEDS = (11, 12, 13)
ROW_COUNT = 3000
OPERATION_COUNT = 300
BATCH_ROWS = 200
AGGREGATE = "SELECT COUNT(*), SUM(id), SUM(grp), SUM(LENGTH(v)) FROM t;"
AGGREGATE_ROW = re.compile(
    r"COUNT\(\*\)\tSUM\(id\)\tSUM\(grp\)\tSUM\(LENGTH\(v\)\)\n([^\n]*)\n"
)
INDEX_HEADER = re.compile(r"index t\(id\): height (\d+), nodes (\d+), keys (\d+)")

Model = dict[int, tuple[int, str]]


def random_text(rng: random.Random, shortest: int, longest: int) -> str:
    length = rng.randint(shortest, longest)
    return "".join(rng.choice(string.ascii_lowercase) for _ in range(length))


def insert_statements(
    rng: random.Random, model: Model, row_ids: list[int]
) -> list[str]:
    """INSERT statements for the given ids; the model gets the same rows."""
    statements = []
    for start in range(0, len(row_ids), BATCH_ROWS):
        tuples = []
        for row_id in row_ids[start : start + BATCH_ROWS]:
            group = rng.randint(0, 9)
            text = random_text(rng, 1, 60)
            model[row_id] = (group, text)
            tuples.append(f"({row_id}, {group}, '{text}')")
        statements.append("INSERT INTO t VALUES " + ", ".join(tuples) + ";")
    return statements


def operation_statements(rng: random.Random, model: Model, key_space: int) -> list[str]:
    """One random operation as statements; the model follows it."""
    key = rng.randrange(key_space)
    choice = rng.random()
    if choice < 0.2:
        high = key + rng.choice([1, 5, 50, 150])
        for row_id in [found for found in model if key <= found < high]:
            del model[row_id]
        return [f"DELETE FROM t WHERE id >= {key} AND id < {high};"]
    if choice < 0.3:
        model.pop(key, None)
        return [f"DELETE FROM t WHERE id = {key};"]
    if choice < 0.5:
        fresh = list(dict.fromkeys(rng.randrange(key_space) for _ in range(60)))
        return insert_statements(
            rng, model, [row_id for row_id in fresh if row_id not in model]
        )
    if choice < 0.55 and model:
        return [f"INSERT INTO t VALUES ({next(iter(model))}, 1, 'duplicate');"]
    if choice < 0.7:
        text = random_text(rng, 100, 600)
        high = key + rng.choice([1, 3, 20])
        for row_id in [found for found in model if key <= found < high]:
            model[row_id] = (model[row_id][0], text)
        return [f"UPDATE t SET v = '{text}' WHERE id >= {key} AND id < {high};"]
    if choice < 0.8:
        high = key + rng.choice([10, 100])
        for row_id in [found for found in model if key <= found < high]:
            group, text = model[row_id]
            model[row_id] = (group + 1, text)
        return [f"UPDATE t SET grp = grp + 1 WHERE id >= {key} AND id < {high};"]
    if choice < 0.9:
        return [f"SELECT * FROM t WHERE id = {key};"]
    statement = (
        f"SELECT id, grp, LENGTH(v) FROM t WHERE id >= {key} "
        f"AND id < {key + 20} ORDER BY id;"
    )
    return [statement]


def build_script(seed: int) -> tuple[list[str], Model]:
    rng = random.Random(seed)
    model: Model = {}
    key_space = ROW_COUNT * 2
    statements = ["CREATE TABLE t (id INT PRIMARY KEY, grp INT, v TEXT);"]
    statements += insert_statements(rng, model, rng.sample(range(key_space), ROW_COUNT))
    for _ in range(OPERATION_COUNT):
        statements += operation_statements(rng, model, key_space)
    return statements, model


def run_engine(engine: str, statements: list[str]) -> str:
    """Output of strata_cli for the statements on a new directory."""
    with tempfile.TemporaryDirectory(prefix="strata-scale-") as directory:
        lines = [f".engine {engine}", f".open {directory}"]
        lines += statements + [AGGREGATE]
        if engine == "paged":
            lines += [".dump index t", f".open {directory}", AGGREGATE]
            lines += [".dump index t"]
        done = subprocess.run(
            [CLI_PATH],
            input="\n".join(lines) + "\n",
            capture_output=True,
            text=True,
            timeout=600,
            check=False,
        )
    return done.stdout + done.stderr


def result_lines(output: str, is_paged: bool) -> list[str]:
    """The output without the open lines, and without the index report."""
    skipped = ("engine ", "loaded", "created new database")
    if is_paged:
        skipped += ("index t(id)", "level ", "check:")
    lines = [line for line in output.split("\n") if not line.startswith(skipped)]
    while lines and lines[-1] == "":
        lines.pop()
    return lines


def aggregates_of(output: str) -> list[tuple[int, ...]]:
    return [
        tuple(int(field) for field in match.group(1).split("\t"))
        for match in AGGREGATE_ROW.finditer(output)
    ]


def model_aggregates(model: Model) -> tuple[int, ...]:
    return (
        len(model),
        sum(model),
        sum(group for group, _ in model.values()),
        sum(len(text) for _, text in model.values()),
    )


@unittest.skipUnless(CLI_PATH, "STRATA_CLI_PATH is not set")
class IndexScaleTest(unittest.TestCase):
    def test_index_scale_agrees_between_engines_and_with_a_model(self) -> None:
        for seed in SEEDS:
            with self.subTest(seed=seed):
                statements, model = build_script(seed)
                flat_output = run_engine("flatfile", statements)
                paged_output = run_engine("paged", statements)
                expected = model_aggregates(model)

                flat_lines = result_lines(flat_output, False)
                paged_lines = result_lines(paged_output, True)
                self.assertEqual(flat_lines, paged_lines[: len(flat_lines)])
                self.assertEqual(aggregates_of(flat_output)[-1], expected)
                self.assertEqual(aggregates_of(paged_output), [expected, expected])
                self.assertEqual(paged_output.count("check: ok"), 2)
                headers = INDEX_HEADER.findall(paged_output)
                self.assertEqual(len(headers), 2)
                for height, nodes, keys in headers:
                    self.assertEqual(int(keys), expected[0])
                    self.assertGreaterEqual(int(height), 2)
                    self.assertGreater(int(nodes), 1)


if __name__ == "__main__":
    unittest.main()
