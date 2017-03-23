-- Generic extended statistics support
CREATE TABLE ab1 (a INTEGER, b INTEGER, c INTEGER);

CREATE STATISTICS ab1_a_b_stats ON (a, b) FROM ab1;
DROP STATISTICS ab1_a_b_stats;

CREATE SCHEMA regress_schema_2;
CREATE STATISTICS regress_schema_2.ab1_a_b_stats ON (a, b) FROM ab1;
DROP STATISTICS regress_schema_2.ab1_a_b_stats;

CREATE STATISTICS ab1_b_c_stats ON (b, c) FROM ab1;
CREATE STATISTICS ab1_a_b_c_stats ON (a, b, c) FROM ab1;
CREATE STATISTICS ab1_a_b_stats ON (a, b) FROM ab1;
ALTER TABLE ab1 DROP COLUMN a;
\d ab1

DROP TABLE ab1;


-- data type passed by value
CREATE TABLE ndistinct (
    filler1 TEXT,
    filler2 NUMERIC,
    a INT,
    b INT,
    filler3 DATE,
    c INT,
    d INT
);

-- unknown column
CREATE STATISTICS s10 ON (unknown_column) FROM ndistinct;

-- single column
CREATE STATISTICS s10 ON (a) FROM ndistinct;

-- single column, duplicated
CREATE STATISTICS s10 ON (a,a) FROM ndistinct;

-- two columns, one duplicated
CREATE STATISTICS s10 ON (a, a, b) FROM ndistinct;

-- correct command
CREATE STATISTICS s10 ON (a, b, c) FROM ndistinct;

-- perfectly correlated groups
INSERT INTO ndistinct (a, b, c, filler1)
     SELECT i/100, i/100, i/100, cash_words(i::money)
       FROM generate_series(1,10000) s(i);

ANALYZE ndistinct;

SELECT staenabled, standistinct
  FROM pg_statistic_ext WHERE starelid = 'ndistinct'::regclass;

EXPLAIN (COSTS off)
 SELECT COUNT(*) FROM ndistinct GROUP BY a, b;

EXPLAIN (COSTS off)
 SELECT COUNT(*) FROM ndistinct GROUP BY a, b, c;

EXPLAIN (COSTS off)
 SELECT COUNT(*) FROM ndistinct GROUP BY a, b, c, d;

TRUNCATE TABLE ndistinct;

-- partially correlated groups
INSERT INTO ndistinct (a, b, c)
     SELECT i/50, i/100, i/200 FROM generate_series(1,10000) s(i);

ANALYZE ndistinct;

SELECT staenabled, standistinct
  FROM pg_statistic_ext WHERE starelid = 'ndistinct'::regclass;

EXPLAIN
 SELECT COUNT(*) FROM ndistinct GROUP BY a, b;

EXPLAIN
 SELECT COUNT(*) FROM ndistinct GROUP BY a, b, c;

EXPLAIN
 SELECT COUNT(*) FROM ndistinct GROUP BY a, b, c, d;

EXPLAIN
 SELECT COUNT(*) FROM ndistinct GROUP BY b, c, d;

EXPLAIN
 SELECT COUNT(*) FROM ndistinct GROUP BY a, d;

DROP TABLE ndistinct;