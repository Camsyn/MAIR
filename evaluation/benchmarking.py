#!/usr/bin/env /usr/bin/python3

'''
This script automates performance and memory evaluation for benchmark programs under different Sanitizer configurations.
It stores result data for later analysis.

Main functions:
- Iterate over benchmark programs defined in benchmarks.py.
- Repeat runs for each benchmark and Sanitizer combination, collecting execution time and RSS.
- Support multiprocessing and assign CPU affinity automatically.
- Filter abnormal data and save/load test results.
- Allow evaluating only selected benchmarks.

Usage:
1. Ensure benchmarks.py has been generated (for example by running evaluation/gen_bench_config.py).
2. Run this script from the evaluation directory:

   python3 benchmarking.py

3. Results are saved under each package/data/ directory as .npy and .txt files.

Optional configuration:
- Modify BENCH_TO_RUN to evaluate only selected programs (for example {'cxxfilt',}).
- Modify REPEAT_TIMES to control repeats per input.
- Modify MAX_WORKERS to control worker concurrency.

Dependencies:
- Python 3
- numpy
- benchmarks.py configuration file
- benchmark artefacts and corpora
- eval-sample.sh for actual measurement

'''


import gen_bench_config
import os
import subprocess
from pathlib import Path
import concurrent.futures
import multiprocessing
import sys
import numpy as np
import shutil

SCRIPT_DIR = Path(__file__).resolve().parent

gen_bench_config.generate_bench_configs(lazy=True)
from benchmarks import BENCHMARKS, PACKAGE_2_PROGRAMS


# REPEAT_TIMES = 1
REPEAT_TIMES = 40
MAX_WORKERS = 5

'''
Platform-Specific Initialization Time

Measured by repeating the initialization routine 100 times.
XSan initialization time is generally smaller than the sum of selected sanitizers because common initialization is not repeated.
'''
INIT_TIMES = {
    'asan': 1.798,
    'msan': 1.918,
    'tsan': 6.373,
    'ubsan': 0.281,
    'xsan-asan': 2.210,
    'xsan-asan-msan': 2.538,
    'xsan-asan-tsan': 8.042,
    'xsan-asan-ubsan': 2.261,
    'xsan-asan-msan-tsan': 8.223,
    'xsan-asan-msan-ubsan': 2.603,
    'xsan-asan-tsan-ubsan': 8.156,
    'xsan-asan-msan-tsan-ubsan': 8.316,
}


SANITIZERS = [
    "raw",
    "asan",
    "msan",
    "tsan",
    "ubsan",
    "xsan-asan",
    "xsan-asan-tsan",
    "xsan-asan-msan",
    "xsan-asan-ubsan",
    "xsan-asan-tsan-ubsan",
    "xsan-asan-msan-tsan",
    "xsan-asan-msan-ubsan",
    "xsan-asan-msan-tsan-ubsan",
]


RERUN = {
    'xsan-asan',
    'raw',
    'xsan-asan-msan',
    'xsan-asan-tsan',
    'xsan-asan-tsan-ubsan',
    'xsan-asan-msan-tsan',
    'xsan-asan-msan-ubsan',
    'xsan-asan-msan-tsan-ubsan',
}

BENCH_TO_RUN = {
    *PACKAGE_2_PROGRAMS['binutils'],
    *PACKAGE_2_PROGRAMS['pcre2'],
    *PACKAGE_2_PROGRAMS['zlib'],
    *PACKAGE_2_PROGRAMS['7zip'],
    *PACKAGE_2_PROGRAMS['zstd'],
    *PACKAGE_2_PROGRAMS['xz'],
    *PACKAGE_2_PROGRAMS['lua'],
    *PACKAGE_2_PROGRAMS['tpm2-tss'],
    *PACKAGE_2_PROGRAMS['strongswan'],
    *PACKAGE_2_PROGRAMS['iproute2'],
    *PACKAGE_2_PROGRAMS['cJSON'],
    *PACKAGE_2_PROGRAMS['json'],
    *PACKAGE_2_PROGRAMS['jsoncpp'],
    *PACKAGE_2_PROGRAMS['sqlite3'],
    *PACKAGE_2_PROGRAMS['libjpeg-turbo'],
    *PACKAGE_2_PROGRAMS['libpng'],
    *PACKAGE_2_PROGRAMS['libxml2'],
    *PACKAGE_2_PROGRAMS['lcms'],
    *PACKAGE_2_PROGRAMS['re2'],
    *PACKAGE_2_PROGRAMS['curl'],
    *PACKAGE_2_PROGRAMS['openssl'],
}

FORCE = False


# disable LSan
os.environ['ASAN_OPTIONS'] = 'detect_leaks=0'
os.environ['TSAN_OPTIONS'] = 'report_bugs=0'
# Use perf to measure the performance
os.environ['USE_PERF'] = 'true'


def filter_outliers_and_mean(data, threshold=2):
    """
    Filters out outliers in each row of the data that are more than 'threshold' standard deviations 
    away from the mean, and computes the mean of the remaining values.

    Parameters:
    ----------
    data : np.ndarray
        2D array of shape (size_corpus, REPEAT_TIMES).
    threshold : float, optional
        The number of standard deviations to use for filtering outliers (default is 2).

    Returns:
    -------
    filtered_mean : np.ndarray
        1D array containing the mean of filtered values for each row.
    """
    if not isinstance(data, np.ndarray):
        raise ValueError("Input data must be a NumPy array.")

    if data.ndim != 2:
        raise ValueError("Input data must be a 2D array.")

    # Compute the mean and standard deviation for each row
    mean = np.mean(data, axis=1, keepdims=True)
    std = np.std(data, axis=1, keepdims=True)

    # Debug: Print mean and std
    # print("Mean per row:\n", mean)
    # print("Std per row:\n", std)

    # Create a boolean mask where True indicates the value is within the threshold
    mask = np.abs(data - mean) <= threshold * std

    # Debug: Print mask
    # print("Mask:\n", mask)

    # Apply the mask to filter out outliers
    filtered_data = np.where(mask, data, np.nan)

    # Compute the mean ignoring NaN values
    filtered_mean = np.nanmean(filtered_data, axis=1)

    return filtered_mean


class MyProcessPoolExecutor(concurrent.futures.ProcessPoolExecutor):
    manager = multiprocessing.Manager()
    cpu_list = manager.list([i for i in range(MAX_WORKERS)])
    lock = manager.Lock()

    def __init__(self, max_workers=MAX_WORKERS, mp_context=None):
        '''
        This process pool binds each worker to one core to reduce scheduling noise and compare performance more fairly.
        '''
        super().__init__(
            max_workers,
            mp_context,
            initializer=MyProcessPoolExecutor.initializer,
            initargs=(),
        )

    @staticmethod
    def initializer():
        """
        Run once in each child process:
        1. Pop a CPU ID from the shared list.
        2. Bind the process to that CPU via os.sched_setaffinity.
        """
        with MyProcessPoolExecutor.lock:
            if not MyProcessPoolExecutor.cpu_list:
                raise RuntimeError("No more CPUs to assign!")
            cpu_id = MyProcessPoolExecutor.cpu_list.pop(0)
        os.sched_setaffinity(0, {cpu_id})


SCRIPT_DIR = Path(__file__).resolve().parent

class Bench:

    def __init__(self, config: dict):
        self.program = config['program']
        self.package = config['package']
        self.package_dir = config['package_dir']
        self.data_dir = config['data_dir']
        self.directory = config['program_dir']
        self.corpus_dir = config['corpus']
        self.full_corpus = self.load_corpus()
        if not len(self.full_corpus):
            raise ValueError(f"Corpus is empty: {self.corpus_dir}")
        self.corpus = self.full_corpus
        self.run_script = self.directory / 'run.sh'
        if not self.run_script.exists():
            raise ValueError(f"Run script not found: {self.run_script}")

    def run(self, mode: str, input: str, env=None) -> tuple:
        cmd = [str(self.run_script), mode, input]
        try:
            output = subprocess.check_output(
                cmd, stderr=subprocess.STDOUT, env=env)
            return Bench._resolve_time_rss(output.decode('utf-8'))
        except subprocess.CalledProcessError as e:
            return Bench._resolve_time_rss(e.output.decode('utf-8'))

    def get_data_file(self, name: str, group: str = '') -> Path:
        if not group:
            return self.data_dir / name
        return self.data_dir / group / name

    def save_np_data(self, data: np.ndarray, name: str, group: str = ''):
        data_file = self.get_data_file(name, group)
        text_dir = data_file.parent / 'readable'
        if not text_dir.exists():
            text_dir.mkdir(parents=True, exist_ok=True)
        np.save(data_file.with_suffix('.npy'), data)
        np.savetxt(text_dir / f"{name}.txt",
                   data, delimiter=',', fmt='%d',)

    def load_np_data(self, name: str, group: str = '') -> np.ndarray:
        data_file = self.get_data_file(name, group).with_suffix('.npy')
        if not data_file.exists():
            return None
        return np.load(data_file)

    def load_np_data_batchly(self, names: list, group: str = '') -> np.ndarray:
        return np.array([self.load_np_data(name, group) for name in names])

    def load_overhead(self, name: str, group: str = '', ignore_init: bool = True, depth: int = 1) -> np.ndarray:
        '''
        get the mean overhead of the specified sanitizer

        Args:
            name: str, the name of the sanitizer
            group: str, the group of the data
            ignore_init: bool, if True, ignore the initialization time of the sanitizer
            depth: int, the depth of the data
                if depth == 0, return the overhead of the specified sanitizer
                if depth >= 1, further average the REPEAT_TIMES
                if depth == 2, further average the size_corpus
        '''
        base = self.load_np_data('raw', group)
        data = self.load_np_data(name, group)
        print(f"base: {base} {name} {group} {self.data_dir}")
        print(f"data: {data}")
        # base & data shape: (size_corpus, REPEAT_TIMES)
        # averge the REPEATS, filter out the outliers (> 2 \sigma)
        if depth >= 1:
            base = filter_outliers_and_mean(base)
            data = filter_outliers_and_mean(data)
        if ignore_init:
            data -= INIT_TIMES.get(name, 0)
        # get the overhead
        overhead = data / base * 100
        if depth == 2:
            overhead = np.mean(overhead)
        elif depth > 2:
            raise ValueError(
                f"Invalid depth: {depth} , only support depth <= 2")
        return overhead

    def load_overhead_batchly(self, names: list, group: str = '', 
                              ignore_init: bool = True, depth: int = 1) -> np.ndarray:
        data = np.array([self.load_overhead(name, group, ignore_init, depth) for name in names])
        return data

    def remove_np_data(self, name: str, group: str = ''):
        data_file = self.get_data_file(name, group).with_suffix('.npy')
        if data_file.exists():
            data_file.unlink()
        data_file = self.get_data_file(name, group).with_suffix('.txt')
        if data_file.exists():
            data_file.unlink()

    def load_corpus(self) -> list:
        if not self.corpus_dir.exists():
            raise ValueError(
                f"Corpus directory {self.corpus_dir} does not exist")
        corpus = list(filter(lambda f: f.is_file(), self.corpus_dir.iterdir()))
        corpus.sort()
        return corpus

    def log(self):
        print(
            f"Benchmarking {self.program:15s} with {len(self.corpus):-5d}/{len(self.full_corpus):5d} seeds"
        )

    @staticmethod
    def _resolve_time_rss(output: str) -> tuple:
        '''
            Maximum resident set size (kbytes): 28168
            ...
            9.79 msec task-clock                #    0.952 CPUs utilized                      
        '''
        idx_right = output.find(' msec')
        assert idx_right != -1
        idx_left = output[:idx_right].rfind(' ')
        assert idx_left != -1
        number = output[idx_left+1:idx_right].replace(',', '')
        t_ms = float(number)
        IDENTIFIER = 'RSS     : '
        idx_left = output.find(IDENTIFIER) + len(IDENTIFIER)
        idx_right = output.find('KB', idx_left)
        number = output[idx_left:idx_right].strip().replace(',', '')
        rss_kb = float(number)
        return t_ms, rss_kb


class Benchmarker:

    def __init__(
        self,
        sanitizers: list,
        bench: Bench,
        pool: MyProcessPoolExecutor,
    ):
        self.sanitizers = sanitizers
        self.seen = set()
        self.done = set()
        self.pool = pool
        self.bench = bench
        self.corpus = self.bench.corpus
        self.max_len_san_name = max(len(sanitizer)
                                    for sanitizer in self.sanitizers)

        num_sanitizer = len(self.sanitizers)
        size_corpus = len(self.corpus)

        self.data_dir = self.bench.data_dir
        self.data_dir.mkdir(parents=True, exist_ok=True)
        self.whitelist = set(Path.cwd().iterdir())

        # data size = num_sanitizer * size_corpus * REPEAT_TIMES, type = float
        self.data = np.zeros(
            (num_sanitizer, size_corpus, REPEAT_TIMES), dtype=float)
        self.data_rss = np.zeros(
            (num_sanitizer, size_corpus, REPEAT_TIMES), dtype=float)
        self.load_data()
        self.base = self.data[0, :, :]
        self.base_rss = self.data_rss[0, :, :]

    def _benchmarking_one_sanitizer(self, sanitizer: str, env_str: str = None):
        '''
        benchmarking the specified executable on the specified corpus
        '''
        env = os.environ.copy()
        if env_str is not None:
            kv_split_idx = env_str.find('=')
            assert kv_split_idx != -1
            key = env_str[:kv_split_idx]
            value = env_str[kv_split_idx+1:]
            env[key] = value

        futures_lists = []
        for seed in self.corpus:
            futures_lists.append(self._add_task(sanitizer, seed, env))

        times_rss_2d = [[f.result() for f in futures]
                        for futures in futures_lists]
        time_2d = np.array(
            [[t_ms for t_ms, _ in times_rss] for times_rss in times_rss_2d],
            dtype=float,
        )
        rss_2d = np.array(
            [[rss_kb for _, rss_kb in times_rss]
                for times_rss in times_rss_2d],
            dtype=float,
        )
        self._clear_trash()
        return time_2d, rss_2d

    def benchmarking(self):
        '''
        benchmarking all sanitizers on the given corpus
        '''
        for idx, sanitizer in enumerate(self.sanitizers):
            if sanitizer in self.seen:
                self.print_overhead(
                    sanitizer, self.data[idx], self.data_rss[idx], skip=True)
                continue
            time_2d, rss_2d = self._benchmarking_one_sanitizer(sanitizer)
            if sanitizer == 'raw':
                self.base = time_2d
                self.base_rss = rss_2d
            if time_2d is None:
                print(
                    f"\t{sanitizer:12s} for {self.bench.program:15s}: not found")
                continue
            self.done.add(sanitizer)
            self.data[idx] = time_2d
            self.data_rss[idx] = rss_2d
            self.print_overhead(sanitizer, time_2d, rss_2d)

    def get_overhead(self, data, base=None) -> np.ndarray:
        '''
        get the mean overhead of the specified sanitizer
        '''
        # base & data shape: (size_corpus, REPEAT_TIMES)
        # averge the REPEATS, filter out the outliers (> 2 \sigma)
        base_mean = filter_outliers_and_mean(
            self.base if base is None else base)
        data_mean = filter_outliers_and_mean(data)

        # get the overhead
        overhead = data_mean / base_mean * 100

        return overhead

    def print_overhead(self, name: str, times: np.ndarray, rss: np.ndarray, 
                       ignore_init: bool = True, skip=False):
        '''
        print the overhead of the sanitizers
        '''
        if ignore_init:
            times -= INIT_TIMES.get(name, 0)
        time_overhead = self.get_overhead(times)
        rss_overhead = self.get_overhead(rss, base=self.base_rss)

        # print the mean overhead
        mean_time_overhead = np.mean(time_overhead)
        mean_rss_overhead = np.mean(rss_overhead)
        notation = '    (skip)' if skip else ''
        print(f'\t{name:{self.max_len_san_name}s} : {mean_time_overhead:8.3f} % in times,  {mean_rss_overhead:8.3f} % in RSS {notation}')

    def _clear_trash(self):
        trash = filter(lambda f: f not in self.whitelist, Path.cwd().iterdir())
        for f in trash:
            # remove file or directory
            if f.is_dir():
                shutil.rmtree(f)
            else:
                f.unlink()

    def dump_data(self):
        '''
        dump the benchmarking results to a file
        '''
        for idx, data in enumerate(self.data):
            san = self.sanitizers[idx]
            if san in self.seen or san not in self.done:
                continue
            self.bench.save_np_data(data, san, "time")

        for idx, data in enumerate(self.data_rss):
            san = self.sanitizers[idx]
            if san in self.seen or san not in self.done:
                continue
            self.bench.save_np_data(data, san, "rss")

        seed_file = self.bench.get_data_file("seeds.txt")
        with open(seed_file, 'w') as f:
            for seed in self.corpus:
                f.write(str(seed) + '\n')

    def load_data(self):
        '''
        load the benchmarking results from a file
        '''
        if FORCE:
            '''
            Force to rerun the benchmarks
            '''
            return
        for idx, sanitizer in enumerate(self.sanitizers):
            data = self.bench.load_np_data(sanitizer, "time")
            if data is None or len(data) == 0:
                continue
            # check whether filled with zeros
            if np.all(data[0, :] == 0):
                self.bench.remove_np_data(sanitizer, "time")
                continue
            self.data[idx] = data[:, :REPEAT_TIMES]
            data_rss = self.bench.load_np_data(sanitizer, "rss")
            if data_rss is None or len(data_rss) == 0:
                continue
            self.data_rss[idx] = data_rss[:, :REPEAT_TIMES]
            if sanitizer not in RERUN:
                self.seen.add(sanitizer)

    @staticmethod
    def _resolve_time_rss(output: str) -> float:
        '''
            Maximum resident set size (kbytes): 28168
            ...
            9.79 msec task-clock                #    0.952 CPUs utilized                      
        '''
        idx_right = output.find(' msec')
        assert idx_right != -1
        idx_left = output[:idx_right].rfind(' ')
        assert idx_left != -1
        number = output[idx_left+1:idx_right].replace(',', '')
        t_ms = float(number)
        IDENTIFIER = 'RSS     : '
        idx_left = output.find(IDENTIFIER) + len(IDENTIFIER)
        idx_right = output.find('KB', idx_left)
        number = output[idx_left:idx_right].strip().replace(',', '')
        rss_kb = float(number)
        return t_ms, rss_kb

    def _add_task(self, mode: str, seed: Path, env=None) -> list:
        """
        collect the execution time of the specified executable and seed
        """
        futures = [
            self.pool.submit(self.bench.run, mode, str(seed), env) for _ in range(REPEAT_TIMES)
        ]

        return futures


def benchmarking():
    '''
    benchmarking the sanitizers on the benchmarks
    '''
    pool = MyProcessPoolExecutor()
    for config in BENCHMARKS:
        if len(BENCH_TO_RUN) and (config['program'] not in BENCH_TO_RUN):
            continue
        bench = Bench(config)
        bench.log()
        benchmarker = Benchmarker(
            sanitizers=SANITIZERS, bench=bench, pool=pool
        )
        benchmarker.benchmarking()
        benchmarker.dump_data()


def main():
    benchmarking()


if __name__ == '__main__':
    if len(sys.argv) > 1:
        FORCE = sys.argv[1] == '-f'
    main()
