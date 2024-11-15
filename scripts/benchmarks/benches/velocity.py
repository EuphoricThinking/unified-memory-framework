# Copyright (C) 2024 Intel Corporation
# Part of the Unified-Runtime Project, under the Apache License v2.0 with LLVM Exceptions.
# See LICENSE.TXT
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import re
import shutil
from utils.utils import git_clone
from .base import Benchmark, Suite
from .result import Result
from utils.utils import run, create_build_path
from .options import options
import os

class VelocityBench(Suite):
    def __init__(self, directory):
        if options.sycl is None:
            return

        self.directory = directory

    def setup(self):
        if options.sycl is None:
            return

        self.repo_path = git_clone(self.directory, "velocity-bench-repo", "https://github.com/oneapi-src/Velocity-Bench/", "b22215c16f789100449c34bf4eaa3fb178983d69")

    def benchmarks(self) -> list[Benchmark]:
        if options.sycl is None:
            return []

        return [
            Hashtable(self),
            Bitcracker(self),
            CudaSift(self),
            Easywave(self),
            QuickSilver(self),
            SobelFilter(self)
        ]

class VelocityBase(Benchmark):
    def __init__(self, name: str, bin_name: str, vb: VelocityBench):
        super().__init__(vb.directory)
        self.vb = vb
        self.bench_name = name
        self.bin_name = bin_name
        self.code_path = os.path.join(self.vb.repo_path, self.bench_name, 'SYCL')

    def download_deps(self):
        return

    def setup(self):
        self.download_deps()
        self.benchmark_bin = os.path.join(self.directory, self.bench_name, self.bin_name)

        build_path = create_build_path(self.directory, self.bench_name)

        configure_command = [
            "cmake",
            f"-B {build_path}",
            f"-S {self.code_path}",
            f"-DCMAKE_BUILD_TYPE=Release"
        ]
        run(configure_command, {'CC': 'clang', 'CXX':'clang++'}, add_sycl=True)
        run(f"cmake --build {build_path} -j", add_sycl=True)

    def bin_args(self) -> list[str]:
        return []

    def extra_env_vars(self) -> dict:
        return {}

    def parse_output(self, stdout: str) -> float:
        raise NotImplementedError()

    def run(self, env_vars) -> list[Result]:
        env_vars.update(self.extra_env_vars())

        command = [
            f"{self.benchmark_bin}",
        ]
        command += self.bin_args()

        result = self.run_bench(command, env_vars)

        return [ Result(label=self.name(), value=self.parse_output(result), command=command, env=env_vars, stdout=result) ]

    def teardown(self):
        return

class Hashtable(VelocityBase):
    def __init__(self, vb: VelocityBench):
        super().__init__("hashtable", "hashtable_sycl", vb)

    def name(self):
        return "Velocity-Bench Hashtable"

    def unit(self):
        return "M keys/sec"

    def bin_args(self) -> list[str]:
        return ["--no-verify"]

    def lower_is_better(self):
        return False

    def parse_output(self, stdout: str) -> float:
        match = re.search(r'(\d+\.\d+) million keys/second', stdout)
        if match:
            return float(match.group(1))
        else:
            raise ValueError("{self.__class__.__name__}: Failed to parse keys per second from benchmark output.")


class Bitcracker(VelocityBase):
    def __init__(self, vb: VelocityBench):
        super().__init__("bitcracker", "bitcracker", vb)
        self.data_path = os.path.join(vb.repo_path, "bitcracker", "hash_pass")

    def name(self):
        return "Velocity-Bench Bitcracker"

    def unit(self):
        return "s"

    def bin_args(self) -> list[str]:
        return ["-f", f"{self.data_path}/img_win8_user_hash.txt",
                "-d", f"{self.data_path}/user_passwords_60000.txt",
                "-b", "60000"]

    def parse_output(self, stdout: str) -> float:
        match = re.search(r'bitcracker - total time for whole calculation: (\d+\.\d+) s', stdout)
        if match:
            return float(match.group(1))
        else:
            raise ValueError("{self.__class__.__name__}: Failed to parse benchmark output.")

class SobelFilter(VelocityBase):
    def __init__(self, vb: VelocityBench):
        super().__init__("sobel_filter", "sobel_filter", vb)

    def download_deps(self):
        self.download_untar("sobel_filter", "https://github.com/oneapi-src/Velocity-Bench/raw/main/sobel_filter/res/sobel_filter_data.tgz?download=", "sobel_filter_data.tgz")
        return

    def name(self):
        return "Velocity-Bench Sobel Filter"

    def unit(self):
        return "ms"

    def bin_args(self) -> list[str]:
        return ["-i", f"{self.data_path}/sobel_filter_data/silverfalls_32Kx32K.png",
                "-n", "5"]

    def extra_env_vars(self) -> dict:
        return {"OPENCV_IO_MAX_IMAGE_PIXELS" : "1677721600"}

    def parse_output(self, stdout: str) -> float:
        match = re.search(r'sobelfilter - total time for whole calculation: (\d+\.\d+) s', stdout)
        if match:
            return round(float(match.group(1)) * 1000, 3)
        else:
            raise ValueError("{self.__class__.__name__}: Failed to parse benchmark output.")


class QuickSilver(VelocityBase):
    def __init__(self, vb: VelocityBench):
        super().__init__("QuickSilver", "qs", vb)
        self.data_path = os.path.join(vb.repo_path, "QuickSilver", "Examples", "AllScattering")

    def run(self, env_vars) -> list[Result]:
        # TODO: fix the crash in QuickSilver when UR_L0_USE_IMMEDIATE_COMMANDLISTS=0
        if 'UR_L0_USE_IMMEDIATE_COMMANDLISTS' in env_vars and env_vars['UR_L0_USE_IMMEDIATE_COMMANDLISTS'] == '0':
            return None

        return super().run(env_vars)

    def name(self):
        return "Velocity-Bench QuickSilver"

    def unit(self):
        return "MMS/CTT"

    def lower_is_better(self):
        return False

    def bin_args(self) -> list[str]:
        return ["-i", f"{self.data_path}/scatteringOnly.inp"]

    def extra_env_vars(self) -> dict:
        return {"QS_DEVICE" : "GPU"}

    def parse_output(self, stdout: str) -> float:
        match = re.search(r'Figure Of Merit\s+(\d+\.\d+)', stdout)
        if match:
            return float(match.group(1))
        else:
            raise ValueError("{self.__class__.__name__}: Failed to parse benchmark output.")

class Easywave(VelocityBase):
    def __init__(self, vb: VelocityBench):
        super().__init__("easywave", "easyWave_sycl", vb)

    def download_deps(self):
        self.download_untar("easywave", "https://git.gfz-potsdam.de/id2/geoperil/easyWave/-/raw/master/data/examples.tar.gz", "examples.tar.gz")

    def name(self):
        return "Velocity-Bench Easywave"

    def unit(self):
        return "ms"

    def bin_args(self) -> list[str]:
        return ["-grid", f"{self.data_path}/examples/e2Asean.grd",
                "-source", f"{self.data_path}/examples/BengkuluSept2007.flt",
                "-time", "120"]

    # easywave doesn't output a useful single perf value. Instead, we parse the
    # output logs looking for the very last line containing the elapsed time of the
    # application.
    def get_last_elapsed_time(self, log_file_path) -> float:
        elapsed_time_pattern = re.compile(r'Model time = (\d{2}:\d{2}:\d{2}),\s+elapsed: (\d+) msec')
        last_elapsed_time = None

        try:
            with open(log_file_path, 'r') as file:
                for line in file:
                    match = elapsed_time_pattern.search(line)
                    if match:
                        last_elapsed_time = int(match.group(2))

            if last_elapsed_time is not None:
                return last_elapsed_time
            else:
                raise ValueError("No elapsed time found in the log file.")
        except FileNotFoundError:
            raise FileNotFoundError(f"The file {log_file_path} does not exist.")
        except Exception as e:
            raise e

    def parse_output(self, stdout: str) -> float:
        return self.get_last_elapsed_time(os.path.join(options.benchmark_cwd, "easywave.log"))


class CudaSift(VelocityBase):
    def __init__(self, vb: VelocityBench):
        super().__init__("cudaSift", "cudaSift", vb)

    def download_deps(self):
        images = os.path.join(self.vb.repo_path, self.bench_name, 'inputData')
        dest = os.path.join(self.directory, 'inputData')
        if not os.path.exists(dest):
            shutil.copytree(images, dest)

    def name(self):
        return "Velocity-Bench CudaSift"

    def unit(self):
        return "ms"

    def parse_output(self, stdout: str) -> float:
        match = re.search(r'Avg workload time = (\d+\.\d+) ms', stdout)
        if match:
            return float(match.group(1))
        else:
            raise ValueError("Failed to parse benchmark output.")