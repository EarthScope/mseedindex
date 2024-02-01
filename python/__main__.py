# A Python wrapper for PyPI packaging entry point
import os
import sys
import subprocess


def main() -> None:
    sys.exit(subprocess.call([
             os.path.join(os.path.dirname(__file__), "mseedindex"),
             *sys.argv[1:]
             ]))
