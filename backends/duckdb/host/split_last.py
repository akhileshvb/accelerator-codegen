"""Split SQL on its last statement: write the preceding statements to argv[1], the last to argv[2]."""
import sys

text = sys.stdin.read().rstrip()
text = text[:-1].rstrip() if text.endswith(";") else text
cut = text.rfind(";") + 1
head, tail = text[:cut], text[cut:].strip()
open(sys.argv[1], "w").write(head + "\n")
open(sys.argv[2], "w").write(tail + ";\n")
