-- The statements that tests/differential/differential.py runs on SQLite and on
-- both engines of strata_cli, in order. A statement ends with a semicolon
-- outside a string literal. A comment line of the form
--   -- differs: <area>   the next statement differs from SQLite in <area>
--   -- order: total      the next statement has a total order: its rows are
--                        compared as a sequence
-- applies to the next statement only. Every computed column has an alias,
-- because the two systems name an expression differently.

-- Schema and data.
CREATE TABLE dept (id INT PRIMARY KEY, name TEXT UNIQUE NOT NULL, budget DOUBLE NOT NULL);
CREATE TABLE emp (id INT PRIMARY KEY, name TEXT NOT NULL, dept_id INT NOT NULL REFERENCES dept(id), salary INT NOT NULL, rating DOUBLE NOT NULL);
CREATE TABLE task (id INT PRIMARY KEY, emp_id INT NOT NULL REFERENCES emp(id), title TEXT NOT NULL, hours DOUBLE NOT NULL);
CREATE TABLE tag (id INT PRIMARY KEY, task_id INT NOT NULL REFERENCES task(id), label TEXT NOT NULL);
CREATE TABLE bigtab (id INT PRIMARY KEY, big INT NOT NULL);
CREATE TABLE log (id INTEGER PRIMARY KEY AUTOINCREMENT, msg TEXT NOT NULL);
INSERT INTO dept VALUES (1, 'Eng', 500000.0), (2, 'Ops', 250000.5), (3, 'Art', 120000.25), (4, 'Lab', 0.0);
INSERT INTO emp VALUES (1, 'ann', 1, 50, 4.5), (2, 'bob', 1, 40, 3.0), (3, 'cai', 2, 30, 4.0), (4, 'dave ', 2, 35, 2.5), (5, 'café', 3, 20, 5.0), (6, 'eve', 1, 60, 1.5), (7, 'fay', 3, 25, 0.0), (8, 'gus', 2, 45, 3.5), (9, 'hal', 1, 40, 4.5), (10, 'ivy', 3, 55, 2.0);
INSERT INTO task VALUES (1, 1, 'design', 3.5), (2, 1, 'review', 1.0), (3, 2, 'design', 2.0), (4, 2, 'test', 4.5), (5, 3, 'test', 6.0), (6, 3, 'deploy', 0.5), (7, 4, 'design', 1.5), (8, 5, 'paint', 8.0), (9, 5, 'review', 2.5), (10, 6, 'test', 3.0), (11, 7, 'paint', 5.5), (12, 8, 'deploy', 1.0), (13, 8, 'test', 2.5), (14, 9, 'design', 4.0), (15, 9, 'review', 0.5), (16, 10, 'paint', 7.0), (17, 1, 'test', 2.0), (18, 3, 'review', 1.5), (19, 5, 'design', 3.0), (20, 8, 'review', 2.0);
INSERT INTO tag VALUES (1, 1, 'urgent'), (2, 1, 'ui'), (3, 2, 'late'), (4, 4, 'urgent'), (5, 5, 'slow'), (6, 8, 'ui'), (7, 8, 'urgent'), (8, 11, 'late'), (9, 12, 'ui'), (10, 14, 'urgent'), (11, 16, 'slow'), (12, 17, 'late'), (13, 19, 'ui'), (14, 20, 'urgent'), (15, 20, 'slow');
INSERT INTO bigtab VALUES (1, 9007199254740993), (2, 9223372036854775807);
INSERT INTO log (msg) VALUES ('first'), ('second');
INSERT INTO log (msg) VALUES ('third');

-- Projection, filters and expressions.
-- order: total
SELECT id, name, salary FROM emp ORDER BY id;
-- order: total
SELECT id, name, salary FROM emp WHERE salary >= 40 AND rating > 3.0 ORDER BY id;
-- order: total
SELECT id, name FROM emp WHERE salary < 30 OR rating >= 5.0 ORDER BY id;
-- order: total
SELECT id, salary * 2 AS doubled, salary + 5 AS plus, salary - 100 AS minus, salary / 7 AS quotient FROM emp ORDER BY id;
-- order: total
SELECT id, rating * 2 AS twice, rating + salary AS mixed, rating / 2 AS half FROM emp ORDER BY id;
-- order: total
SELECT id, -salary AS negated, 0 - salary AS flipped, (salary + 3) * (salary - 3) AS product FROM emp ORDER BY id;
-- order: total
SELECT id, name FROM emp WHERE name = 'dave ' ORDER BY id;
-- order: total
SELECT id, name FROM emp WHERE name <> 'ann' AND name <= 'fay' ORDER BY id;
SELECT * FROM dept;
SELECT * FROM emp WHERE id > 100;
-- order: total
SELECT id, name AS who, salary AS pay FROM emp WHERE id <= 3 ORDER BY who DESC;
-- order: total
SELECT e.id, e.name FROM emp e WHERE e.dept_id = 2 ORDER BY e.id;

-- Functions and LIKE.
-- order: total
SELECT id, UPPER(name) AS up, LOWER(name) AS low, LENGTH(name) AS len FROM emp ORDER BY id;
-- order: total
SELECT id, TRIM(name) AS trimmed, LENGTH(TRIM(name)) AS len FROM emp WHERE id = 4 ORDER BY id;
-- order: total
SELECT id, SUBSTR(name, 2) AS tail, SUBSTR(name, 1, 2) AS head, SUBSTR(name, 2, 1) AS mid FROM emp ORDER BY id;
-- order: total
SELECT id, name || '-' || title AS joined FROM emp JOIN task ON task.emp_id = emp.id WHERE task.id <= 4 ORDER BY task.id;
-- order: total
SELECT id, ABS(salary - 40) AS dist, ABS(rating - 3.0) AS gap FROM emp ORDER BY id;
-- order: total
SELECT id, ROUND(rating) AS r0, ROUND(rating * 1.25, 1) AS r1 FROM emp ORDER BY id;
-- order: total
SELECT id, CAST(salary AS DOUBLE) AS d, CAST(salary AS TEXT) AS t, CAST('42' AS INT) AS n, CAST('2.5' AS DOUBLE) AS f FROM emp WHERE id <= 3 ORDER BY id;
-- order: total
SELECT id, name FROM emp WHERE name LIKE 'a%' OR name LIKE '%e' ORDER BY id;
-- order: total
SELECT id, name FROM emp WHERE name LIKE '_a_' ORDER BY id;
-- order: total
SELECT id, name FROM emp WHERE name LIKE 'CAF_' ORDER BY id;
-- order: total
SELECT id, name FROM emp WHERE name LIKE 'D%' ORDER BY id;
-- order: total
SELECT id, name FROM emp WHERE name LIKE '%' ORDER BY id LIMIT 3;

-- Sorting, LIMIT and OFFSET.
-- order: total
SELECT id, salary FROM emp ORDER BY salary DESC, id;
-- order: total
SELECT id, rating FROM emp ORDER BY rating, id DESC;
-- order: total
SELECT id, name FROM emp ORDER BY name, id;
-- order: total
SELECT id, salary FROM emp ORDER BY 2 DESC, 1 LIMIT 4;
-- order: total
SELECT id, salary FROM emp ORDER BY salary, id LIMIT 3 OFFSET 2;
-- order: total
SELECT id FROM emp ORDER BY id LIMIT 0;
-- order: total
SELECT id FROM emp ORDER BY id LIMIT 100 OFFSET 8;
-- order: total
SELECT id FROM emp ORDER BY id LIMIT 3 OFFSET 20;

-- Aggregates.
SELECT COUNT(*) AS n, SUM(salary) AS total, MIN(salary) AS lo, MAX(salary) AS hi, AVG(salary) AS mean FROM emp;
SELECT COUNT(name) AS n, SUM(rating) AS r, MIN(rating) AS lo, MAX(rating) AS hi, AVG(rating) AS mean FROM emp;
SELECT MIN(name) AS first, MAX(name) AS last FROM emp;
-- order: total
SELECT dept_id, COUNT(*) AS n, SUM(salary) AS total, MAX(rating) AS top FROM emp GROUP BY dept_id ORDER BY dept_id;
-- order: total
SELECT dept_id, AVG(salary) AS mean, MIN(rating) AS low FROM emp WHERE salary > 25 GROUP BY dept_id ORDER BY 1;
-- order: total
SELECT title, COUNT(*) AS n, SUM(hours) AS spent FROM task GROUP BY title ORDER BY n DESC, title;
SELECT dept_id, COUNT(*) AS n FROM emp GROUP BY dept_id;
SELECT COUNT(*) AS n FROM emp WHERE salary > 1000;
-- order: total
SELECT salary + 0 AS pay, COUNT(*) AS n FROM emp GROUP BY salary + 0 ORDER BY pay;

-- Joins.
-- order: total
SELECT e.id, e.name, d.name AS dept FROM emp e JOIN dept d ON e.dept_id = d.id ORDER BY e.id;
-- order: total
SELECT t.id, e.id AS emp, t.title, t.hours FROM emp e JOIN task t ON t.emp_id = e.id WHERE t.hours > 4 ORDER BY t.id;
-- order: total
SELECT t.id, d.name AS dept, e.name AS who, t.title FROM dept d JOIN emp e ON e.dept_id = d.id JOIN task t ON t.emp_id = e.id WHERE t.title = 'test' ORDER BY t.id;
-- order: total
SELECT g.id, d.name AS dept, e.name AS who, t.title, g.label FROM dept d JOIN emp e ON e.dept_id = d.id JOIN task t ON t.emp_id = e.id JOIN tag g ON g.task_id = t.id ORDER BY g.id;
-- order: total
SELECT d.name AS dept, COUNT(*) AS n, SUM(t.hours) AS spent FROM dept d JOIN emp e ON e.dept_id = d.id JOIN task t ON t.emp_id = e.id GROUP BY d.name ORDER BY spent DESC, dept;
-- order: total
SELECT g.label, COUNT(*) AS n FROM tag g JOIN task t ON g.task_id = t.id JOIN emp e ON t.emp_id = e.id WHERE e.dept_id = 1 GROUP BY g.label ORDER BY n DESC, g.label;
-- order: total
SELECT a.id AS first, b.id AS second FROM emp a JOIN emp b ON a.dept_id = b.dept_id AND a.id < b.id WHERE a.dept_id = 3 ORDER BY a.id, b.id;
-- order: total
SELECT e.id, e.name FROM emp e JOIN dept d ON e.dept_id = d.id AND d.budget > 200000 ORDER BY e.id;
SELECT e.name, d.name AS dept FROM emp e JOIN dept d ON e.dept_id = d.id WHERE d.id = 99;
-- order: total
SELECT t.id, g.id AS tag, g.label FROM task t JOIN tag g ON g.task_id = t.id WHERE t.hours >= 2.0 AND g.label <> 'ui' ORDER BY t.id, g.id;

-- Writes and constraints.
INSERT INTO emp VALUES (11, 'jon', 4, 33, 3.25);
-- order: total
SELECT id, name, dept_id FROM emp WHERE id > 9 ORDER BY id;
INSERT INTO emp VALUES (11, 'dup', 4, 1, 1.0);
INSERT INTO emp VALUES (12, 'orphan', 99, 1, 1.0);
INSERT INTO dept VALUES (5, 'Eng', 1.0);
INSERT INTO task VALUES (21, 99, 'ghost', 1.0);
INSERT INTO emp VALUES (12, 'kim', 4, 28, 3.75), (13, 'lee', 4, 31, 2.25);
INSERT INTO emp VALUES (14, 'ok', 4, 1, 1.0), (14, 'twice', 4, 2, 2.0);
-- order: total
SELECT id, name FROM emp WHERE id >= 11 ORDER BY id;
UPDATE emp SET salary = salary + 5 WHERE dept_id = 4;
UPDATE emp SET rating = 1.0, name = 'jonny' WHERE id = 11;
UPDATE emp SET dept_id = 99 WHERE id = 11;
UPDATE dept SET name = 'Eng' WHERE id = 4;
UPDATE emp SET salary = salary WHERE id = 1;
UPDATE emp SET salary = 0 WHERE id = 999;
-- order: total
SELECT id, name, salary, rating FROM emp WHERE dept_id = 4 ORDER BY id;
DELETE FROM dept WHERE id = 1;
DELETE FROM emp WHERE dept_id = 4;
DELETE FROM dept WHERE id = 4;
DELETE FROM tag WHERE label = 'late';
DELETE FROM tag WHERE id = 999;
-- order: total
SELECT id, label FROM tag ORDER BY id;
INSERT INTO log (msg) VALUES ('fourth'), ('fifth');
-- order: total
SELECT id, msg FROM log ORDER BY id;

-- ALTER TABLE ADD COLUMN and DROP TABLE.
ALTER TABLE task ADD COLUMN prio INT NOT NULL DEFAULT 0;
ALTER TABLE task ADD COLUMN weight DOUBLE NOT NULL DEFAULT 0.5;
ALTER TABLE task ADD COLUMN note TEXT NOT NULL DEFAULT 'none';
-- order: total
SELECT * FROM task WHERE id <= 4 ORDER BY id;
INSERT INTO task (id, emp_id, title, hours) VALUES (30, 1, 'late work', 9.5);
INSERT INTO task VALUES (31, 2, 'with all', 1.0, 3, 2.5, 'given');
UPDATE task SET prio = 2, note = 'urgent' WHERE hours > 6;
UPDATE task SET weight = weight * 2 WHERE id = 30;
-- order: total
SELECT id, prio, weight, note FROM task WHERE id = 30 OR id = 31 OR hours > 6 ORDER BY id;
-- order: total
SELECT prio, COUNT(*) AS n, SUM(weight) AS w FROM task GROUP BY prio ORDER BY prio;
-- order: total
SELECT t.id, e.name, t.note FROM emp e JOIN task t ON t.emp_id = e.id WHERE t.prio > 0 ORDER BY t.id;
ALTER TABLE task ADD COLUMN prio INT NOT NULL DEFAULT 1;
ALTER TABLE nosuch ADD COLUMN x INT NOT NULL DEFAULT 0;
ALTER TABLE emp ADD COLUMN bonus INT NOT NULL DEFAULT 0;
-- order: total
SELECT id, name, bonus FROM emp WHERE id <= 3 ORDER BY id;
UPDATE emp SET bonus = salary / 10 WHERE bonus = 0;
-- order: total
SELECT id, salary, bonus FROM emp WHERE id <= 4 ORDER BY id;
DROP TABLE tag;
SELECT * FROM tag;
CREATE TABLE tag (id INT PRIMARY KEY, label TEXT NOT NULL, weight DOUBLE NOT NULL DEFAULT 1.5);
INSERT INTO tag (id, label) VALUES (1, 'fresh'), (2, 'new');
-- order: total
SELECT * FROM tag ORDER BY id;
DROP TABLE tag;
DROP TABLE tag;
CREATE TABLE tag (id INT PRIMARY KEY, task_id INT NOT NULL REFERENCES task(id), label TEXT NOT NULL);
INSERT INTO tag VALUES (1, 1, 'again');
INSERT INTO tag VALUES (2, 999, 'orphan');
-- order: total
SELECT * FROM tag ORDER BY id;
DROP TABLE bigtab;
DROP TABLE log;

-- Errors that both systems report.
SELECT nosuch FROM emp;
SELECT * FROM nosuch;
SELECT emp.nosuch FROM emp;
SELECT id FROM emp JOIN task ON task.emp_id = emp.id;
SELEKT 1;
SELECT id FROM emp WHERE;
INSERT INTO emp VALUES (1);
INSERT INTO nosuch VALUES (1);
UPDATE nosuch SET a = 1;
DELETE FROM nosuch;
CREATE TABLE emp (id INT PRIMARY KEY);
DROP TABLE nosuch;

-- Differences from SQLite, each marked with its area (Design Document 2.3).
-- differs: Aggregates over no rows
SELECT SUM(salary) AS total FROM emp WHERE id > 999;
-- differs: Aggregates over no rows
SELECT AVG(salary) AS mean FROM emp WHERE id > 999;
-- differs: Aggregates over no rows
SELECT MIN(salary) AS lo FROM emp WHERE id > 999;
-- differs: Division by zero
SELECT id, salary / 0 AS q FROM emp WHERE id = 1;
-- differs: Division by zero
SELECT id, rating / 0.0 AS q FROM emp WHERE id = 1;
CREATE TABLE bigtab (id INT PRIMARY KEY, big INT NOT NULL);
INSERT INTO bigtab VALUES (1, 9007199254740993), (2, 9223372036854775807);
-- differs: INT overflow
SELECT id, big + 1 AS next FROM bigtab WHERE id = 2;
-- differs: INT overflow
SELECT id, big * 2 AS twice FROM bigtab WHERE id = 2;
-- differs: TEXT with a number
SELECT id, name || salary AS mixed FROM emp WHERE id = 1;
-- differs: TEXT with a number
SELECT id FROM emp WHERE name = 1;
-- differs: CAST of text
SELECT id, CAST('12abc' AS INT) AS n FROM dept WHERE id = 1;
-- differs: CAST of text
SELECT id, CAST('abc' AS DOUBLE) AS f FROM dept WHERE id = 1;
-- differs: ROUND and SUBSTR ranges
SELECT id, SUBSTR(name, 1, -1) AS s FROM dept WHERE id = 1;
-- differs: ROUND and SUBSTR ranges
SELECT id, ROUND(budget, -2) AS r FROM dept WHERE id = 1;
-- differs: SUBSTR negative start
SELECT id, SUBSTR(name, -2) AS s FROM dept WHERE id = 1;
-- differs: Conditions
SELECT id FROM emp WHERE salary;
-- differs: Conditions
SELECT id, salary > 40 AS high FROM emp WHERE id = 1;
-- differs: ORDER BY and GROUP BY rules
SELECT id FROM emp ORDER BY salary;
-- differs: ORDER BY and GROUP BY rules
SELECT name, COUNT(*) AS n FROM emp;
-- differs: LIMIT -1
SELECT id FROM emp ORDER BY id LIMIT -1;
-- differs: INT against DOUBLE
SELECT id FROM bigtab WHERE big = 9007199254740992.0;
-- differs: DOUBLE text
SELECT id, CAST(rating * -1 AS TEXT) AS neg FROM emp WHERE id = 7;
CREATE TABLE parent (id INT PRIMARY KEY);
CREATE TABLE child (id INT PRIMARY KEY, parent_id INT REFERENCES parent(id));
-- differs: ADD COLUMN default
ALTER TABLE child ADD COLUMN note TEXT;
-- differs: DROP of a referenced table
DROP TABLE parent;

-- Reads by key: a key that is present and one that is absent, ranges, the ends
-- of the key range, a DOUBLE bound, and the same reads after an update that
-- grows a row, a delete and an insert of the same key, ADD COLUMN, and DROP
-- TABLE with the table created again. A paged table reads these through its
-- index.
CREATE TABLE keyed (id INT PRIMARY KEY, label TEXT NOT NULL, n INT NOT NULL);
INSERT INTO keyed VALUES (1, 'a', 10), (2, 'b', 20), (3, 'c', 30), (4, 'd', 40), (5, 'e', 50), (6, 'f', 60);
SELECT id, label FROM keyed WHERE id = 3;
SELECT id, label FROM keyed WHERE id = 99;
-- order: total
SELECT id, n FROM keyed WHERE id >= 2 AND id < 5 ORDER BY id;
-- order: total
SELECT id FROM keyed WHERE id < 3 ORDER BY id;
-- order: total
SELECT id FROM keyed WHERE id > 4 ORDER BY id;
SELECT id FROM keyed WHERE id = 0;
SELECT id FROM keyed WHERE id = -1;
-- order: total
SELECT id FROM keyed WHERE id < 2.5 ORDER BY id;
UPDATE keyed SET label = 'a label that is much longer than the one it replaces, so that the row grows' WHERE id = 3;
SELECT id, label FROM keyed WHERE id = 3;
-- order: total
SELECT id FROM keyed WHERE id >= 3 ORDER BY id;
DELETE FROM keyed WHERE id = 4;
SELECT id FROM keyed WHERE id = 4;
INSERT INTO keyed VALUES (4, 'd2', 41);
SELECT id, label, n FROM keyed WHERE id = 4;
ALTER TABLE keyed ADD COLUMN extra INT NOT NULL DEFAULT 7;
SELECT id, extra FROM keyed WHERE id = 6;
-- order: total
SELECT id, extra FROM keyed WHERE id <= 2 ORDER BY id;
DROP TABLE keyed;
CREATE TABLE keyed (id INT PRIMARY KEY, label TEXT NOT NULL, n INT NOT NULL);
INSERT INTO keyed VALUES (5, 'x', 1), (2, 'y', 2);
SELECT id, label FROM keyed WHERE id = 2;
-- order: total
SELECT id FROM keyed WHERE id >= 3 ORDER BY id;
UPDATE keyed SET n = n + 1 WHERE id = 5;
DELETE FROM keyed WHERE id > 4;
-- order: total
SELECT id, n FROM keyed ORDER BY id;
