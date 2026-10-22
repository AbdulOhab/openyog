-- Target for the live Data Sync selftest (tests/CMakeLists.txt): the
-- PostgreSQL fixture's tables with rows of every kind of difference, in a
-- schema of their own (datasync_target). The selftest makes its rows match
-- the fixture's public schema and compares again. It drops and recreates
-- that schema.
DROP SCHEMA IF EXISTS datasync_target CASCADE;
CREATE SCHEMA datasync_target;

CREATE TABLE datasync_target.emp_dept (
    id   INTEGER PRIMARY KEY,
    name TEXT NOT NULL UNIQUE
);
CREATE TABLE datasync_target.employees (
    id      SERIAL PRIMARY KEY,
    name    TEXT NOT NULL,
    salary  NUMERIC(10,2),
    city    TEXT,
    dept_id INTEGER REFERENCES datasync_target.emp_dept(id) ON DELETE SET NULL,
    hired   DATE,
    photo   BYTEA
);

-- 1 the same, 2 renamed (update), 3 not in the source (delete)
INSERT INTO datasync_target.emp_dept VALUES (1, 'Engineering'), (2, 'Marketing'), (3, 'Legacy');
-- 1 the same; 2 another salary and photo (update); 3 and 4 missing (insert);
-- 9 not in the source (delete), pointing at the row of emp_dept that goes too
INSERT INTO datasync_target.employees (id, name, salary, city, dept_id, hired, photo) VALUES
    (1, 'Alice', 5000, 'Dhaka', 1, '2024-01-15', NULL),
    (2, 'Karim', 4300, 'Chattogram', 2, '2023-06-01', '\x00'),
    (9, 'Gone', 1, NULL, 3, NULL, NULL);
