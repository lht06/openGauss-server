-- check compatibility --
show sql_compatibility; -- expect A --

DROP SCHEMA IF EXISTS plpgsql_nested_array_and_record CASCADE;
CREATE SCHEMA plpgsql_nested_array_and_record;
SET current_schema = plpgsql_nested_array_and_record;

-- array of arrays
CREATE OR REPLACE PROCEDURE test_nested AS
DECLARE
    TYPE arr2 IS VARRAY(5) OF INTEGER;
    TYPE arr1 IS VARRAY(5) OF INTEGER;
    TYPE nt1 IS VARRAY(10) OF arr1;
    TYPE rec1 IS RECORD(id int, arrarg nt1);
    arr_rec rec1:=rec1(7, nt1(arr1(1,2,4,5),arr1(1,3)));
BEGIN
    RAISE NOTICE 'ID: %', arr_rec.id;
END;
/

CREATE OR REPLACE PROCEDURE test_nested AS
DECLARE
    TYPE arr2 IS TABLE OF INTEGER;
    TYPE arr1 IS TABLE OF INTEGER;
    TYPE nt1 IS TABLE OF arr1;
    TYPE rec1 IS RECORD(id int, arrarg nt1);
    arr_rec rec1:=rec1(7, nt1(arr1(1,2,4,5),arr1(1,3)));
BEGIN
    RAISE NOTICE 'ID: %', arr_rec.id;
END;
/

DECLARE
    TYPE arr1 IS VARRAY(5) OF INTEGER;
    TYPE arr2 IS VARRAY(5) OF arr1;
    nst_arr arr2;
BEGIN
    FOR I IN 1..5 LOOP
        nst_arr(1)(I) := I;
        RAISE NOTICE 'RESULT: %', nst_arr(1)(I);
    END LOOP;
END;
/

CREATE OR REPLACE PACKAGE package13 is
TYPE age_rec IS RECORD (years INTEGER DEFAULT 35, months INTEGER DEFAULT 6);
TYPE name_rec_src IS RECORD (age age_rec, first varchar DEFAULT 'John');
END package13;
/

declare
name1 package13.name_rec_src;
begin
    raise info 'first %', name1.first;
    raise info 'last %', name1.age.years;
END;
/

CREATE OR REPLACE PACKAGE package13 is
TYPE age_rec IS RECORD (years INTEGER DEFAULT 35, months INTEGER DEFAULT 6);
TYPE name_rec_src IS RECORD (first varchar DEFAULT 'John',age age_rec);
END package13;
/


declare
name1 package13.name_rec_src;
begin
    raise info 'first %', name1.first;
    raise info 'last %', name1.age.years;
END;
/
drop PACKAGE package13;

DECLARE									
TYPE t1 IS VARRAY(10) OF INTEGER;  -- varray of integer
va t1 := t1(2,3);

TYPE nt1 IS VARRAY(10) OF t1;      -- varray of varray of integer
nva nt1 := nt1(t1(2,3,5), t1(55,6), t1(2,3,8));

i INTEGER;
va1 t1;
BEGIN
  raise notice '%', nva(2)(3);
END;
/

DECLARE
TYPE t1 IS VARRAY(10) OF INTEGER;  -- varray of integer
va t1 := t1(2,3);

TYPE nt1 IS VARRAY(10) OF t1;      -- varray of varray of integer
nva nt1 := nt1(t1(2,3,5), t1(55,8,6), t1(2,3,8));

i INTEGER;
va1 t1;
BEGIN
  raise notice '%', nva(2)(1);
END;
/

DECLARE									
TYPE t1 IS VARRAY(10) OF INTEGER;  -- varray of integer
va t1 := t1(2,3,9);

TYPE nt1 IS VARRAY(10) OF t1;      -- varray of varray of integer
nva nt1 := nt1(va, t1(55,8,6), t1(2,3,8));

i INTEGER;
va1 t1;
BEGIN
  raise notice '%', nva(1)(3);
END;
/

DECLARE									
TYPE t1 IS VARRAY(10) OF INTEGER;  -- varray of integer
va t1 := t1(2,3,9);
TYPE nt1 IS VARRAY(10) OF t1;      -- varray of varray of integer
TYPE nnt1 IS VARRAY(10) OF nt1;
nva nnt1 := nt1(nt1(t1(2,3,9), t1(55,8,6), t1(2,3,8)),nt1(t1(95,80,65), t1(2,3,9), t1(2,3,8)));

i INTEGER;
va1 t1;
BEGIN
  raise notice '%', nva(2)(1)(3);
END;
/

DECLARE									
TYPE t1 IS VARRAY(10) OF INTEGER;  -- varray of integer
va t1 := t1(2,3,9);
TYPE nt1 IS VARRAY(10) OF t1;      -- varray of varray of integer
TYPE nnt1 IS VARRAY(10) OF nt1;
nva nnt1 := nt1(nt1(t1(2,3,9), va, t1(2,3,8)),nt1(t1(95,80,65), t1(2,3,9), t1(2,3,8)));

i INTEGER;
va1 t1;
BEGIN
  raise notice '%', nva(1)(2)(2);
END;
/

DECLARE									
TYPE t1 IS VARRAY(10) OF INTEGER;  -- varray of integer
va t1 := t1(2,3,9);
TYPE nt1 IS VARRAY(10) OF t1;      -- varray of varray of integer
TYPE nnt1 IS VARRAY(10) OF nt1;
nva nnt1 := nt1(nt1(t1(2,3,9), va, t1(2,3,8)),nt1(va, t1(2,3,9), t1(2,3,8)));

i INTEGER;
va1 t1;
BEGIN
  raise notice '%', nva(2)(1)(3);
END;
/

CREATE OR REPLACE PROCEDURE test_nested_array as
TYPE typ_PLArray_case0001 IS varray(3) OF integer;
TYPE typ_PLArray_case0002 IS varray(3) OF typ_PLArray_case0001;
nstarr typ_PLArray_case0002;
BEGIN
        nstarr(1):=1;
        RAISE NOTICE '二维数组(1)：%', nstarr(1);
END;
/
CALL test_nested_array();

CREATE OR REPLACE PROCEDURE test_nested_array as
TYPE typ_PLArray_case0001 IS varray(3) OF integer;
TYPE typ_PLArray_case0002 IS varray(3) OF typ_PLArray_case0001;
nstarr typ_PLArray_case0002;
arr typ_PLArray_case0001;
BEGIN
        arr(1):=1;
        nstarr(1):=arr;
        RAISE NOTICE '二维数组(1)：%', nstarr(1);
END;
/
CALL test_nested_array();

-- record of arrays
DECLARE
    TYPE arr1 IS VARRAY(5) OF INTEGER;
    TYPE rec1 IS RECORD(id int, arrarg arr1);
    arr_rec rec1;
BEGIN
    FOR I IN 1..5 LOOP
        arr_rec.arrarg(I):=I;
        RAISE NOTICE 'RESULT: %', arr_rec.arrarg(I);
    END LOOP;    
END;
/

-- array of records
CREATE OR REPLACE PROCEDURE test_nested AS
DECLARE
    TYPE rec1 IS RECORD(id int, name char(10));
    TYPE arr1 IS VARRAY(5) OF rec1;
    rec_arr arr1;
BEGIN
    FOR I IN 1..5 LOOP
        rec_arr(I).id := I;
        RAISE NOTICE 'RESULT: %', rec_arr(I).id;
    END LOOP;
END;
/
CALL test_nested();

-- record of records
CREATE OR REPLACE PROCEDURE test_nested AS
DECLARE
    TYPE rec1 IS RECORD(id int, name char(10));
    TYPE rec2 IS RECORD(id int, recarg rec1);
    recrec rec2;
BEGIN
    recrec.recarg.id := 1;
    recrec.recarg.name := 'RECORD';
    RAISE NOTICE 'ID: %, NAME: %', recrec.recarg.id, recrec.recarg.name;
END;
/
CALL test_nested();

set behavior_compat_options='plpgsql_dependency';

create or replace package pac_PLArray_Case0021 is
  type typ_PLArray_1 is table of varchar(100);
  type typ_PLArray_2 is table of typ_PLArray_1;
  nstarr typ_PLArray_2;

  procedure p_PLArray_1;
  procedure p_PLArray_2(var typ_PLArray_2);
end pac_PLArray_Case0021;
/

create or replace package body pac_PLArray_Case0021 is
procedure p_PLArray_1() is
begin
nstarr(2)(1):='第二行第一列';
perform p_PLArray_2(nstarr);
end;

procedure p_PLArray_2(var typ_PLArray_2) is
begin
    insert into t_PLArray_case0021(col) values(var(2)(1));
end;
end pac_PLArray_Case0021;
/

create or replace package pac_PLArray_Case0021 is
  procedure p_PLArray_1;
  procedure p_PLArray_2(var typ_PLArray_3);
end pac_PLArray_Case0021;
/

create or replace package body pac_PLArray_Case0021 is
procedure p_PLArray_1() is
begin
nstarr(2)(1):='第二行第一列';
perform p_PLArray_2(nstarr);
end;

procedure p_PLArray_2(var typ_PLArray_3) is
begin
    insert into t_PLArray_case0021(col) values(var(2)(1));
end;
end pac_PLArray_Case0021;
/

declare
    type typ_PLArray_1 is varray(3) of varchar(50);
    type typ_PLArray_2 is varray(3) of typ_PLArray_1;
    nstarr1 typ_PLArray_2;
    nstarr2 typ_PLArray_2;
begin
    nstarr1(1)(1):='第一行第一列';
    nstarr2:=nstarr1;
    raise notice '二维数组nstarr(1)(1): %',nstarr2(1)(1);
end;
/

drop package pac_PLArray_Case0021;

CREATE OR REPLACE PACKAGE package11 is
TYPE age_rec IS RECORD (years INTEGER DEFAULT 35, months INTEGER DEFAULT 6);
TYPE name_rec_src IS RECORD (first varchar DEFAULT 'John',age age_rec);
END package11;
/

declare
name1 package11.name_rec_src;
begin
    raise info 'first %', name1.first;
    raise info 'last %', name1.age.years;
END;
/

drop PACKAGE package11;


DROP SCHEMA plpgsql_nested_array_and_record CASCADE;
