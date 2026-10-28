SELECT id, strlen(kind) AS n FROM ev WHERE strlen(kind) > 3 AND length(cc) = 2 ORDER BY id
