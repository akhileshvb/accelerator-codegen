UPDATE part SET p_brand = catalog.brand, p_container = catalog.container FROM (VALUES
    (0, 'Brand#23', 'WRAP BOX'),
    (1, 'Brand#25', 'MED DRUM'),
    (2, 'Brand#43', 'LG CAN'),
    (3, 'Brand#11', 'SM BOX'),
    (4, 'Brand#12', 'MED BOX'),
    (5, 'Brand#21', 'LG DRUM'),
    (6, 'Brand#32', 'SM BAG'),
    (7, 'Brand#33', 'LG BOX'),
    (8, 'Brand#42', 'MED DRUM'),
    (9, 'Brand#45', 'LG BOX'),
    (10, 'Brand#51', 'MED DRUM'),
    (11, 'Brand#55', 'WRAP JAR')
) AS catalog(ordinal, brand, container) WHERE part.p_name LIKE 'q17-fixture-%' AND part.p_size <= 8 AND (part.p_size - 1) % 12 = catalog.ordinal;
