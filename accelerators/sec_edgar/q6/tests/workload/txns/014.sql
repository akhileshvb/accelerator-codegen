-- an amendment: values of a filing change
UPDATE num SET value = value + 1 WHERE adsh IN (SELECT adsh FROM sub WHERE fy = 2022 ORDER BY adsh LIMIT 3);
