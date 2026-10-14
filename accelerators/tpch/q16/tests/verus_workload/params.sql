'Brand#11', 'SMALL BRUSHED%', 1, 2, 3, 4, 5, 6, 7, 8
'Brand#12', 'SMALL BRUSHED%', 9, 10, 11, 12, 13, 14, 15, 16
'Brand#13', 'SMALL BRUSHED%', 17, 18, 19, 20, 21, 22, 23, 24
'Brand#14', 'SMALL BRUSHED%', 25, 26, 27, 28, 29, 30, 31, 32
'Brand#15', 'SMALL BRUSHED%', 33, 34, 35, 36, 37, 38, 39, 40
'Brand#21', 'SMALL BRUSHED%', 41, 42, 43, 44, 45, 46, 47, 48
'Brand#22', 'SMALL BRUSHED%', 43, 44, 45, 46, 47, 48, 49, 50
'Brand#23', 'SMALL BRUSHED%', 3, 9, 14, 19, 23, 36, 45, 49
'Brand#24', 'SMALL BRUSHED%', 1, 7, 13, 19, 25, 31, 43, 50
'Brand#25', 'SMALL BRUSHED%', 5, 12, 18, 23, 26, 32, 38, 48
'Brand#31', 'SMALL BRUSHED%', 1, 2, 3, 4, 5, 6, 7, 8
'Brand#32', 'SMALL BRUSHED%', 9, 10, 11, 12, 13, 14, 15, 16
'Brand#33', 'SMALL BRUSHED%', 17, 18, 19, 20, 21, 22, 23, 24
'Brand#34', 'SMALL BRUSHED%', 25, 26, 27, 28, 29, 30, 31, 32
'Brand#35', 'SMALL BRUSHED%', 33, 34, 35, 36, 37, 38, 39, 40
'Brand#41', 'SMALL BRUSHED%', 41, 42, 43, 44, 45, 46, 47, 48
'Brand#42', 'SMALL BRUSHED%', 43, 44, 45, 46, 47, 48, 49, 50
'Brand#43', 'SMALL BRUSHED%', 3, 9, 14, 19, 23, 36, 45, 49
'Brand#44', 'SMALL BRUSHED%', 1, 7, 13, 19, 25, 31, 43, 50
'Brand#45', 'SMALL BRUSHED%', 3, 9, 14, 19, 23, 36, 45, 49
'Brand#51', 'SMALL BRUSHED%', 1, 2, 3, 4, 5, 6, 7, 8
'Brand#52', 'SMALL BRUSHED%', 9, 10, 11, 12, 13, 14, 15, 16
'Brand#53', 'SMALL BRUSHED%', 17, 18, 19, 20, 21, 22, 23, 24
'Brand#54', 'SMALL BRUSHED%', 25, 26, 27, 28, 29, 30, 31, 32
'Brand#55', 'SMALL BRUSHED%', 43, 44, 45, 46, 47, 48, 49, 50
'Brand#45', 'SMALL BRUSHED%', 1, 1, 1, 1, 1, 1, 1, 1
'Brand#45', 'SMALL BRUSHED%', 50, 50, 50, 50, 50, 50, 50, 50
'Brand#45', 'SMALL BRUSHED%', 48, 48, 48, 48, 48, 48, 48, 48
'Brand#45', 'SMALL BRUSHED%', 18, 18, 18, 18, 18, 18, 18, 18
'Brand#12', 'SMALL BRUSHED%', 5, 12, 18, 23, 26, 32, 38, 48
'Brand#99', 'SMALL BRUSHED%', 3, 9, 14, 19, 23, 36, 45, 49
'', 'SMALL BRUSHED%', 1, 3, 9, 14, 19, 23, 49, 50
'brand#45', 'SMALL BRUSHED%', 3, 9, 14, 19, 23, 36, 45, 49
'Brand#45 ', 'SMALL BRUSHED%', 3, 9, 14, 19, 23, 36, 45, 49
' Brand#45', 'SMALL BRUSHED%', 3, 9, 14, 19, 23, 36, 45, 49
'Bränd#45', 'SMALL BRUSHED%', 3, 9, 14, 19, 23, 36, 45, 49
'Brand#45', 'SMALL BRUSHED%', 0, 51, -1, 100, 0, 51, -1, 100
'Brand#45', 'SMALL BRUSHED%', -2147483648, 2147483647, 0, 51, -2147483648, 2147483647, 0, 51
'Brand#45', 'SMALL BRUSHED%', 49, 45, 36, 23, 19, 14, 9, 3
'Brand#45', 'SMALL BRUSHED%', 14, 14, 14, 14, 14, 14, 14, 14
-- edge cases of a parameter that was a literal when the tests were written (scripts/gen_verus_kit_params.py)
-- $2 was the literal 'SMALL BRUSHED%'; its edge values, the other parameters as in the first tuple
'Brand#11', 'MEDIUM POLISHED%', 1, 2, 3, 4, 5, 6, 7, 8
'Brand#11', 'PROMO%', 1, 2, 3, 4, 5, 6, 7, 8
'Brand#11', '%', 1, 2, 3, 4, 5, 6, 7, 8
'Brand#11', '', 1, 2, 3, 4, 5, 6, 7, 8
'Brand#11', 'small brushed%', 1, 2, 3, 4, 5, 6, 7, 8
'Brand#11', '%BRUSHED%', 1, 2, 3, 4, 5, 6, 7, 8
'Brand#11', 'STANDARD PLATED TIN', 1, 2, 3, 4, 5, 6, 7, 8
-- random parameter sets drawn from the domain when the tests were frozen
-- 8 sets, seed 20261014 (eval/compare/gen_tpch_params.py 8 20261014; regenerate: scripts/gen_verus_kit_params.py)
'Brand#32', 'ECONOMY BURNISHED%', 11, 15, 34, 4, 33, 38, 7, 5
'Brand#53', 'LARGE BRUSHED%', 23, 31, 26, 22, 32, 4, 44, 45
'Brand#15', 'PROMO POLISHED%', 14, 12, 46, 22, 43, 47, 48, 28
'Brand#51', 'MEDIUM BURNISHED%', 11, 16, 3, 2, 25, 43, 30, 29
'Brand#25', 'STANDARD BRUSHED%', 24, 11, 13, 9, 44, 46, 38, 25
'Brand#21', 'STANDARD POLISHED%', 35, 16, 12, 48, 47, 18, 30, 5
'Brand#24', 'LARGE PLATED%', 11, 38, 32, 6, 36, 4, 37, 24
'Brand#33', 'MEDIUM ANODIZED%', 11, 37, 32, 30, 23, 3, 41, 20
