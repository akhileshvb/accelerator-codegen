-- bulk_delete / lineitem: withdraw the updated chunk of 020/021 in one statement.
DELETE FROM lineitem WHERE l_shipmode = 'Q14RF1';
