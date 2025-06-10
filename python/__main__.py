# A Python wrapper for PyPI packaging entry point
import subprocess
import sys
from pathlib import Path

def main():
    if sys.platform.lower().startswith("win"):
        binary_name = "mseedindex.exe"
    else:
        binary_name = "mseedindex"

    binary_path = Path(__file__).parent / binary_name

    result = subprocess.run([str(binary_path)] + sys.argv[1:])
    sys.exit(result.returncode)