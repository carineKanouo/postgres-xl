--
-- XC_REMOTE
--

-- Test cases for Postgres-XC remote queries
-- Disable fast query shipping, all the queries go through standard planner
SET enable_fast_query_shipping TO false;

-- Create of non-Coordinator quals
CREATE FUNCTION func_stable (int) RETURNS int AS $$ SELECT $1 $$ LANGUAGE SQL STABLE;
CREATE FUNCTION func_volatile (int) RETURNS int AS $$ SELECT $1 $$ LANGUAGE SQL VOLATILE;
CREATE FUNCTION func_immutable (int) RETURNS int AS $$ SELECT $1 $$ LANGUAGE SQL IMMUTABLE;

-- Test for remote DML on different tables
CREATE TABLE rel_rep (a int, b int) DISTRIBUTE BY REPLICATION;
CREATE TABLE rel_hash (a int, b int) DISTRIBUTE BY HASH (a);
CREATE TABLE rel_rr (a int, b int) DISTRIBUTE BY ROUND ROBIN;
CREATE SEQUENCE seqtest START 10;
CREATE SEQUENCE seqtest2 START 100;

-- INSERT cases
INSERT INTO rel_rep VALUES (1,1);
INSERT INTO rel_hash VALUES (1,1);
INSERT INTO rel_rr VALUES (1,1);

-- Multiple entries with non-shippable expressions
INSERT INTO rel_rep VALUES (nextval('seqtest'), nextval('seqtest')), (1, nextval('seqtest'));
INSERT INTO rel_rep VALUES (nextval('seqtest'), 1), (nextval('seqtest'), nextval('seqtest2'));
INSERT INTO rel_hash VALUES (nextval('seqtest'), nextval('seqtest')), (1, nextval('seqtest'));
INSERT INTO rel_hash VALUES (nextval('seqtest'), 1), (nextval('seqtest'), nextval('seqtest2'));
INSERT INTO rel_rr VALUES (nextval('seqtest'), nextval('seqtest')), (1, nextval('seqtest'));
INSERT INTO rel_rr VALUES (nextval('seqtest'), 1), (nextval('seqtest'), nextval('seqtest2'));

-- Global check
SELECT a, b FROM rel_rep ORDER BY 1,2;
SELECT a, b FROM rel_hash ORDER BY 1,2;
SELECT a, b FROM rel_rr ORDER BY 1,2;

-- Some SELECT queries with some quals
-- Coordinator quals first
SELECT a, b FROM rel_rep WHERE a <= currval('seqtest') - 15 ORDER BY 1,2;
SELECT a, b FROM rel_hash WHERE a <= currval('seqtest') - 15 ORDER BY 1,2;
SELECT a, b FROM rel_rr WHERE a <= currval('seqtest') - 15 ORDER BY 1,2;
-- Non Coordinator quals
SELECT a, b FROM rel_rep WHERE a <= func_immutable(5) ORDER BY 1,2;
SELECT a, b FROM rel_hash WHERE a <= func_immutable(5) ORDER BY 1,2;
SELECT a, b FROM rel_rr WHERE a <= func_immutable(5) ORDER BY 1,2;
SELECT a, b FROM rel_rep WHERE a <= func_stable(5) ORDER BY 1,2;
SELECT a, b FROM rel_hash WHERE a <= func_stable(5) ORDER BY 1,2;
SELECT a, b FROM rel_rr WHERE a <= func_stable(5) ORDER BY 1,2;
SELECT a, b FROM rel_rep WHERE a <= func_volatile(5) ORDER BY 1,2;
SELECT a, b FROM rel_hash WHERE a <= func_volatile(5) ORDER BY 1,2;
SELECT a, b FROM rel_rr WHERE a <= func_volatile(5) ORDER BY 1,2;

-- Clean up everything
DROP SEQUENCE seqtest;
DROP SEQUENCE seqtest2;
DROP TABLE rel_rep;
DROP TABLE rel_hash;
DROP TABLE rel_rr;

-- UPDATE cases for replicated table
-- Plain case, change it completely
CREATE TABLE rel_rep (a int, b timestamp DEFAULT NULL, c boolean DEFAULT NULL) DISTRIBUTE BY REPLICATION;
CREATE SEQUENCE seqtest3 START 1;
INSERT INTO rel_rep VALUES (1),(2),(3),(4),(5);
UPDATE rel_rep SET a = nextval('seqtest3'), b = now(), c = false;
SELECT a FROM rel_rep ORDER BY 1;
-- Non-Coordinator quals
UPDATE rel_rep SET b = now(), c = true WHERE a < func_volatile(2);
SELECT a FROM rel_rep WHERE c = true ORDER BY 1;
UPDATE rel_rep SET c = false;
UPDATE rel_rep SET b = now(), c = true WHERE a < func_stable(3);
SELECT a FROM rel_rep WHERE c = true ORDER BY 1;
UPDATE rel_rep SET c = false WHERE c = true;
UPDATE rel_rep SET b = now(), c = true WHERE a < func_immutable(4);
SELECT a FROM rel_rep WHERE c = true ORDER BY 1;
UPDATE rel_rep SET c = false;
-- Coordinator quals
UPDATE rel_rep SET b = now(), c = true WHERE a < currval('seqtest3') - 3 AND b < now();
SELECT a FROM rel_rep  WHERE c = true ORDER BY 1;
DROP SEQUENCE seqtest3;

-- UPDATE cases for round robin table
-- Plain cases change it completely
CREATE TABLE rel_rr (a int, b timestamp DEFAULT NULL, c boolean DEFAULT NULL) DISTRIBUTE BY ROUND ROBIN;
CREATE SEQUENCE seqtest4 START 1;
INSERT INTO rel_rr VALUES (1),(2),(3),(4),(5);
UPDATE rel_rr SET a = nextval('seqtest4'), b = now(), c = false;
SELECT a FROM rel_rr ORDER BY 1;
-- Non-Coordinator quals
UPDATE rel_rr SET b = now(), c = true WHERE a < func_volatile(2);
SELECT a FROM rel_rr WHERE c = true ORDER BY 1;
UPDATE rel_rr SET c = false;
UPDATE rel_rr SET b = now(), c = true WHERE a < func_stable(3);
SELECT a FROM rel_rr WHERE c = true ORDER BY 1;
UPDATE rel_rr SET c = false WHERE c = true;
UPDATE rel_rr SET b = now(), c = true WHERE a < func_immutable(4);
SELECT a FROM rel_rr WHERE c = true ORDER BY 1;
UPDATE rel_rr SET c = false;
-- Coordinator qual
UPDATE rel_rr SET b = now(), c = true WHERE a < currval('seqtest4') - 3 AND b < now();
SELECT a FROM rel_rr WHERE c = true ORDER BY 1;
DROP SEQUENCE seqtest4;

-- UPDATE cases for hash table
-- Hash tables cannot be updated on distribution keys so insert fresh rows
CREATE TABLE rel_hash (a int, b timestamp DEFAULT now(), c boolean DEFAULT false) DISTRIBUTE BY HASH(a);
CREATE SEQUENCE seqtest5 START 1;
INSERT INTO rel_hash VALUES (nextval('seqtest5'));
INSERT INTO rel_hash VALUES (nextval('seqtest5'));
INSERT INTO rel_hash VALUES (nextval('seqtest5'));
INSERT INTO rel_hash VALUES (nextval('seqtest5'));
INSERT INTO rel_hash VALUES (nextval('seqtest5'));
SELECT a FROM rel_hash ORDER BY 1;
UPDATE rel_hash SET b = now(), c = true;
SELECT a FROM rel_hash WHERE c = true ORDER BY 1;
UPDATE rel_hash SET c = false;
-- Non-Coordinator quals
UPDATE rel_hash SET b = now(), c = true WHERE a < func_volatile(2);
SELECT a FROM rel_hash WHERE c = true ORDER BY 1;
UPDATE rel_hash SET c = false;
UPDATE rel_hash SET b = now(), c = true WHERE a < func_stable(3);
SELECT a FROM rel_hash WHERE c = true ORDER BY 1;
UPDATE rel_hash SET c = false;
UPDATE rel_hash SET b = now(), c = true WHERE a < func_immutable(4);
SELECT a FROM rel_hash WHERE c = true ORDER BY 1;
UPDATE rel_hash SET c = false;
-- Coordinator quals
UPDATE rel_hash SET b = now(), c = true WHERE a < currval('seqtest5') - 3 AND b < now();
SELECT a FROM rel_hash WHERE c = true ORDER BY 1;
DROP SEQUENCE seqtest5;

-- DELETE cases
-- Coordinator quals
CREATE SEQUENCE seqtest7 START 1;
DELETE FROM rel_rep WHERE a < nextval('seqtest7') + 1;
DELETE FROM rel_rr WHERE a < nextval('seqtest7') - 3;
DELETE FROM rel_hash WHERE a < nextval('seqtest7') - 3;
-- Plain cases
DELETE FROM rel_rep;
DELETE FROM rel_rr;
DELETE FROM rel_hash;
DROP SEQUENCE seqtest7;

-- Clean up
DROP TABLE rel_rep, rel_hash, rel_rr;
DROP FUNCTION func_stable (int);
DROP FUNCTION func_volatile (int);
DROP FUNCTION func_immutable (int);
