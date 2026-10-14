'ALGERIA', 0.0001
'ARGENTINA', 0.0001
'BRAZIL', 0.0001
'CANADA', 0.0001
'EGYPT', 0.0001
'ETHIOPIA', 0.0001
'FRANCE', 0.0001
'GERMANY', 0.0001
'INDIA', 0.0001
'INDONESIA', 0.0001
'IRAN', 0.0001
'IRAQ', 0.0001
'JAPAN', 0.0001
'JORDAN', 0.0001
'KENYA', 0.0001
'MOROCCO', 0.0001
'MOZAMBIQUE', 0.0001
'PERU', 0.0001
'CHINA', 0.0001
'ROMANIA', 0.0001
'SAUDI ARABIA', 0.0001
'VIETNAM', 0.0001
'RUSSIA', 0.0001
'UNITED KINGDOM', 0.0001
'UNITED STATES', 0.0001
'Q11 SMALL', 0.0001
'Q11 EDGE', 0.0001
'Q11 BAG', 0.0001
'germany', 0.0001
'Germany', 0.0001
'GERMANY ', 0.0001
' GERMANY', 0.0001
'Q11 CAFÉ', 0.0001
'Q11 CAFÉ', 0.0001
'NO SUCH NATION', 0.0001
'', 0.0001
'                         ', 0.0001
-- edge cases of a parameter that was a literal when the tests were written (scripts/gen_verus_kit_params.py)
-- $2 was the literal 0.0001; its edge values, the other parameters as in the first tuple
'ALGERIA', 0.00001
'ALGERIA', 0.001
'ALGERIA', 0.01
'ALGERIA', 0.0
'ALGERIA', 1.0
'ALGERIA', 0.000000001
'ALGERIA', 0.0000000001
-- random parameter sets drawn from the domain when the tests were frozen
-- 8 sets, seed 20261014 (eval/compare/gen_tpch_params.py 8 20261014; regenerate: scripts/gen_verus_kit_params.py)
'BRAZIL', 0.000005
'INDONESIA', 0.000005
'MOZAMBIQUE', 0.000005
'CANADA', 0.000005
'IRAQ', 0.000005
'MOROCCO', 0.000005
'ARGENTINA', 0.000005
'ALGERIA', 0.000005
