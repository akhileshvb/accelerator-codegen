"""The Verus accelerator pipeline (``run_pipeline --mode verus``): see stages.py."""

STAGES = ("preflight", "spec", "analysis", "freeze", "tests", "glue", "kernel", "integration", "optimization", "audit")
