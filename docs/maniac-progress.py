#!/usr/bin/env python3
"""Display the delivery board without generating new scope or changing state."""
from pathlib import Path
import sys

if __name__ == "__main__":
    if len(sys.argv) != 1:
        raise SystemExit("Historical ledger generation is retired. Read docs/maniac-completion-plan.md.")
    sys.stdout.reconfigure(encoding="utf-8")
    print(Path(__file__).with_suffix(".md").read_text(encoding="utf-8"), end="")
