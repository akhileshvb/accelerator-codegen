UPDATE customer SET c_name = 'Q18 Customer' || RIGHT(c_comment, 2)
WHERE c_comment IN ('q18:customer:01', 'q18:customer:02', 'q18:customer:03', 'q18:customer:04');
