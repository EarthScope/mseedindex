import sys
from pathlib import Path

def get_path_to_binary() -> Path:
    """
    Returns a path to a binary of mseedindex, regardless of the OS.
    """
    if sys.platform.lower().startswith("win"):
        binary_name = "mseedindex.exe"
    else:
        binary_name = "mseedindex"

    binary_path = Path(__file__).parent / binary_name
    return binary_path

