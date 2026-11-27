# Strata

Strata is a small database that can be run from a terminal. It reads Structured Query Language (SQL) statements through a Command-Line Interface (CLI), parses them and turns each one into a plan, and hands the plan to a storage engine. The parser and the binder, which resolves names and builds the plan, are shared by every engine, so an engine only has to execute plans and decide how data is stored. The first engine, flatfile, is deliberately plain: it is the baseline that the second engine, paged, is compared with. The paged engine keeps a database in one file of fixed-size pages and reads them through a buffer pool, a cache that keeps some of the pages in memory.

In the flatfile engine a directory is a database. `catalog.csv` records the table definitions, and each table is a Comma-Separated Values (CSV) file. Writes go back to the CSV immediately, and a write that spans several files either completes or changes nothing, except when a rename fails and so does the undo of the earlier ones; the error then names the `.orig` file that holds the original content.

## Documentation

| Document | Contents |
|---|---|
| [`docs/DesignDocument.md`](docs/DesignDocument.md) | Architecture, SQL rules, flows and interfaces, for the front end and both engines |
| [`docs/DecisionRecord.md`](docs/DecisionRecord.md) | The rationale, cost and rejected alternatives of each design decision, written as Architecture Decision Records (`ADR-NNNN`) |
| [`docs/TestDocument.md`](docs/TestDocument.md) | Test matrix of the whole system |

## Directory Structure

```text
strata/
├── include/strata/
│   ├── domain/            # values, syntax tree, plan, Binder, expression evaluation, scalar functions, import inference, engine port, index inspector port
│   ├── paged/             # the paged engine: page format, buffer pool, page allocator, table heap, B+Tree index, catalog, operators, writes
│   ├── application.h      # QueryService: run one statement, import a CSV
│   ├── bootstrap.h        # acquire the directory lock, assemble the engine and QueryService, shared by every entry point
│   ├── sql_parser.h       # SQL parser
│   ├── repl.h, statement_buffer.h
│   │                      # CLI: input loop, commands, statement splitting
│   └── infrastructure.h, flat_file_engine.h, flat_file_executor.h,
│       csv_codec.h, csv_file_source.h, directory_lock.h
│                          # flatfile engine: CSV repository, executor, codec, external CSV reading, directory lock
├── src/                   # the matching implementations; cli.cpp contains only main
├── tests/                 # test harness, the tests of each module and the generator of the volume-test data
│   └── differential/      # Python harness that compares Strata with SQLite
├── docs/                  # design document, decision record and test matrix
└── sample_my_db/          # sample database: course enrollment system (4 tables plus catalog.csv)
```

## Build

The build needs CMake 3.22 or later and a C++20 compiler.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

This produces `build/strata_cli` and `build/strata_tests`.

## Quick Start

The CLI reads commands and SQL statements from standard input, line by line, until the input ends. `strata_cli [<directory> [--parallel]]` opens a database at startup, like typing `.open <directory> [--parallel]` afterwards. Any other arguments, a first argument that starts with `-` included, print the usage to standard error and exit with status 2; write a directory whose name starts with `-` as `./-name`.

A statement ends with `;` and can span several lines. One line can hold several statements, a `;` inside a string literal does not end one, and `--` starts a comment that runs to the end of the line. `.open <directory>` has to come before the first SQL statement. If the directory does not exist but its parent does, `.open` creates it and prints `created new database <directory>`.

On a terminal the prompt is `strata> `, which changes to `   ...> ` inside an unfinished statement and to `     '> ` inside a string literal. Input from a pipe or a file gets no prompt.

| Command | Effect |
|---|---|
| `.open <directory> [--parallel]` | Opens the database directory, creating it if it does not exist |
| `.engine [flatfile\|paged]` | Shows the engine of the session, or selects it (see Engines) |
| `.import <file.csv> [<table>] [--pk <column>] [--type <column>=<type>]... [--skip-bad-rows]` | Imports a CSV into a new table |
| `.tables` | Lists one table per line |
| `.schema [<table>]` | Prints `CREATE TABLE` statements that can be run again |
| `.dump index <table>` | Prints the shape of the index of a `paged` table and the result of its check (see Engines) |
| `.clear` | Clears the screen; prints nothing unless standard input is a terminal |
| `.cancel` | Abandons an unfinished statement; inside a string literal the line is read as part of the string |
| `.help` | Prints the command list |
| `.quit`, `.exit` | Exits with status 0 |

A command takes the whole line and only counts as one when no statement is unfinished. A command splits its line at whitespace, so the directory of `.open` and the paths of `.import` cannot contain a space; `.open /tmp/My Docs/db` prints the usage. A line that starts with `.` but names no command, such as `.tbles`, prints `error: unknown command .tbles; .help lists the commands` and is discarded. Without the leading `.`, as in `open mydb`, the CLI prints `error: commands start with '.'; did you mean .open` and discards the line. A line that contains a full-width `；` (U+FF1B) outside a string or comment is discarded too, while the earlier lines of an unfinished statement are kept, with an error that asks for an American Standard Code for Information Interchange (ASCII) semicolon: `error: full-width ';' (U+FF1B) found; use ASCII ';'`.

### Sample Database `sample_my_db`

`sample_my_db/` is a small course enrollment database that ships with the repository. It has four tables: `instructors` (6 rows), `students` (30), `courses` (12) and `enrollments` (144). Each `enrollments` row references a student and a course, and each course references an instructor. `.schema` lists the columns. There is no NULL, so a `score` of 0 means no grade yet, and course capacity and duplicate enrollments are not checked. Every table has an `id`, so a join writes `courses.id`. Apart from `id`, only `name` (instructors, students) and `dept` (instructors, courses) repeat, so the other columns can be written bare.

A read-only query can open the database directly (opening only creates `.strata.lock`, which `.gitignore` excludes):

```bash
CLI="$PWD/build/strata_cli"
"$CLI" <<'EOF'
.open sample_my_db
.tables
.schema courses
SELECT code, title, credits, capacity FROM courses ORDER BY code LIMIT 4;
SELECT code, COUNT(*)
FROM courses JOIN enrollments ON course_id = courses.id
WHERE semester = '2026-Spring' AND status = 'enrolled'
GROUP BY code ORDER BY 2 DESC, 1 LIMIT 3;
SELECT name, code, score
FROM students
JOIN enrollments ON student_id = students.id
JOIN courses ON course_id = courses.id
WHERE semester = '2025-Fall' AND status = 'completed'
ORDER BY 3 DESC, 1 LIMIT 3;
SELECT id, name, LENGTH(name), UPPER(major) FROM students WHERE name LIKE 'K%';
EOF
```

```text
loaded instructors=6 students=30 courses=12 enrollments=144
instructors
students
courses
enrollments
CREATE TABLE courses (
  id INT PRIMARY KEY AUTOINCREMENT,
  code TEXT UNIQUE,
  title TEXT,
  credits INT,
  dept TEXT,
  capacity INT,
  instructor_id INT REFERENCES instructors(id)
);
code	title	credits	capacity
ART101	Art Appreciation	2	10
CS101	Intro to Programming	3	6
CS201	Data Structures	3	5
CS301	Database Systems	3	5
code	COUNT(*)
HIST101	8
STAT101	8
ECON101	7
name	code	score
Ivy Chen	CS301	97
Wendy Morgan	HIST101	97
Frank Miller	CS301	95
id	name	LENGTH(name)	UPPER(major)
1011	Kevin Anderson	14	CS
```

Writes rewrite the CSV, so this session uses a copy. The foreign key checks reject the `INSERT` and the `DELETE`, and the valid `UPDATE` is written to `enrollments.csv` at once:

```bash
CLI="$PWD/build/strata_cli"
rm -rf /tmp/sample_my_db_copy && cp -R sample_my_db /tmp/sample_my_db_copy
"$CLI" <<'EOF'
.open /tmp/sample_my_db_copy
SELECT id, student_id, course_id, status, score FROM enrollments WHERE id = 100;
INSERT INTO enrollments (student_id, course_id, semester) VALUES (9999, 1, '2026-Spring');
DELETE FROM students WHERE id = 1001;
UPDATE enrollments SET status = 'completed', score = 90 WHERE id = 100;
SELECT id, student_id, course_id, status, score FROM enrollments WHERE id = 100;
EOF
```

```text
loaded instructors=6 students=30 courses=12 enrollments=144
id	student_id	course_id	status	score
100	1008	3	enrolled	0
error: constraint violation: no matching row: enrollments.student_id=9999 not found in students.id
error: constraint violation: students.id=1001 is referenced by enrollments.student_id
OK affected=1
id	student_id	course_id	status	score
100	1008	3	completed	90
```

To use it interactively, run `"$CLI"` without a pipe, type `.open sample_my_db` at the `strata> ` prompt and enter statements line by line.

### Starting from an Empty Directory

`.open` creates a directory that does not exist, so no `mkdir` is needed:

```bash
CLI="$PWD/build/strata_cli"
rm -rf /tmp/mydb
"$CLI" <<'EOF'
.open /tmp/mydb
CREATE TABLE notes (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  body TEXT NOT NULL,
  owner TEXT DEFAULT 'none'
);
INSERT INTO notes (body) VALUES ('Hello, "world"');
ALTER TABLE notes ADD COLUMN pinned INT DEFAULT 0;
SELECT id, body, owner, pinned FROM notes;
EOF
```

Output (columns separated by Tab):

```text
created new database /tmp/mydb
OK CREATE TABLE notes
OK affected=1
OK ALTER TABLE notes
id	body	owner	pinned
1	Hello, "world"	none	0
```

`/tmp/mydb` now holds `catalog.csv` and `notes.csv`, and `.open /tmp/mydb` loads the table again. `--parallel` loads the tables with several threads and gives the same result.

`.open` fails with `error: could not open <directory>: <reason>`, where `<reason>` includes `no such directory` when the parent does not exist, `not a directory` when the path is a file, and `cannot create directory` when creation fails. The open database stays as it was. With the paged engine, a `paged.db` whose first creation was interrupted fails with a `corrupt:` reason that names `paged.db`, for example `size of 2 pages is below the minimum of 3`, or `size 100 is not a multiple of 4096` after a partial page. That file holds no data, so delete it and `.open` the directory again.

A statement that spans lines, a `--` comment and `.schema`:

```bash
CLI="$PWD/build/strata_cli"
"$CLI" <<'EOF'
.open /tmp/mydb
.tables
SELECT id, body  -- two columns
FROM notes
WHERE id = 1;
.schema
EOF
```

```text
loaded notes=1
notes
id	body
1	Hello, "world"
CREATE TABLE notes (
  id INT PRIMARY KEY AUTOINCREMENT,
  body TEXT,
  owner TEXT DEFAULT 'none',
  pinned INT DEFAULT 0
);
```

The `.schema` output doubles as a schema file: feed it to the CLI after `.open` on another empty directory to create the same table there.

### CSV Import

`.import` loads an external CSV into a new table. It infers the column types and the primary key from the data and leaves the source file alone. The example writes a small CSV to `/tmp` first. The comma in `"Eraser, large"` is inside double quotes, so it stays part of the value:

```bash
CLI="$PWD/build/strata_cli"
rm -rf /tmp/shop_db
cat > /tmp/products.csv <<'EOF'
id,title,price,stock
1,Pencil,0.5,10
2,Notebook,2.25,4
3,"Eraser, large",1.0,0
4,Ruler,1.75,7
EOF
"$CLI" <<'EOF'
.open /tmp/shop_db
.import /tmp/products.csv
.schema products
SELECT id, title, price * stock FROM products ORDER BY 3 DESC;
EOF
```

```text
created new database /tmp/shop_db
OK IMPORT products (imported 4 rows)
CREATE TABLE products (
  id INT PRIMARY KEY,
  title TEXT,
  price DOUBLE,
  stock INT
);
id	title	price * stock
4	Ruler	12.25
2	Notebook	9.0
1	Pencil	5.0
3	Eraser, large	0.0
```

The table name defaults to the file name without its extension, or follows the file name on the command line. It must match `[A-Za-z_][A-Za-z0-9_]*`, cannot be `catalog`, and cannot already exist.

A column is `INT` when every value is an integer in canonical form (`0` or `-?[1-9][0-9]*`), `DOUBLE` when every value is a decimal in canonical form (`0.5`, `1.0`), and `TEXT` otherwise, including when a cell is empty.

Canonical means that the engine's output format reproduces the text. `007`, `+5` and `1.50` are not canonical, and neither is a `10` in a column of decimals.

`--type <column>=<type>` sets a type explicitly (repeatable) and rewrites the values to it. `007` in an `INT` column is stored as `7`, and `1` in a `DOUBLE` column as `1.0`.

The primary key is the column given by `--pk <column>`, or else the `INT` column named `id`. The import fails when neither exists and when key values repeat. It generates no `UNIQUE`, foreign key, `NOT NULL`, `DEFAULT` or `AUTOINCREMENT` constraint.

A bad row has a different column count from the header, unpaired quotes, or a value that does not match its `--type`. By default the import fails and names the line of the first bad row. `--skip-bad-rows` skips the bad rows and lists them.

In the file below, `10x3` on line 4 is not an integer and line 5 lacks a column. The first `.import` stops at line 5. The second stops at line 4 once `order_no` is declared `INT`, and the third skips both:

```bash
CLI="$PWD/build/strata_cli"
rm -rf /tmp/shop_db2
cat > /tmp/orders.csv <<'EOF'
order_no,customer,amount
1001,Ann,12.5
1002,Bob,7.0
10x3,Cy,9.0
1004,Di
1005,Eve,3.25
EOF
"$CLI" <<'EOF'
.open /tmp/shop_db2
.import /tmp/orders.csv
.import /tmp/orders.csv --pk order_no --type order_no=INT
.import /tmp/orders.csv --pk order_no --type order_no=INT --skip-bad-rows
SELECT * FROM orders ORDER BY order_no;
EOF
```

```text
created new database /tmp/shop_db2
error: bind: line 5: expected 3 fields, found 2
error: bind: line 4: order_no '10x3' is not INT
OK IMPORT orders (imported 3 rows, skipped 2)
skipped line 4: order_no '10x3' is not INT
skipped line 5: expected 3 fields, found 2
order_no	customer	amount
1001	Ann	12.5
1002	Bob	7.0
1005	Eve	3.25
```

On the flatfile engine the import writes `<table>.csv` and `catalog.csv` in one staged commit, so a failure changes neither; on the paged engine a failure can leave an empty table (see Limitations). On the flatfile engine it does not replace an existing file: when `<table>.csv` is already in the database directory, it stops with `error: io: <table>.csv already exists; use CREATE TABLE to adopt it`. Table and column names cannot be reserved words such as `order` or `select`. At most 10 skipped rows are listed, then `... and <k> more skipped`.

### Attaching an Existing CSV

When a table's CSV already exists, `CREATE TABLE` adopts the file and its data. The header must match the declared columns (ignoring case), and the data must satisfy the declared primary key, `UNIQUE` and foreign keys.

The statements below attach the CSV files of `sample_my_db/` in a new directory that has no `catalog.csv`. They work on a copy, because writes rewrite the CSV:

```bash
(
  CLI="$PWD/build/strata_cli"
  rm -rf /tmp/strata_demo
  mkdir /tmp/strata_demo
  cp -R sample_my_db /tmp/strata_demo/school
  rm /tmp/strata_demo/school/catalog.csv
  cd /tmp/strata_demo
  "$CLI" <<'EOF'
.open school
CREATE TABLE instructors (id INT PRIMARY KEY AUTOINCREMENT, name TEXT, dept TEXT);
CREATE TABLE students (
  id INT PRIMARY KEY,
  name TEXT,
  major TEXT,
  year INT,
  email TEXT UNIQUE
);
CREATE TABLE courses (
  id INT PRIMARY KEY AUTOINCREMENT,
  code TEXT UNIQUE,
  title TEXT,
  credits INT,
  dept TEXT,
  capacity INT,
  instructor_id INT REFERENCES instructors(id)
);
CREATE TABLE enrollments (
  id INT PRIMARY KEY AUTOINCREMENT,
  student_id INT REFERENCES students(id),
  course_id INT REFERENCES courses(id),
  semester TEXT,
  status TEXT DEFAULT 'enrolled',
  score INT DEFAULT 0
);
SELECT name, major FROM students WHERE id = 1001;
EOF
)
```

```text
loaded no tables
OK CREATE TABLE instructors (adopted 6 rows)
OK CREATE TABLE students (adopted 30 rows)
OK CREATE TABLE courses (adopted 12 rows)
OK CREATE TABLE enrollments (adopted 144 rows)
name	major
Alice Johnson	ART
```

From then on `.open /tmp/strata_demo/school` is enough.

A line with the wrong column count, or a value that does not match its type, is rejected when the file is loaded. The loader leaves the line out, so `students=30` is followed by `(rejected 1)` and a line naming it. A file lists at most 10 line numbers, then `, ...`. In the example below, the appended line has 4 fields where `students` has 5:

```bash
CLI="$PWD/build/strata_cli"
echo '1031,Zed Young,ART,2' >> /tmp/strata_demo/school/students.csv
"$CLI" <<'EOF'
.open /tmp/strata_demo/school
SELECT id, UPPER(name), LENGTH(name)
FROM students WHERE name LIKE 's%' ORDER BY id LIMIT 2;
SELECT COUNT(*), MIN(capacity), MAX(capacity), AVG(capacity) FROM courses;
EOF
```

```text
loaded instructors=6 students=30 (rejected 1) courses=12 enrollments=144
students.csv: rejected lines 32
id	UPPER(name)	LENGTH(name)
1019	SAMUEL INGRAM	13
1028	SOPHIE RIVERA	13
COUNT(*)	MIN(capacity)	MAX(capacity)	AVG(capacity)
12	4	10	6.916666666666667
```

## Supported SQL

| Statement | Example |
|---|---|
| `CREATE TABLE`, with column constraints `PRIMARY KEY [AUTOINCREMENT]`, `UNIQUE`, `NOT NULL`, `DEFAULT <constant>`, `REFERENCES t(c)`; types `INT`, `DOUBLE`, `TEXT` and the aliases `INTEGER`, `BIGINT`, `REAL`, `FLOAT`, `VARCHAR(n)`, `CHAR(n)` | `CREATE TABLE students (id INT PRIMARY KEY, name TEXT UNIQUE, major TEXT);` |
| `ALTER TABLE ... ADD COLUMN` (a `DEFAULT` is required) | `ALTER TABLE students ADD COLUMN age INT DEFAULT 0;` |
| `DROP TABLE` (rejected while another table references it) | `DROP TABLE notes;` |
| `SELECT`, with `WHERE`, `JOIN ... ON` (up to 64 joins), `GROUP BY`, `ORDER BY`, `LIMIT ... OFFSET ...`; aggregates `COUNT`, `SUM`, `AVG`, `MIN`, `MAX` | `SELECT e.status, COUNT(*) FROM enrollments e JOIN students s ON e.student_id = s.id WHERE s.major = 'ART' GROUP BY e.status;` |
| `INSERT`; the column list can leave out a column that has a `DEFAULT` or is auto-increment; `VALUES` can have several rows, and the whole statement is all-or-nothing | `INSERT INTO enrollments (student_id, course_id, semester) VALUES (1002, 1, '2026-Spring');` |
| `UPDATE` | `UPDATE courses SET capacity = capacity + 1 WHERE id = 1;` |
| `DELETE` | `DELETE FROM enrollments WHERE id = 145;` |

Expressions:

| Kind | Supported |
|---|---|
| Operators | `+`, `-`, `*`, `/` (between two `INT` the result is truncated toward 0), `\|\|` (text concatenation), `= <> != < <= > >=`, `LIKE` (`%` and `_`, ASCII case-insensitive), `AND`, `OR` |
| Functions | `LOWER`, `UPPER`, `LENGTH`, `SUBSTR`, `TRIM`, `ABS`, `ROUND`, `CAST(<expression> AS INT\|DOUBLE\|TEXT)` |

Dividing by 0, a `CAST` that cannot convert, and `AVG`, `MIN` or `MAX` over no rows give `error: runtime: <message>`, since there is no NULL to return. Design Document 2.3 has the full rules for operators and functions.

When a table or column does not exist, the message suggests an existing name within edit distance 2, as in `error: bind: unknown column 'nam'; did you mean name?`.

A Data Definition Language (DDL) statement prints `OK <tag>` and a write statement prints `OK affected=<n>`. Errors print `error: <kind>: <message>`. A syntax error prints `syntax error at column <n>: <description>`, or `syntax error at line <l>, column <c>: <description>` when it falls on the second line or later of a multi-line statement. A row that another table's foreign key references cannot be deleted:

```text
DELETE FROM students WHERE id = 1001;
error: constraint violation: students.id=1001 is referenced by enrollments.student_id
```

Design Document 2.3 to 2.6, 3.6 and 3.7 cover syntax, semantics, constraints, DDL and the output and error format.

## Engines

Strata has two engines. `flatfile` is the default, and most of this README describes it. `paged` stores a database in one file of fixed-size pages and reads them through a buffer pool. Sections 2.10 to 2.13 of [`docs/DesignDocument.md`](docs/DesignDocument.md) describe how `paged` stores tables and runs statements, and Section 2.15 describes what it keeps and what it can lose when a write fails.

`.engine` shows the engine of the session, and `.engine paged` or `.engine flatfile` selects one. The choice lasts for one session and is not saved, so every start begins with `flatfile`. `.open` uses the selected engine. When a database is already open, naming the other engine reopens the same directory with it; if that fails, the database and the engine stay as they were.

The two engines keep separate data in the same directory: `catalog.csv` and one CSV file per table for `flatfile`, and `paged.db` for `paged`. A directory can hold both, and neither engine reads the other's data. When a directory has the other engine's data and none of its own, `.open` adds a `note:` line that says so.

Queries, writes and constraints behave the same on both engines, apart from a few known differences that Section 2.8 of the [design document](docs/DesignDocument.md) lists. `paged` supports every statement that `flatfile` does, and `.import` too.

A session with `paged`, in a temporary directory:

```bash
CLI="$PWD/build/strata_cli"
cd "$(mktemp -d)"
printf '%s\n' '.engine paged' '.open demo' \
  'CREATE TABLE students (id INT PRIMARY KEY, name TEXT UNIQUE);' \
  "INSERT INTO students VALUES (1, 'ann'), (2, 'bob');" \
  'SELECT id, name FROM students ORDER BY id;' | "$CLI"
```

```text
engine paged
created new database demo
OK CREATE TABLE students
OK affected=2
id	name
1	ann
2	bob
```

Each write statement is written to `demo/paged.db` as soon as it succeeds, so the data is there when the database is opened again in the same shell:

```bash
printf '%s\n' '.engine paged' '.open demo' 'SELECT COUNT(*) FROM students;' | "$CLI"
```

```text
engine paged
loaded students=2
COUNT(*)
2
```

A table whose primary key is `INT` also has a B+Tree index on that key: a sorted tree of pages in which a key is found by reading one page per level. A `WHERE` that compares the key with an integer literal (`=`, `<`, `<=`, `>` or `>=`, alone or inside `AND`) reads only the rows in that range instead of the whole table, and an `INSERT` that supplies the key into a table without a `UNIQUE` column checks for a duplicate through the index. A row that the `WHERE` does not reach is not read, so an error that the `WHERE` would raise on that row is not raised; Section 2.8 of the design document lists this difference. `.dump index <table>` prints the height (the number of levels), the node count (a node is one page of the tree), the key count, the first 40 nodes level by level from the root and the result of the index check, and answers `error: bind: table <table> has no index` for a table with a `TEXT` key. For the table above:

```bash
printf '%s\n' '.engine paged' '.open demo' '.dump index students' | "$CLI"
```

```text
engine paged
loaded students=2
index students(id): height 1, nodes 1, keys 2
level 0 page 4 keys 2 [1 .. 2]
check: ok
```

`ALTER TABLE ... ADD COLUMN` copies the rows into a new set of pages and switches to it, and `DROP TABLE` puts the pages of the table on a list of free pages that later tables reuse. The file does not get smaller.

```bash
printf '%s\n' '.engine paged' '.open demo' \
  'ALTER TABLE students ADD COLUMN age INT DEFAULT 0;' \
  'SELECT id, name, age FROM students ORDER BY id;' \
  'DROP TABLE students;' | "$CLI"
```

```text
engine paged
loaded students=2
OK ALTER TABLE students
id	name	age
1	ann	0
2	bob	0
OK DROP TABLE students
```

## Limitations

Both engines:

- There is no NULL. `NOT NULL` is accepted but has no effect, and the length in `VARCHAR(n)` is not checked.
- Constraints are column-level: a primary key or foreign key covers one column, and there are no table-level constraints and no `DROP COLUMN`.
- There is no query optimizer, and joins run in `FROM` order.
- Auto-increment takes the current maximum plus 1, so deleting the row with the maximum frees that value for reuse.
- Only one process can open a database directory at a time (`.strata.lock`, `flock`).
- `fsync`, the call that forces written data onto the disk, is not made after a statement, so a write reported as done can still be lost in a power failure, and a crash in the middle of a write is not recovered.

Only `flatfile`:

- There are no indexes, so a point lookup and a constraint check scan the table.
- Every row of every table is held in memory.
- A TEXT value or a TEXT `DEFAULT` cannot contain a newline. Commas and double quotes are stored with CSV quoting.
- A row that cannot be parsed at load (wrong column count, malformed number, unpaired quotes) is not loaded, and the next `UPDATE`, `DELETE` or `ALTER TABLE` rewrite of the table drops it from the file. `INSERT` only appends a row and leaves those lines in place.
- Load does not check `UNIQUE` and foreign keys, which apply to later statements only.
- The lock does not stop a text editor. If a file's size or modification time changes outside the CLI after loading, a write statement prints `error: io: <file name> changed on disk since it was loaded; run .open again` and writes no file. An edit that leaves both unchanged goes unnoticed.

Only `paged`:

- A row that takes more than 4060 bytes in a page is refused with `error: unsupported: paged engine cannot store a row longer than 4060 bytes in <table>`, because there are no overflow pages.
- A table name that starts with `sys_` is refused with `error: unsupported: table name '<name>' starts with the reserved prefix sys_`.
- The file does not shrink. The pages of a dropped table go on a free list and later tables reuse them.
- A crash in the middle of a statement that writes several pages can leave some of them written. A torn page fails its checksum, and a bad header, catalog or free list is refused at open, both as `corrupt`. Pages that are each intact but come from different moments are not caught that way: the index can lack the key of a row, so a lookup by that key finds nothing and an `INSERT` of that key succeeds. Only `.dump index <table>` reports the disagreement, and nothing repairs it.
- A damaged index root fails `.open` for the whole database, tables that are intact included. A damaged node below the root leaves scans working and fails the lookups that reach it and the `DROP TABLE` of its table. There is no repair command and no export command: the way out is a copy of the data, such as the source CSV files imported again into a new directory.
- An `.import` that stops midway on an I/O error leaves an empty table behind, and the message ends with `; table <name> was created`. A bad row is rejected before the table is created, and `flatfile` changes nothing in either case.
- A sort, a hash join (a join that holds one side in an in-memory table) or an aggregation is not bounded by the buffer pool, so nothing is promised for data larger than memory.
- Only a primary key of type `INT` has an index. A `TEXT` key and a `UNIQUE` column are checked by scanning, and so is a foreign key column when a row that it references is deleted or changed (the check of an `INSERT` into the child table reads the parent through its index when the parent key is an `INT`). An `INSERT` that takes the next auto-increment value scans the table once per statement, so a loop of single-row inserts into such a table takes time that grows with the square of the row count (ten times the rows, a hundred times the time) (one multi-row `INSERT` scans once). A `WHERE` with no top-level `AND` term that compares the key with an integer literal scans the whole table, as in `id + 0 = 5`, `id <> 5`, `id = 1 OR id = 2` and `id = 2.5`. Nothing chooses between an index range and a scan, so a range that covers most of the table reads more pages than a scan would.
- A delete leaves the index nodes in place, so an index does not shrink, and rows are read from the index in key order, which can differ from the order of a scan when a statement has no `ORDER BY`.
- A `paged` file written before the index existed (format version 1) is refused with an error; import the data again into a new directory.

A Write-Ahead Log (WAL) and crash recovery are planned for `paged`.

## Tests

Run all tests, or only those whose name contains a string:

```bash
./build/strata_tests              # all
./build/strata_tests test_binder  # tests whose name contains test_binder
```

The tests use a small harness in the repository, so no framework is needed. No test reads a file outside the repository: the volume tests generate their data from a fixed seed (three tables of 10000 rows each, in `tests/test_scale_data_support.cpp`), and most end-to-end tests of the CLI run on a copy of `sample_my_db/`.

A second check compares Strata with SQLite. A Python script runs the statements of `tests/differential/corpus.sql` on the `sqlite3` module and on both engines of `strata_cli`, one process per statement, so each statement reads the database that the one before it wrote. Python 3.10 or later is enough, with no extra package. A statement that is known to differ from SQLite is marked in the corpus, and the script fails when a marked statement stops differing as well as when an unmarked one starts.

```bash
python3 -I tests/differential/differential.py --cli build/strata_cli
STRATA_CLI_PATH="$PWD/build/strata_cli" python3 -I -m unittest discover -s tests/differential -p 'test_*.py'
```

The unit tests of the comparison run without the variable `STRATA_CLI_PATH`, and the three tests that run the whole corpus are skipped unless it names `strata_cli`.

## Acknowledgement

The ideas of Strata are inspired by the flat-file database exercise in CS 6422: Database System Implementation at the Georgia Institute of Technology.
