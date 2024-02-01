# A Python wrapper for PyPI packaging
import os, sys, subprocess

def main() -> None:
    sys.exit(subprocess.call([
             os.path.join(os.path.dirname(__file__), "mseedindex"),
             *sys.argv[1:]
    ]))

if __name__ == "__main__":
    main()