-- Kestrel: a small synthetic retail workload. It exists to exercise what TPC-H and SEC-EDGAR do not: text join
-- keys that are not named *key / *id, a table no other table links to, decimals, dates and NULLs.
CREATE TABLE shop (
    shop_code VARCHAR NOT NULL,
    region VARCHAR NOT NULL,
    opened DATE NOT NULL,
    PRIMARY KEY (shop_code)
);
CREATE TABLE item (
    sku VARCHAR NOT NULL,
    family VARCHAR NOT NULL,
    weight DOUBLE,
    PRIMARY KEY (sku)
);
CREATE TABLE sale (
    receipt VARCHAR NOT NULL,
    shop_code VARCHAR NOT NULL REFERENCES shop (shop_code),
    sku VARCHAR NOT NULL REFERENCES item (sku),
    qty INTEGER NOT NULL,
    price DECIMAL(12, 2) NOT NULL,
    sold DATE NOT NULL,
    note VARCHAR
);
CREATE TABLE audit_log (
    entry INTEGER NOT NULL,
    severity SMALLINT NOT NULL,
    message VARCHAR
);
