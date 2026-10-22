-- Target for the live Schema Sync selftest (tests/CMakeLists.txt): the
-- PostgreSQL fixture's tables with one difference of each kind, in a schema
-- of their own (sync_target). The selftest makes it match the fixture's
-- public schema and compares again. It drops and recreates that schema.
DROP SCHEMA IF EXISTS sync_target CASCADE;
CREATE SCHEMA sync_target;

-- name varchar and nullable; city and photo missing; retired extra; the
-- CHECK, idx_employees_city and the foreign key missing; idx_employees_name
-- extra
CREATE TABLE sync_target.employees (
    id      SERIAL PRIMARY KEY,
    name    VARCHAR(60),
    salary  NUMERIC(10,2),
    dept_id INTEGER,
    hired   DATE,
    retired BOOLEAN
);
CREATE INDEX idx_employees_name ON sync_target.employees (name);

-- not in the source: dropped (when picked)
CREATE TABLE sync_target.legacy_notes (
    id   INTEGER PRIMARY KEY,
    note TEXT
);

-- emp_dept is missing altogether: created
INSERT INTO sync_target.employees (name, salary) VALUES ('Old timer', 100);
