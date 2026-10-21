-- Fixture for the live PostgreSQL selftests (tests/CMakeLists.txt,
-- OPENYOG_TEST_PG), loaded into that database's public schema before they
-- run — the layout --pgtest= checks, plus a UNIQUE and a CHECK constraint
-- the dump round trip has to carry over. It DROPs and recreates the objects
-- below: point OPENYOG_TEST_PG at a throwaway database, never a real one.
DROP VIEW IF EXISTS v_employee_dept;
DROP TABLE IF EXISTS employees;
DROP TABLE IF EXISTS emp_dept;
DROP FUNCTION IF EXISTS trg_no_negative_salary() CASCADE;

CREATE TABLE emp_dept (
    id   INTEGER PRIMARY KEY,
    name TEXT NOT NULL UNIQUE
);

CREATE TABLE employees (
    id      SERIAL PRIMARY KEY,
    name    TEXT NOT NULL,
    salary  NUMERIC(10,2) CHECK (salary IS NULL OR salary < 1000000),
    city    TEXT,
    dept_id INTEGER REFERENCES emp_dept(id) ON DELETE SET NULL ON UPDATE CASCADE,
    hired   DATE,
    photo   BYTEA
);
CREATE INDEX idx_employees_city ON employees(city);

CREATE VIEW v_employee_dept AS
SELECT e.id, e.name, d.name AS dept
FROM employees e LEFT JOIN emp_dept d ON d.id = e.dept_id;

CREATE FUNCTION trg_no_negative_salary() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    IF NEW.salary < 0 THEN
        RAISE EXCEPTION 'salary must not be negative';
    END IF;
    RETURN NEW;
END;
$$;
CREATE TRIGGER trg_employees_no_negative_salary
    BEFORE INSERT ON employees
    FOR EACH ROW EXECUTE FUNCTION trg_no_negative_salary();

INSERT INTO emp_dept VALUES (1, 'Engineering'), (2, 'Sales');
-- a bytea that isn't valid UTF-8 (with a NUL and 0xFF) and a non-ASCII name:
-- what the dump round trip compares byte for byte
INSERT INTO employees (name, salary, city, dept_id, hired, photo) VALUES
    ('Alice', 5000, 'Dhaka', 1, '2024-01-15', NULL),
    ('Karim', 4200, 'Chattogram', 2, '2023-06-01', '\x89504e4700ff'),
    ('রহিম', NULL, NULL, 1, NULL, NULL),
    ('Dana', 6100.50, 'Sylhet', NULL, '2022-11-30', NULL);
