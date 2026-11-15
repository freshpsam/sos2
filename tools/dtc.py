#!/usr/bin/env python3
"""Use the installed device-tree compiler or this project's local copy."""
import os
import sys
from host_tools import find_tool, prepend_library_path

def main():
    binary, local = find_tool("DTC", "dtc", "dtc")
    if not binary:
        sys.exit("Install device-tree-compiler, or set DTC to its executable path.")
    if local:
        prepend_library_path(local / "usr/lib/x86_64-linux-gnu")
    os.execvp(binary, [binary, *sys.argv[1:]])

if __name__ == "__main__":
    main()
