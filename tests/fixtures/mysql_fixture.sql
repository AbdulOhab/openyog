-- Fixture for the live MySQL/MariaDB selftests (tests/CMakeLists.txt,
-- OPENYOG_TEST_MYSQL), loaded into that database before they run.
-- It DROPs and recreates the objects below: point OPENYOG_TEST_MYSQL at a
-- throwaway database, never a real one.
DROP VIEW IF EXISTS v_employee_dept;
DROP TABLE IF EXISTS employees;
DROP TABLE IF EXISTS emp_dept;
DROP PROCEDURE IF EXISTS refresh_stats;
DROP FUNCTION IF EXISTS calc_tax;

CREATE TABLE emp_dept (
    id   INT PRIMARY KEY,
    name VARCHAR(50) NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE employees (
    id      INT AUTO_INCREMENT PRIMARY KEY,
    name    VARCHAR(100) NOT NULL,
    salary  DECIMAL(10,2) NULL,
    city    VARCHAR(50) NULL,
    dept_id INT NULL,
    hired   DATE NULL,
    photo   BLOB NULL,
    KEY idx_employees_city (city),
    CONSTRAINT fk_emp_dept FOREIGN KEY (dept_id) REFERENCES emp_dept(id) ON DELETE SET NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE VIEW v_employee_dept AS
SELECT e.id, e.name, d.name AS dept
FROM employees e LEFT JOIN emp_dept d ON d.id = e.dept_id;

DELIMITER $$
CREATE TRIGGER trg_employees_no_negative_salary BEFORE INSERT ON employees FOR EACH ROW
BEGIN
    IF NEW.salary < 0 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'salary must not be negative';
    END IF;
END$$
CREATE PROCEDURE refresh_stats()
BEGIN
    SELECT COUNT(*) FROM employees;
END$$
CREATE FUNCTION calc_tax(amount DECIMAL(10,2)) RETURNS DECIMAL(10,2) DETERMINISTIC
    RETURN amount * 0.15$$
DELIMITER ;

INSERT INTO emp_dept VALUES (1, 'Engineering'), (2, 'Sales');
-- a BLOB that isn't valid UTF-8 (with a NUL and 0xFF) and a non-ASCII name:
-- what the dump round trip compares byte for byte
INSERT INTO employees (name, salary, city, dept_id, hired, photo) VALUES
    ('Alice', 5000, 'Dhaka', 1, '2024-01-15', NULL),
    ('Karim', 4200, 'Chattogram', 2, '2023-06-01', X'89504E4700FF'),
    ('রহিম', NULL, NULL, 1, NULL, NULL),
    ('Dana', 6100.50, 'Sylhet', NULL, '2022-11-30', NULL);
