-- SEC EDGAR schema for PostgreSQL test databases (no foreign keys).

CREATE TABLE sub (
    adsh VARCHAR(64) PRIMARY KEY,
    cik INTEGER,
    name VARCHAR(150),
    sic INTEGER,
    countryba VARCHAR(2),
    stprba VARCHAR(2),
    cityba VARCHAR(30),
    countryinc VARCHAR(3),
    form VARCHAR(10),
    period INTEGER,
    fy INTEGER,
    fp VARCHAR(2),
    filed INTEGER,
    accepted VARCHAR(24),
    prevrpt INTEGER,
    nciks INTEGER,
    afs VARCHAR(5),
    wksi INTEGER,
    fye VARCHAR(4),
    instance VARCHAR(64)
);

CREATE TABLE num (
    adsh VARCHAR(64),
    tag VARCHAR(256),
    version VARCHAR(20),
    ddate INTEGER,
    qtrs INTEGER,
    uom VARCHAR(20),
    coreg VARCHAR(256),
    value DOUBLE PRECISION,
    footnote VARCHAR(1024)
);

CREATE TABLE tag (
    tag VARCHAR(256),
    version VARCHAR(20),
    custom INTEGER,
    abstract INTEGER,
    datatype VARCHAR(20),
    iord VARCHAR(1),
    crdr VARCHAR(1),
    tlabel VARCHAR(512),
    doc TEXT,
    PRIMARY KEY (tag, version)
);

CREATE TABLE pre (
    adsh VARCHAR(64),
    report INTEGER,
    line INTEGER,
    stmt VARCHAR(2),
    inpth INTEGER,
    rfile VARCHAR(1),
    tag VARCHAR(256),
    version VARCHAR(20),
    plabel VARCHAR(1024),
    negating INTEGER
);
