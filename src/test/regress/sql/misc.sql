--
-- MISC
--

--
-- BTREE
--
UPDATE onek
   SET unique1 = onek.unique1 + 1;

UPDATE onek
   SET unique1 = onek.unique1 - 1;

--
-- BTREE partial
--
-- UPDATE onek2
--   SET unique1 = onek2.unique1 + 1;

--UPDATE onek2 
--   SET unique1 = onek2.unique1 - 1;

--
-- BTREE shutting out non-functional updates
--
-- the following two tests seem to take a long time on some 
-- systems.    This non-func update stuff needs to be examined
-- more closely.  			- jolly (2/22/96)
-- 
UPDATE tmp
   SET stringu1 = reverse_name(onek.stringu1)
   FROM onek
   WHERE onek.stringu1 = 'JBAAAA' and
	  onek.stringu1 = tmp.stringu1;

UPDATE tmp
   SET stringu1 = reverse_name(onek2.stringu1)
   FROM onek2
   WHERE onek2.stringu1 = 'JCAAAA' and
	  onek2.stringu1 = tmp.stringu1;

DROP TABLE tmp;

--UPDATE person*
--   SET age = age + 1;

--UPDATE person*
--   SET age = age + 3
--   WHERE name = 'linda';

--
-- copy
--
COPY onek TO '/Users/masonsharp/dev/pgxc/postgres-xc/src/test/regress/results/onek.data';

DELETE FROM onek;

COPY onek FROM '/Users/masonsharp/dev/pgxc/postgres-xc/src/test/regress/results/onek.data';

SELECT unique1 FROM onek WHERE unique1 < 2 ORDER BY unique1;

DELETE FROM onek2;

COPY onek2 FROM '/Users/masonsharp/dev/pgxc/postgres-xc/src/test/regress/results/onek.data';

SELECT unique1 FROM onek2 WHERE unique1 < 2 ORDER BY unique1;

COPY BINARY stud_emp TO '/Users/masonsharp/dev/pgxc/postgres-xc/src/test/regress/results/stud_emp.data';

DELETE FROM stud_emp;

COPY BINARY stud_emp FROM '/Users/masonsharp/dev/pgxc/postgres-xc/src/test/regress/results/stud_emp.data';

SELECT * FROM stud_emp ORDER BY 1,2;

-- COPY aggtest FROM stdin;
-- 56	7.8
-- 100	99.097
-- 0	0.09561
-- 42	324.78
-- .
-- COPY aggtest TO stdout;


--
-- inheritance stress test
--
SELECT * FROM a_star* ORDER BY 1,2;

SELECT * 
   FROM b_star* x
   WHERE x.b = text 'bumble' or x.a < 3;

SELECT class, a 
   FROM c_star* x 
   WHERE x.c ~ text 'hi' ORDER BY 1,2;

SELECT class, b, c
   FROM d_star* x
   WHERE x.a < 100 ORDER BY 1,2,3;

SELECT class, c FROM e_star* x WHERE x.c NOTNULL ORDER BY 1,2;

SELECT * FROM f_star* x WHERE x.c ISNULL ORDER BY 1,2;

-- grouping and aggregation on inherited sets have been busted in the past...

SELECT sum(a) FROM a_star*;

SELECT class, sum(a) FROM a_star* GROUP BY class ORDER BY class;


ALTER TABLE f_star RENAME COLUMN f TO ff;

ALTER TABLE e_star* RENAME COLUMN e TO ee;

ALTER TABLE d_star* RENAME COLUMN d TO dd;

ALTER TABLE c_star* RENAME COLUMN c TO cc;

ALTER TABLE b_star* RENAME COLUMN b TO bb;

ALTER TABLE a_star* RENAME COLUMN a TO aa;

SELECT class, aa
   FROM a_star* x
   WHERE aa ISNULL ORDER BY 1,2;

-- As of Postgres 7.1, ALTER implicitly recurses,
-- so this should be same as ALTER a_star*

ALTER TABLE a_star RENAME COLUMN aa TO foo;

SELECT class, foo
   FROM a_star* x
   WHERE x.foo >= 2 ORDER BY 1,2;

ALTER TABLE a_star RENAME COLUMN foo TO aa;

SELECT * 
   from a_star*
   WHERE aa < 1000 ORDER BY 1,2;

ALTER TABLE f_star ADD COLUMN f int4;

UPDATE f_star SET f = 10;

ALTER TABLE e_star* ADD COLUMN e int4;

--UPDATE e_star* SET e = 42;

SELECT * FROM e_star* ORDER BY 1,2;

ALTER TABLE a_star* ADD COLUMN a text;

--UPDATE b_star*
--   SET a = text 'gazpacho'
--   WHERE aa > 4;

SELECT class, aa, a FROM a_star* ORDER BY 1,2;


--
-- versions
--

--
-- postquel functions
--
--
-- mike does post_hacking,
-- joe and sally play basketball, and
-- everyone else does nothing.
--
SELECT p.name, name(p.hobbies) FROM ONLY person p ORDER BY 1,2;

--
-- as above, but jeff also does post_hacking.
--
SELECT p.name, name(p.hobbies) FROM person* p ORDER BY 1,2;

--
-- the next two queries demonstrate how functions generate bogus duplicates.
-- this is a "feature" ..
--
SELECT DISTINCT hobbies_r.name, name(hobbies_r.equipment) FROM hobbies_r
  ORDER BY 1,2;

SELECT hobbies_r.name, (hobbies_r.equipment).name FROM hobbies_r ORDER BY 1,2;

--
-- mike needs advil and peet's coffee,
-- joe and sally need hightops, and
-- everyone else is fine.
--
SELECT p.name, name(p.hobbies), name(equipment(p.hobbies)) FROM ONLY person p ORDER BY 1,2,3;

--
-- as above, but jeff needs advil and peet's coffee as well.
--
SELECT p.name, name(p.hobbies), name(equipment(p.hobbies)) FROM person* p ORDER BY 1,2,3;

--
-- just like the last two, but make sure that the target list fixup and
-- unflattening is being done correctly.
--
SELECT name(equipment(p.hobbies)), p.name, name(p.hobbies) FROM ONLY person p ORDER BY 1,2,3;

SELECT (p.hobbies).equipment.name, p.name, name(p.hobbies) FROM person* p ORDER BY 1,2,3;

SELECT (p.hobbies).equipment.name, name(p.hobbies), p.name FROM ONLY person p ORDER BY 1,2,3;

SELECT name(equipment(p.hobbies)), name(p.hobbies), p.name FROM person* p ORDER BY 1,2,3;

SELECT user_relns() AS user_relns
   ORDER BY user_relns;

SELECT name(equipment(hobby_construct(text 'skywalking', text 'mer')));

SELECT hobbies_by_name('basketball');

SELECT name, overpaid(emp.*) FROM emp ORDER BY 1,2;

--
-- Try a few cases with SQL-spec row constructor expressions
--
SELECT * FROM equipment(ROW('skywalking', 'mer'));

SELECT name(equipment(ROW('skywalking', 'mer')));

SELECT *, name(equipment(h.*)) FROM hobbies_r h ORDER BY 1,2,3;

SELECT *, (equipment(CAST((h.*) AS hobbies_r))).name FROM hobbies_r h ORDER BY 1,2,3;

--
-- check that old-style C functions work properly with TOASTed values
--
create table oldstyle_test(i int4, t text);
insert into oldstyle_test values(null,null);
insert into oldstyle_test values(0,'12');
insert into oldstyle_test values(1000,'12');
insert into oldstyle_test values(0, repeat('x', 50000));

select i, length(t), octet_length(t), oldstyle_length(i,t) from oldstyle_test ORDER BY 1,2,3;

drop table oldstyle_test;

--
-- functional joins
--

--
-- instance rules
--

--
-- rewrite rules
--

