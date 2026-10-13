"""Generate C source/header files from Lua scripts using templates.

Supports two modes:
  source   - Escapes a .lua file into a C string and substitutes into a template.
  bytecode - Compiles a .lua file to bytecode via luac -s and substitutes the
             resulting byte array into a template.

Template placeholders:
  @FILE_NAME@        - Base name of the Lua script (without extension)
  @LUA_CONTENT@      - (source mode) C-escaped script contents
  @LUA_BYTECODE@     - (bytecode mode) Comma-separated hex bytes
  @LUA_BYTECODE_LEN@ - (bytecode mode) Byte count

Replaces lua_cat.py and lua_compile.py with a single script that writes the
final output file directly, enabling CMake add_custom_command dependency
tracking on the .lua source.
"""

import argparse
import os
import subprocess
import sys
import tempfile


def lua_to_c_string(path):
    """Read a .lua file and return a C-escaped string literal body."""
    with open(path, "r") as f:
        script = f.read()

    strings = ["\\\n"]
    for line in script.splitlines():
        line = line.replace("\\", "\\\\")
        line = line.replace('"', '\\"')
        line = line.replace("\r", "")
        strings.append(line + "\\n")

    return " \\\n".join(strings)


def lua_to_bytecode(luac, path):
    """Compile a .lua file and return (length, hex_bytes) strings."""
    with tempfile.NamedTemporaryFile(suffix=".luac", delete=False) as tmp:
        tmp_path = tmp.name

    try:
        result = subprocess.run(
            [luac, "-s", "-o", tmp_path, path],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            print(f"luac error: {result.stderr}", file=sys.stderr)
            sys.exit(1)

        with open(tmp_path, "rb") as f:
            data = f.read()

        byte_count = str(len(data))
        hex_bytes = ", ".join(f"0x{b:02x}" for b in data)
        return byte_count, hex_bytes
    finally:
        os.unlink(tmp_path)


def main():
    parser = argparse.ArgumentParser(
        description="Generate C source/header from a Lua script and a template."
    )
    parser.add_argument(
        "--mode",
        choices=["source", "bytecode"],
        required=True,
        help="Processing mode: escape source or compile to bytecode",
    )
    parser.add_argument("--template", required=True, help="Path to the .in template")
    parser.add_argument("--output", required=True, help="Path to the output file")
    parser.add_argument(
        "--name", required=True, help="Value for @FILE_NAME@ placeholder"
    )
    parser.add_argument(
        "--luac", default=None, help="Path to host luac binary (bytecode mode)"
    )
    parser.add_argument("file", help="Lua script file to process")
    args = parser.parse_args()

    if args.mode == "bytecode" and args.luac is None:
        parser.error("--luac is required in bytecode mode")

    with open(args.template, "r") as f:
        template = f.read()

    output = template.replace("@FILE_NAME@", args.name)
    output = output.replace("@FILE_NAME_UPPER@", args.name.upper())

    if args.mode == "source":
        content = lua_to_c_string(args.file)
        output = output.replace("@LUA_CONTENT@", content)
    else:
        byte_count, hex_bytes = lua_to_bytecode(args.luac, args.file)
        output = output.replace("@LUA_BYTECODE@", hex_bytes)
        output = output.replace("@LUA_BYTECODE_LEN@", byte_count)

    os.makedirs(os.path.dirname(args.output), exist_ok=True)

    with open(args.output, "w") as f:
        f.write(output)


if __name__ == "__main__":
    main()
