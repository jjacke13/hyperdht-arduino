"""Keep GCC TLS out of every task's stack, especially the 1536-byte idle stack."""
import subprocess
import sys

if len(sys.argv) != 2:
    sys.exit("Usage: python scripts/check-tls.py firmware.elf")
symbols = {}
for line in subprocess.check_output(
    ["xtensa-esp32s3-elf-nm", "--defined-only", sys.argv[1]], text=True
).splitlines():
    fields = line.split()
    if len(fields) == 3 and fields[2] in ("_thread_local_start", "_thread_local_end"):
        symbols[fields[2]] = int(fields[0], 16)
size = symbols["_thread_local_end"] - symbols["_thread_local_start"]
if size:
    sys.exit(f"GCC TLS is {size} bytes per task; this demo requires zero. Check the dependency patch.")
print("GCC TLS: 0 bytes per task")
