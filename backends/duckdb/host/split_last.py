"""Split SQL on its last statement: write the preceding statements to argv[1], the last to argv[2].

A query is written as a CSV copy to stdout: NULL comes out bare and the empty string as "" (host_contract.md)."""
import re
import sys

text = sys.stdin.read().rstrip()
text = text[:-1].rstrip() if text.endswith(";") else text
cut = text.rfind(";") + 1
head, tail = text[:cut], text[cut:].strip()
open(sys.argv[1], "w").write(head + "\n")
if re.match(r"(?i)\s*\(*\s*(select|with|values|from|table)\b", tail):
    tail = f"COPY ({tail}) TO '/dev/stdout' (FORMAT csv, HEADER, NULLSTR '')"
open(sys.argv[2], "w").write(tail + ";\n")
