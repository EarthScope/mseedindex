# Overview

The Python packaging specified in pyproject.toml and support files
included here allow a package to be built and installed via pip.

Release builds are created automatically on GitHub and published
to PyPI.

# Build distribution source package

You may wish to run these commands in a virtual environment

## Ensure required build modules are installed
python3 -m pip install build twine

## Build sdist only
python3 -m build --sdist

The source distribution can be installed using pip:

pip install /path/to/project/dist/mseedindex-3.0.7.tar.gz
