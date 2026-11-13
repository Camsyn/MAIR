-- Independent CRUD and savepoint exercise; all state stays in memory.
CREATE TABLE items(id INTEGER PRIMARY KEY, name TEXT UNIQUE, qty INTEGER CHECK(qty>=0), payload BLOB, note TEXT);
BEGIN;
INSERT INTO items VALUES(1,'alpha',3,x'007f80ff',NULL),(2,'beta',7,x'010203','ready'),(3,'gamma',0,x'',NULL);
UPDATE items SET qty=qty+2, note='updated' WHERE id=1;
SAVEPOINT trial;
DELETE FROM items WHERE id=2;
INSERT INTO items VALUES(4,'temporary',1,NULL,NULL);
ROLLBACK TO trial;
RELEASE trial;
DELETE FROM items WHERE id=3;
COMMIT;
SELECT id,name,qty,hex(payload),coalesce(note,'missing') FROM items ORDER BY id;
SELECT count(*),sum(qty),length(group_concat(name,',')) FROM items;
