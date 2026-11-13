-- Exercise triggers, UPSERT conflict handling, and an indexed read.
CREATE TABLE inventory(sku TEXT PRIMARY KEY, qty INTEGER NOT NULL CHECK(qty>=0));
CREATE TABLE changes(sku TEXT, old_qty INTEGER, new_qty INTEGER);
CREATE TRIGGER inventory_change AFTER UPDATE OF qty ON inventory
BEGIN
  INSERT INTO changes VALUES(old.sku,old.qty,new.qty);
END;
INSERT INTO inventory VALUES('bolt',4),('nut',8),('washer',0);
INSERT INTO inventory VALUES('bolt',3),('screw',5)
ON CONFLICT(sku) DO UPDATE SET qty=inventory.qty+excluded.qty;
CREATE INDEX inventory_positive ON inventory(qty) WHERE qty>0;
UPDATE inventory SET qty=qty-2 WHERE sku='nut';
SELECT sku,qty FROM inventory WHERE qty>0 ORDER BY qty,sku;
SELECT sku,old_qty,new_qty FROM changes ORDER BY rowid;
SELECT count(*),sum(qty) FROM inventory;
