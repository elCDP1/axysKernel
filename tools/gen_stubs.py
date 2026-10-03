"""
Emit the ISR stub table for every vector the kernel does not already handle.

Vectors 48..255 are "unexpected" in a kernel with no device drivers beyond
the 8259s, but an IDT slot that is not-present turns a stray interrupt into
#GP, and a #GP on a possibly-corrupt stack into a triple fault. Filling all
256 slots with a real handler turns any such event into a clean, reportable
panic instead of a silent reboot.

Run:  python3 tools/gen_stubs.py > /tmp/stubs.txt
"""

EXTRA = list(range(48, 256))

print("/*")
print(" * Vectors 48..255. Nothing in this kernel is expected to raise these")
print(" * yet, but every IDT slot must point at a present gate: a not-present")
print(" * entry escalates a stray interrupt to #GP, which on a damaged stack is")
print(" * a triple fault. These stubs make that a reportable panic instead.")
print(" *")
print(" * They all push a dummy error code so the frame shape matches the real")
print(" * exception stubs and the C handler needs no special case.")
print(" */")
for n in EXTRA:
    print("UNEXPECTED %d" % n)
