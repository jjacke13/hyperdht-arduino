"""Prepare an isolated Uno R4 WiFi SDK 1.6.0 variant for the stack probe."""

from pathlib import Path
import re
import sys


def replace_once(text, pattern, replacement):
    result, count = re.subn(pattern, replacement, text, flags=re.MULTILINE)
    if count != 1:
        raise SystemExit(f"Unexpected R4 linker layout for {pattern!r}")
    return result


source = Path(sys.argv[1]).resolve(strict=True)
target = Path(sys.argv[2])
if source.parts[-3:] != ("1.6.0", "variants", "UNOWIFIR4"):
    raise SystemExit(f"Expected the pinned Uno R4 WiFi variant, got {source}")

regions = replace_once((source / "memory_regions.ld").read_text(),
                       r"^BSP_CFG_STACK_MAIN_BYTES = 0x400;$",
                       "BSP_CFG_STACK_MAIN_BYTES = 0x2000;")
script = (source / "fsp.ld").read_text()
# The allocator uses __HeapBase/__HeapLimit. The SDK's input array merely
# reserves a fixed range. Reserve the actual range up to the enlarged stack.
script = replace_once(script, r"^    \.heap \(NOLOAD\):$", "    .heap ALIGN(8) (NOLOAD):")
script = replace_once(script, r"^        KEEP\(\*\(\.heap\)\)$",
                      "        . = ABSOLUTE(ORIGIN(RAM) + LENGTH(RAM) - "
                      "VECTOR_TABLE_LENGTH - BSP_CFG_STACK_MAIN_BYTES);")
# The reset vector points to the end of the SDK's 1 KiB stack object. Pad
# below that object so its end remains at the top of the expanded stack.
script = replace_once(script, r"^        KEEP\(\*\(\.stack\)\)$",
                      "        . += BSP_CFG_STACK_MAIN_BYTES - 0x400;\n"
                      "        KEEP(*(.stack))")
script = replace_once(script, r"^\}\s*\Z", "    /DISCARD/ : { *libfsp.a:startup.o(.heap) }\n}\n")

target.mkdir()
for entry in source.iterdir():
    if entry.name not in ("fsp.ld", "memory_regions.ld"):
        (target / entry.name).symlink_to(entry)
(target / "fsp.ld").write_text(script)
(target / "memory_regions.ld").write_text(regions)
