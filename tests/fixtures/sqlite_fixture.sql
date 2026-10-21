-- Fixture for the SQLite selftests (`openyog --sqlitetest=FILE` and the
-- other --sqlite*test= flags, qt/main.cpp): the layout --sqlitetest's shape
-- checks expect. employees has an INTEGER PRIMARY KEY id, a city index, an
-- inline FK to emp_dept and a BEFORE INSERT trigger, and there is one view.
-- SQLite has no routines or events.
-- ctest builds a fresh database from this file per test, on every run.
DROP VIEW IF EXISTS v_employee_dept;
DROP TABLE IF EXISTS employees;
DROP TABLE IF EXISTS emp_dept;

CREATE TABLE emp_dept (
    id   INTEGER PRIMARY KEY,
    name TEXT NOT NULL
);

CREATE TABLE employees (
    id      INTEGER PRIMARY KEY,
    name    TEXT NOT NULL,
    city    TEXT,
    salary  REAL,
    dept_id INTEGER REFERENCES emp_dept(id)
);
CREATE INDEX idx_employees_city ON employees(city);

CREATE TRIGGER trg_employees_no_negative_salary
BEFORE INSERT ON employees
WHEN NEW.salary < 0
BEGIN
    SELECT RAISE(ABORT, 'salary must not be negative');
END;

CREATE VIEW v_employee_dept AS
SELECT e.id, e.name, d.name AS dept
FROM employees e LEFT JOIN emp_dept d ON d.id = e.dept_id;

INSERT INTO emp_dept (id, name) VALUES (1, 'Engineering'), (2, 'Sales');
INSERT INTO employees (name, city, salary, dept_id) VALUES
    ('Alice', 'Dhaka', 5000, 1),
    ('Bob', 'Chattogram', NULL, 2);
