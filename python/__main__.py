# A Python wrapper for PyPI packaging entry point
import subprocess 
import sys

from mseedindex import get_path_to_binary

def main():
    binary_path = get_path_to_binary()
    result = subprocess.run([str(binary_path)] + sys.argv[1:])
    sys.exit(result.returncode)
