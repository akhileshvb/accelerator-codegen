-- Fixtures the test database lacks: NULL join and group columns, repeated rows, a company that is
-- sometimes unknown, tags that must not count (abstract, NULL), values that do not add exactly.
INSERT INTO sub (adsh, cik, name, sic, fy, prevrpt) VALUES
    ('F-A1', 7000001, 'Alpha One', 4011, 2023, 0), ('F-A2', 7000002, 'Alpha Two', 4011, 2023, 0),
    ('F-B1', 7000003, 'Beta One', NULL, 2023, 0), ('F-B2', 7000004, 'Beta Two', NULL, 2023, 0),
    ('F-C1', 7000005, 'Gamma One', 5000, 2023, 0), ('F-C2', 7000006, 'Gamma Two', 5000, 2023, 0),
    ('F-D1', NULL, 'Delta Unknown', 5100, 2023, 0), ('F-D2', 7000007, 'Delta Known', 5100, 2023, 0),
    ('F-D3', NULL, 'Delta Unknown 2', 5150, 2023, 0), ('F-D4', 7000008, 'Delta Known 2', 5150, 2023, 0),
    ('F-D5', 7000009, 'Delta Known 3', 5150, 2023, 0),
    ('F-E1', 7000010, 'Epsilon One', 5200, 2023, 0), ('F-E2', 7000011, 'Epsilon Two', 5200, 2023, 0),
    ('F-H1', 7000012, 'Eta One', 5300, 2023, 0), ('F-H2', 7000013, 'Eta Two', 5300, 2023, 0),
    ('F-H3', 7000014, 'Eta Three', 5300, 2023, 0);
INSERT INTO tag (tag, version, custom, abstract, tlabel) VALUES
    ('NullLabel', 'vF', 1, 0, NULL), ('NullSic', 'vF', 1, 0, 'No SIC'), ('DupTag', 'vF', 1, 0, 'Dup label'),
    ('UnknownCik', 'vF', 1, 0, 'Unknown cik'), ('AbstractTag', 'vF', 1, 1, 'Abstract'), ('NullAbstract', 'vF', 1, NULL, 'Null abstract'),
    ('Rounding', 'vF', 1, 0, 'Rounding'), ('StmtOther', 'vF', 1, 0, 'Other statement');
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value) VALUES
    ('F-A1', 'NullLabel', 'vF', 20230101, 4, 'USD', 10.5), ('F-A2', 'NullLabel', 'vF', 20230101, 4, 'USD', 4.25),
    ('F-B1', 'NullSic', 'vF', 20230101, 4, 'USD', 1.0), ('F-B2', 'NullSic', 'vF', 20230101, 4, 'USD', 2.0),
    ('F-C1', 'DupTag', 'vF', 20230101, 4, 'USD', 3.25), ('F-C1', 'DupTag', 'vF', 20230101, 4, 'USD', 3.25),
    ('F-C2', 'DupTag', 'vF', 20230101, 4, 'USD', 3.25), ('F-C2', 'DupTag', 'vF', 20230101, 4, 'USD', 3.25),
    ('F-D1', 'UnknownCik', 'vF', 20230101, 4, 'USD', 5.0), ('F-D2', 'UnknownCik', 'vF', 20230101, 4, 'USD', 6.0),
    ('F-D3', 'UnknownCik', 'vF', 20230101, 4, 'USD', 7.0), ('F-D4', 'UnknownCik', 'vF', 20230101, 4, 'USD', 8.0),
    ('F-D5', 'UnknownCik', 'vF', 20230101, 4, 'USD', 9.0),
    ('F-E1', 'AbstractTag', 'vF', 20230101, 4, 'USD', 1.0), ('F-E2', 'AbstractTag', 'vF', 20230101, 4, 'USD', 1.0),
    ('F-E1', 'NullAbstract', 'vF', 20230101, 4, 'USD', 1.0), ('F-E2', 'NullAbstract', 'vF', 20230101, 4, 'USD', 1.0),
    ('F-E1', 'StmtOther', 'vF', 20230101, 4, 'USD', NULL), ('F-E2', 'StmtOther', 'vF', 20230101, 4, 'USD', 2.0),
    ('F-H1', 'Rounding', 'vF', 20230101, 4, 'USD', 0.1), ('F-H2', 'Rounding', 'vF', 20230101, 4, 'USD', 0.2),
    ('F-H3', 'Rounding', 'vF', 20230101, 4, 'USD', 0.3);
INSERT INTO pre (adsh, report, line, stmt, tag, version) VALUES
    ('F-A1', 1, 1, 'BS', 'NullLabel', 'vF'), ('F-A2', 1, 1, 'BS', 'NullLabel', 'vF'),
    ('F-B1', 1, 1, 'BS', 'NullSic', 'vF'), ('F-B2', 1, 1, 'BS', 'NullSic', 'vF'),
    ('F-C1', 1, 1, 'BS', 'DupTag', 'vF'), ('F-C1', 1, 2, 'BS', 'DupTag', 'vF'), ('F-C1', 1, 3, 'BS', 'DupTag', 'vF'),
    ('F-C2', 1, 1, 'BS', 'DupTag', 'vF'),
    ('F-D1', 1, 1, 'BS', 'UnknownCik', 'vF'), ('F-D2', 1, 1, 'BS', 'UnknownCik', 'vF'),
    ('F-D3', 1, 1, 'BS', 'UnknownCik', 'vF'), ('F-D4', 1, 1, 'BS', 'UnknownCik', 'vF'), ('F-D5', 1, 1, 'BS', 'UnknownCik', 'vF'),
    ('F-E1', 1, 1, 'BS', 'AbstractTag', 'vF'), ('F-E2', 1, 1, 'BS', 'AbstractTag', 'vF'),
    ('F-E1', 1, 2, 'BS', 'NullAbstract', 'vF'), ('F-E2', 1, 2, 'BS', 'NullAbstract', 'vF'),
    ('F-E1', 1, 3, 'UN', 'StmtOther', 'vF'), ('F-E2', 1, 3, 'UN', 'StmtOther', 'vF'),
    ('F-H1', 1, 1, 'BS', 'Rounding', 'vF'), ('F-H2', 1, 1, 'BS', 'Rounding', 'vF'), ('F-H3', 1, 1, 'BS', 'Rounding', 'vF');
