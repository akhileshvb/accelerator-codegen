-- A steady filing stream above the stored prefix: 1500 new filings of fiscal year 2022 with USD values larger than any stored, so the class outgrows its stored prefix and is trimmed.
INSERT INTO sub (adsh, cik, name, sic, form, period, fy, fp, filed, prevrpt)
SELECT '00099' || lpad(i::varchar, 5, '0') || '-24-000001', 990000 + i, 'Stream Filer ' || i, 1000, '10-K', 20221231, 2022, 'FY', 20230301, 0 FROM range(0, 1500) t(i);
INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value)
SELECT '00099' || lpad(i::varchar, 5, '0') || '-24-000001', 'StreamTag', 'us-gaap/2023', 20221231, 4, 'USD', 1.0e29 + i * 1.0e24 FROM range(0, 1500) t(i);
