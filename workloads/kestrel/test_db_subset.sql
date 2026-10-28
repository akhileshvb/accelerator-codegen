-- The small test database: every shop and item, a tenth of the sales, the first audit entries.
CREATE TABLE shop AS SELECT * FROM src.shop;
CREATE TABLE item AS SELECT * FROM src.item;
CREATE TABLE sale AS SELECT * FROM src.sale WHERE hash(receipt) % 10 = 0;
CREATE TABLE audit_log AS SELECT * FROM src.audit_log ORDER BY entry LIMIT 1000;
