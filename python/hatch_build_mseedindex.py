import os
import sys
import shutil
import subprocess
from packaging.tags import sys_tags

from hatchling.builders.hooks.plugin.interface import BuildHookInterface

class CustomBuildHook(BuildHookInterface):

    # Pack root diretory is the parent directory containing this file
    package_root = os.path.abspath(os.path.join(os.path.dirname(__file__), os.pardir))

    def initialize(self, version, build_data):
        # Set wheel tag, e.g. py3-none-macosx_14_0_x86_64
        python_tag = 'py3'
        abi_tag = 'none'
        platform_tag = next(sys_tags()).platform
        build_data['tag'] = f'{python_tag}-{abi_tag}-{platform_tag}'
        build_data['pure_python'] = False

        print(f"Building mseedindex via Makefile in {self.package_root}")

        if sys.platform.lower().startswith("win"):
            cmd = f"nmake /f Makefile.win"
            binary_name = "mseedindex.exe"
        else:
            cmd = f"CFLAGS='-O2' make -j"
            binary_name = "mseedindex"

        subprocess.check_call(cmd, cwd=self.package_root, shell=True)

        # Add the binary to the force_include list
        build_data["force_include"][binary_name] = f"mseedindex/{binary_name}"

    def clean(self, versions):
        if sys.platform.lower().startswith("win"):
            cmd = f"pushd {self.package_root} && nmake /f Makefile.win clean & popd"
        else:
            cmd = f"make -C {self.package_root} clean"

        subprocess.check_call(cmd, shell=True)

    def finalize(self, version, build_data, artifact_path):
        pass
