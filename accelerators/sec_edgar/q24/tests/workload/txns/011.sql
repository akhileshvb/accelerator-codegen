-- the most common group is wiped out and rebuilt in two statements (duplicate rows, same key)
DELETE FROM num WHERE adsh = '0001558370-24-012598' AND tag = 'StockholdersEquityIncludingPortionAttributableToNoncontrollingInterest';
INSERT INTO num SELECT * FROM num WHERE adsh = '0001558370-24-012598' AND tag = 'RevenueFromContractWithCustomerExcludingAssessedTax';
