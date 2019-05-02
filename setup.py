from setuptools.dist import Distribution
from setuptools import setup, find_packages
from setuptools.command.install import install
from setuptools.command.develop import develop
from setuptools.command.sdist import sdist
from io import open
from tempfile import gettempdir
import pkg_resources
import subprocess
import os
import sys
import zipfile
import glob
import shutil
import re

module_name = 'mseedindex'

with open(os.path.join(os.path.abspath(os.path.dirname(__file__)),
                       "README.md"), encoding='utf-8') as fh:
    long_description = fh.read()

# python 2 / 3 compatibility
try:
    from urllib.request import urlopen
except ImportError:
    from urllib2 import urlopen

dist_options = dict(
    name=module_name,
    version = pkg_resources.get_distribution(module_name).version,
    author="IRIS",
    author_email="software-owner@iris.washington.edu",
    description="Python hook for installing mseedindex",
    long_description=long_description,
    long_description_content_type="text/markdown",
    url="https://github.com/iris-edu/mseedindexpypy",
    packages=find_packages(),
    python_requires='>=2.7, <4',
    classifiers=[
        "Development Status :: 5 - Production/Stable",
        "Environment :: Console",
        "Intended Audience :: Science/Research",
        "License :: OSI Approved :: GNU General Public License v3 (GPLv3)",
        "Operating System :: Unix",
        "Operating System :: MacOS",
        "Programming Language :: Python :: 2.7",
        "Programming Language :: Python :: 3.5",
        "Programming Language :: Python :: 3.6",
        "Programming Language :: Python :: 3.7",
    ]
)


class InstallBase():

    def get_virtualenv_path(self):
        """Used to work out path to install compiled binaries to."""
        return os.path.join(sys.prefix, 'bin')

    def download_mseedindex(self):
        # download mseed index zip ball
        url = 'https://api.github.com/repos/iris-edu/mseedindex/zipball'
        temp_dir = gettempdir()
        mseed_index_zip = os.path.join(temp_dir, "mseedindex.zip")
        f = urlopen(url)
        data = f.read()
        with open(mseed_index_zip, "wb") as fd:
            fd.write(data)
        # extract zip in system temporary directory
        zip_ref = zipfile.ZipFile(mseed_index_zip, 'r')
        exract_path = os.path.join(temp_dir, 'mseedindex')
        zip_ref.extractall(exract_path)
        zip_ref.close()
        # return extracted zip file
        return glob.glob(os.path.join(exract_path, '*'))[0]

    def compile_and_install_mseedindex(self, mseedindex_path):
        """Used the subprocess module to compile/install mseedindex."""
        # compile the software
        cmd = "WITHOUTPOSTGRESQL=1 CFLAGS='-O2' make"
        subprocess.check_call(cmd, cwd=mseedindex_path, shell=True)
        mseedindex_binary = os.path.join(mseedindex_path, 'mseedindex')
        mseedindex_binary_dest = self.get_mseedindex_path()
        shutil.copy(mseedindex_binary, mseedindex_binary_dest)
        return mseedindex_binary_dest

    def install_mseedindex(self):
        try:
            mseedindex_path = self.download_mseedindex()
            mseedindex_binary = self.compile_and_install_mseedindex(
                                                            mseedindex_path)
            print("Successfully installed mseedindex at {}"
                  .format(mseedindex_binary))
        except Exception as e:
            raise Exception("Failed to install mseedindex - {}"
                            .format(e))

    def get_mseedindex_path(self):
        venv = self.get_virtualenv_path()
        mseedindex_binary = os.path.join(venv, 'mseedindex')
        return mseedindex_binary


class DevelopMSeedIndex(develop, InstallBase):

    def initialize_options(self):
        develop.initialize_options(self)

    def run(self):
        self.install_mseedindex()
        develop.run(self)


class InstallMSeedIndex(install, InstallBase):

    def initialize_options(self):
        install.initialize_options(self)

    def run(self):
        self.install_mseedindex()
        install.run(self)


class SDistMSeedIndex(sdist):

    def get_version(self):
        with open(os.path.join(os.path.abspath(os.path.dirname(__file__)),
                               "src/mseedindex.c"), encoding='utf-8') as fh:
            # extract mseedindex.c version from the source code
            result = re.search('#define VERSION "(.*)"', fh.read())
            version = result.group(1)
            return version

    def run(self):
        dist_options["version"] = self.get_version()
        dist = Distribution(dist_options)
        dist.script_name = 'setup.py'
        cmd = sdist(dist)
        cmd.ensure_finalized()
        cmd.run()


setup(
    cmdclass={'install': InstallMSeedIndex,
              'develop': DevelopMSeedIndex,
              'sdist': SDistMSeedIndex},
    **dist_options
)
