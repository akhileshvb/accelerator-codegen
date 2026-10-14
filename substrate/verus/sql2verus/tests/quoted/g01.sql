SELECT "Owner", SUM(t.amt) AS total FROM "Accounts" a JOIN tx t ON t.account_id = a."AccountId" GROUP BY "Owner" ORDER BY "Owner"
