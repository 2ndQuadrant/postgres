# MERGE INSERT UPDATE
#
# This test tries to expose problems between concurrent sessions

setup
{
  CREATE TABLE target (key int primary key, val text);
  INSERT INTO target VALUES (1, 'setup');
}

teardown
{
  DROP TABLE target;
}

session "s1"
setup
{
  BEGIN ISOLATION LEVEL READ COMMITTED;
}
step "merge1" { MERGE INTO target t USING (SELECT 1 as key, 'merge1' as val) s ON s.key = t.key WHEN NOT MATCHED THEN INSERT VALUES (s.key, s.val) WHEN MATCHED THEN UPDATE set val = t.val || ' updated by merge1'; }
step "delete1" { DELETE FROM target WHERE key = 1; }
step "insert1" { INSERT INTO target VALUES (1, 'insert1'); }
step "c1" { COMMIT; }
step "a1" { ABORT; }

session "s2"
setup
{
  BEGIN ISOLATION LEVEL READ COMMITTED;
}
step "merge2" { MERGE INTO target t USING (SELECT 1 as key, 'merge2' as val) s ON s.key = t.key WHEN NOT MATCHED THEN INSERT VALUES (s.key, s.val) WHEN MATCHED THEN UPDATE set val = t.val || ' updated by merge2'; }

step "merge2i" { MERGE INTO target t USING (SELECT 1 as key, 'merge2' as val) s ON s.key = t.key WHEN MATCHED THEN UPDATE set val = t.val || ' updated by merge2'; }

step "select2" { SELECT * FROM target; }
step "c2" { COMMIT; }
step "a2" { ABORT; }

# Basic effects
permutation "merge1" "c1" "select2" "c2"
permutation "merge1" "c1" "merge2" "select2" "c2"

# check concurrent updates, unconditional
permutation "merge1" "merge2" "c1" "select2" "c2"
permutation "merge1" "merge2" "a1" "select2" "c2"

# check how we handle when visible row has been concurrently deleted
permutation "delete1" "c1" "merge2" "select2" "c2"
permutation "delete1" "merge2" "c1" "select2" "c2"
permutation "delete1" "merge2i" "c1" "select2" "c2"

# check how we handle when visible row has been concurrently deleted, then same key re-inserted
permutation "delete1" "insert1" "c1" "merge2" "select2" "c2"
permutation "delete1" "insert1" "merge2" "c1" "select2" "c2"
permutation "delete1" "insert1" "merge2i" "c1" "select2" "c2"
