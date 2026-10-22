-- Target for the live Data Sync selftest (tests/CMakeLists.txt): the MySQL
-- fixture's tables with rows of every kind of difference, loaded into
-- <db>_data. The selftest makes its rows match the fixture's and compares
-- again. It DROPs and recreates the tables below.
DROP TABLE IF EXISTS employees;
DROP TABLE IF EXISTS emp_dept;

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

-- 1 the same, 2 renamed (update), 3 not in the source (delete)
INSERT INTO emp_dept VALUES (1, 'Engineering'), (2, 'Marketing'), (3, 'Legacy');
-- 1 the same; 2 another salary and photo (update); 3 and 4 missing (insert);
-- 9 not in the source (delete), pointing at the row of emp_dept that goes too
INSERT INTO employees (id, name, salary, city, dept_id, hired, photo) VALUES
    (1, 'Alice', 5000, 'Dhaka', 1, '2024-01-15', NULL),
    (2, 'Karim', 4300, 'Chattogram', 2, '2023-06-01', X'00'),
    (9, 'Gone', 1, NULL, 3, NULL, NULL);
