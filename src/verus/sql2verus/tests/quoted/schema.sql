CREATE TABLE "Accounts" ("AccountId" INTEGER PRIMARY KEY, "Owner" VARCHAR(40) NOT NULL, balance NUMERIC(12,2) NOT NULL, "type" CHAR(3) NOT NULL);
CREATE TABLE tx (id BIGINT NOT NULL, account_id INTEGER NOT NULL REFERENCES "Accounts"("AccountId"), amt INTEGER NOT NULL, memo TEXT, CHECK (amt <> 0));
CREATE INDEX tx_acct ON tx(account_id);
