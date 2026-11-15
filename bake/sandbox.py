"""Linux job restrictions. Missing facilities are explicit, never a silent fallback."""

from __future__ import annotations

import ctypes
import errno
import os
import resource
import tempfile
from pathlib import Path


class SandboxUnavailable(RuntimeError):
    pass


def landlock_version() -> int:
    libc = ctypes.CDLL(None, use_errno=True)
    version = libc.syscall(444, 0, 0, 1)
    if version < 3:
        raise SandboxUnavailable(
            f"Landlock ABI >= 3 required (result={version}, errno={ctypes.get_errno()})"
        )
    return version


class _PathRule(ctypes.Structure):
    _pack_ = 1
    _fields_ = [("access", ctypes.c_uint64), ("parent", ctypes.c_int)]


def restrict_filesystem(read_paths: list[Path], output: Path):
    abi = landlock_version()
    libc = ctypes.CDLL(None, use_errno=True)
    rights = (1 << (16 if abi >= 5 else 15)) - 1
    ruleset = ctypes.c_uint64(rights)
    descriptor = libc.syscall(444, ctypes.byref(ruleset), ctypes.sizeof(ruleset), 0)
    if descriptor < 0:
        raise SandboxUnavailable(f"Landlock ruleset: errno={ctypes.get_errno()}")
    try:
        for path, access in [(path, (1 << 2) | (1 << 3)) for path in read_paths] + [
            (output, rights & ~1)
        ]:
            fd = os.open(path, os.O_PATH | os.O_CLOEXEC)
            try:
                rule = _PathRule(access, fd)
                if libc.syscall(445, descriptor, 1, ctypes.byref(rule), 0):
                    raise SandboxUnavailable(f"Landlock add rule: errno={ctypes.get_errno()}")
            finally:
                os.close(fd)
        if libc.prctl(38, 1, 0, 0, 0) or libc.syscall(446, descriptor, 0):
            raise SandboxUnavailable(f"Landlock restrict: errno={ctypes.get_errno()}")
    finally:
        os.close(descriptor)


class _Comparison(ctypes.Structure):
    _fields_ = [
        ("arg", ctypes.c_uint),
        ("op", ctypes.c_int),
        ("a", ctypes.c_uint64),
        ("b", ctypes.c_uint64),
    ]


def load_seccomp(library_paths: list[str]):
    for directory in library_paths:
        path = Path(directory) / "libseccomp.so.2"
        if path.exists():
            library = ctypes.CDLL(str(path))
            break
    else:
        raise SandboxUnavailable("libseccomp.so.2 missing; run python -m bake.setup")
    library.seccomp_init.argtypes = [ctypes.c_uint32]
    library.seccomp_init.restype = ctypes.c_void_p
    library.seccomp_rule_add_array.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint32,
        ctypes.c_int,
        ctypes.c_uint,
        ctypes.POINTER(_Comparison),
    ]
    library.seccomp_syscall_resolve_name.argtypes = [ctypes.c_char_p]
    library.seccomp_load.argtypes = [ctypes.c_void_p]
    library.seccomp_release.argtypes = [ctypes.c_void_p]
    return library


def restrict_syscalls(library):
    context = library.seccomp_init(0x7FFF0000)  # SCMP_ACT_ALLOW
    if not context:
        raise SandboxUnavailable("Cannot allocate seccomp context")
    try:
        denied = [
            "socket",
            "socketpair",
            "connect",
            "bind",
            "listen",
            "accept",
            "accept4",
            "fork",
            "vfork",
            "execve",
            "execveat",
            "ptrace",
            "process_vm_readv",
            "process_vm_writev",
            "open_by_handle_at",
            "mount",
            "umount2",
            "unshare",
            "setns",
            "userfaultfd",
            "bpf",
            "io_uring_setup",
            "kill",
            "tkill",
        ]
        for name in denied:
            number = library.seccomp_syscall_resolve_name(name.encode())
            if number >= 0 and library.seccomp_rule_add_array(
                context, 0x50000 | errno.EPERM, number, 0, None
            ):
                raise SandboxUnavailable(f"Cannot filter {name}")
        # glibc falls back to clone for threads when clone3 returns ENOSYS.
        clone3 = library.seccomp_syscall_resolve_name(b"clone3")
        if clone3 >= 0:
            if library.seccomp_rule_add_array(context, 0x50000 | errno.ENOSYS, clone3, 0, None):
                raise SandboxUnavailable("Cannot filter clone3")
        comparison = _Comparison(0, 7, 0x10000, 0)  # MASKED_EQ: deny non-thread clone
        if library.seccomp_rule_add_array(
            context,
            0x50000 | errno.EPERM,
            library.seccomp_syscall_resolve_name(b"clone"),
            1,
            ctypes.byref(comparison),
        ):
            raise SandboxUnavailable("Cannot filter process clone")
        if library.seccomp_load(context):
            raise SandboxUnavailable("Cannot install seccomp filter")
    finally:
        library.seccomp_release(context)


def restrict_resources(config: dict):
    memory = config["BAKE_MB"] * 1024 * 1024
    resource.setrlimit(resource.RLIMIT_AS, (memory, memory))
    seconds = max(1, int(config["BAKE_S"]))
    resource.setrlimit(resource.RLIMIT_CPU, (seconds, seconds + 1))
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def cgroup_probe(root: Path | None) -> dict:
    if root is None:
        return {"available": False, "error": "No delegated cgroup directory configured"}
    probe = None
    try:
        probe = Path(tempfile.mkdtemp(prefix="bake-probe-", dir=root))
        (probe / "memory.max").write_text("2147483648")
        return {"available": True}
    except OSError as error:
        return {
            "available": False,
            "error": f"{type(error).__name__}: errno={error.errno}: {error.strerror}",
        }
    finally:
        if probe is not None:
            probe.rmdir()
