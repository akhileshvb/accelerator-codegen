# Workload schema -> the types the PG accelerators read: unpadded text in byte order.
s/\b(VAR)?CHAR\([0-9]+\)/VARCHAR COLLATE "C"/Ig
