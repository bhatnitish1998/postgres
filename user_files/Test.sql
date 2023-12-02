-- Test 1
drop table t;
create table t (a int primary key , b int);
insert into t values(1,30);
insert into t values(2,30);
insert into t values(3,30);
insert into t values(4,30);
insert into t values(5,30);
insert into t values(6,30);
insert into t values(7,30);
insert into t values(8,30);
insert into t values(9,30);
insert into t values(10,30);
insert into t values(11,30);
insert into t values(12,30);
insert into t values(13,30);
insert into t values(14,30);
insert into t values(15,30);
insert into t values(16,30);
insert into t values(17,30);
insert into t values(18,30);
insert into t values(19,30);
insert into t values(20,30);



drop table t;
create table t (a int , b int);
insert into t values(1,30);
insert into t values(2,30);
create index i1 on t (a);
insert into t values(3,30);