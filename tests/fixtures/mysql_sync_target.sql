-- Target for the live Schema Sync selftest (tests/CMakeLists.txt): the
-- MySQL fixture's tables with one difference of each kind, loaded into
-- <db>_sync. The selftest makes it match the fixture and compares again. It
-- DROPs and recreates the objects below.
DROP VIEW IF EXISTS v_employee_dept;
DROP PROCEDURE IF EXISTS refresh_stats;
DROP PROCEDURE IF EXISTS legacy_cleanup;
DROP FUNCTION IF EXISTS calc_tax;
DROP TABLE IF EXISTS employees;
DROP TABLE IF EXISTS emp_dept;
DROP TABLE IF EXISTS legacy_notes;

-- name narrower; city and photo missing; retired extra; idx_employees_city
-- and the foreign key missing; idx_employees_name extra
CREATE TABLE employees (
    id      INT AUTO_INCREMENT PRIMARY KEY,
    name    VARCHAR(60) NOT NULL,
    salary  DECIMAL(10,2) NULL,
    dept_id INT NULL,
    hired   DATE NULL,
    retired TINYINT NULL,
    KEY idx_employees_name (name)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- not in the source: dropped (when picked)
CREATE TABLE legacy_notes (
    id   INT PRIMARY KEY,
    note TEXT
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- emp_dept is missing altogether: created
INSERT INTO employees (name, salary) VALUES ('Old timer', 100);

-- the view reads less: replaced; refresh_stats counts something else:
-- dropped and created again; calc_tax and the trigger are missing: created;
-- legacy_cleanup isn't in the source: dropped (when picked)
CREATE VIEW v_employee_dept AS SELECT e.id, e.name FROM employees e;

DELIMITER $$
CREATE PROCEDURE refresh_stats()
BEGIN
    SELECT COUNT(*) FROM legacy_notes;
END$$
CREATE PROCEDURE legacy_cleanup()
BEGIN
    DELETE FROM legacy_notes;
END$$
DELIMITER ;
