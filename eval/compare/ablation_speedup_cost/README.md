# Agent pipeline ablation: speedup vs synthesis cost

Figure `data/speedup_vs_cost.pdf`. One point per generator and accelerator kind: the mean synthesis cost per query
(USD) and the geomean read speedup over DuckDB of the accelerators it produced, over TPC-H Q1, Q4, Q6, Q7, Q11, Q12
and Q16. Generators: JetStream with learned
memory and proof library (`warm`), JetStream without them (`cold`), and a single Claude Code agent given the same
substrate, tests, and acceptance criteria (`raw`). `correct` is false for a point whose accelerators were flagged or
failed the hidden tests.

Regenerate the figure from the committed data:

    python3 plot_speedup_cost.py data/points.csv data/speedup_vs_cost
