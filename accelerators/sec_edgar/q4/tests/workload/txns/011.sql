-- Doubles at the edges: huge and tiny values, a cancellation that rounds, a negative zero, an exact sum.
INSERT INTO tag (tag, version, custom, abstract, tlabel) VALUES ('EdgeTag', 'vE', 1, 0, 'Edge doubles');
INSERT INTO sub (adsh, cik, name, sic, fy, prevrpt) VALUES
    ('E-1', 7300001, 'Edge 1', 7370, 2024, 0), ('E-2', 7300002, 'Edge 2', 7370, 2024, 0), ('E-3', 7300003, 'Edge 3', 7370, 2024, 0),
    ('E-4', 7300004, 'Edge 4', 7370, 2024, 0), ('E-5', 7300005, 'Edge 5', 7370, 2024, 0);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value) VALUES
    ('E-1', 'EdgeTag', 'vE', 20240101, 4, 'USD', 1.0e16), ('E-2', 'EdgeTag', 'vE', 20240101, 4, 'USD', 1.0),
    ('E-3', 'EdgeTag', 'vE', 20240101, 4, 'USD', -1.0e16), ('E-4', 'EdgeTag', 'vE', 20240101, 4, 'USD', 1.0e-300),
    ('E-5', 'EdgeTag', 'vE', 20240101, 4, 'USD', 4.9e-324), ('E-2', 'EdgeTag', 'vE', 20240101, 0, 'USD', -(0.0::DOUBLE)),
    ('E-1', 'EdgeTag', 'vE', 20240101, 0, 'USD', 1.7e308), ('E-2', 'EdgeTag', 'vE', 20240101, 3, 'USD', -1.7e308);
INSERT INTO pre (adsh, report, line, stmt, tag, version) VALUES
    ('E-1', 1, 1, 'CF', 'EdgeTag', 'vE'), ('E-2', 1, 1, 'CF', 'EdgeTag', 'vE'), ('E-3', 1, 1, 'CF', 'EdgeTag', 'vE'),
    ('E-4', 1, 1, 'CF', 'EdgeTag', 'vE'), ('E-5', 1, 1, 'CF', 'EdgeTag', 'vE');
