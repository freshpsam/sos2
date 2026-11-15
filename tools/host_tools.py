"""Find host tools, falling back to copies extracted under the project's .tools/."""
import os
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]


def find_tool(env_var, executable, package):
    """Return (binary, local_root); local_root is set only for a .tools/<package> copy."""
    binary = os.environ.get(env_var) or shutil.which(executable)
    if binary:
        return binary, None
    local_root = ROOT / ".tools" / package
    if (local_root / "usr/bin" / executable).is_file():
        return str(local_root / "usr/bin" / executable), local_root
    return None, None


def prepend_library_path(directory):
    """Prefer a local copy's libraries in this process and its children.

    An empty LD_LIBRARY_PATH entry means the current directory to ld.so, so none is
    added, and repeated calls do not stack copies of the same directory.
    """
    directory = str(directory)
    current = os.environ.get("LD_LIBRARY_PATH", "")
    entries = current.split(":") if current else []
    if directory not in entries:
        os.environ["LD_LIBRARY_PATH"] = ":".join([directory, *entries])
