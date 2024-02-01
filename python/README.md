# Overview

The Python packaging specified in pyproject.toml and support files
included here allow a package to be built and submissted to PyPI.

# Build distribution packages

You may wish to run these commands in a virtual environment

## Ensure required build modules are installed
python3 -m pip install build twine

## Build sdist only
python3 -m build --sdist

## Test and upload to PyPI
python3 -m twine check --strict dist/*
python3 -m twine upload dist/*
