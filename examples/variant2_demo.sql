DROP DATABASE course;
CREATE DATABASE course;
USE course;
CREATE TABLE students (
  id int INDEXED,
  name string NOT_NULL,
  group_name string DEFAULT "IU7",
  age int
);

INSERT INTO students (id, name, age) VALUE
  (1, "Ivan", 19),
  (2, "Anna", 20),
  (3, "Petr", 21);

SELECT * FROM students;
SELECT (id, name AS student, age) FROM students WHERE id == 2;
SELECT (COUNT(id) AS total, AVG(age) AS avg_age, SUM(age) AS sum_age) FROM students WHERE age >= 19 AND age < 22;
UPDATE students SET age = 22 WHERE name LIKE "A.*";
SELECT * FROM students WHERE age BETWEEN 20 AND 23;
DELETE FROM students WHERE id == 3;
SELECT * FROM students;
