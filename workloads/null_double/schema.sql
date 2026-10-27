-- The SEC EDGAR `num` table (all columns NULL-able; `value` is DOUBLE) without its foreign key.
CREATE TABLE num (
    adsh VARCHAR(20),
    tag VARCHAR(256),
    version VARCHAR(20),
    ddate INTEGER,
    qtrs INTEGER,
    uom VARCHAR(20),
    coreg VARCHAR(256),
    value DOUBLE,
    footnote VARCHAR(1024)
);
