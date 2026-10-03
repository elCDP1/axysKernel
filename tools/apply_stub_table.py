"""
Splice the generated 256-entry stub table into arch/x86_64/interrupts.S,
replacing whatever .rodata table is currently there.

Run:  python3 tools/apply_stub_table.py
"""
import re
import subprocess
import sys
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
ASM = ROOT / "arch" / "x86_64" / "interrupts.S"
GEN = ROOT / "tools" / "gen_stub_table.py"

table = subprocess.run(
    [sys.executable, str(GEN)], capture_output=True, text=True, check=True
).stdout.rstrip("\n")

text = ASM.read_text()
marker = "axys_isr_stub_table:"
idx = text.index(marker)
# Back up over the .align/.global lines that introduce the table.
start = text.rindex("\n", 0, text.rindex(".global", 0, idx)) + 1
# The table runs to the end of the file (it is the last thing in .rodata).
new = text[:start] + table + "\n"
ASM.write_text(new)
print("stub table spliced: %d entries" % (new.count(".quad")))
