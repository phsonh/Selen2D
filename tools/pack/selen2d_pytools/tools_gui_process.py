"""Owned, cancellable Windows build process tree for the development GUI.

The helper blocks on an stdin gate before launching any compiler. The GUI first
assigns it to its private kill-on-close job, so cancellation never targets a
user's terminal or an unrelated build. No secrets are passed on a command line.
"""
from __future__ import annotations
import os
import subprocess
import sys


class BuildJob:
    def __init__(self, pid):
        if os.name != 'nt':
            raise RuntimeError('The build action currently supports Windows only')
        import ctypes
        from ctypes import wintypes as w
        self.ctypes = ctypes
        self.kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        class Limits(ctypes.Structure):
            _fields_ = [('ProcessTime', ctypes.c_int64), ('JobTime', ctypes.c_int64),
                        ('Flags', w.DWORD), ('Min', ctypes.c_size_t), ('Max', ctypes.c_size_t),
                        ('Active', w.DWORD), ('Affinity', ctypes.c_size_t),
                        ('Priority', w.DWORD), ('Scheduling', w.DWORD)]
        class Counters(ctypes.Structure):
            _fields_ = [(name, ctypes.c_uint64) for name in
                        ('ReadCount', 'WriteCount', 'OtherCount', 'ReadBytes', 'WriteBytes', 'OtherBytes')]
        class Extended(ctypes.Structure):
            _fields_ = [('Limits', Limits), ('IO', Counters), ('ProcessMemory', ctypes.c_size_t),
                        ('JobMemory', ctypes.c_size_t), ('PeakProcess', ctypes.c_size_t),
                        ('PeakJob', ctypes.c_size_t)]
        api = self.kernel
        api.CreateJobObjectW.argtypes = [w.LPVOID, w.LPCWSTR]
        api.CreateJobObjectW.restype = w.HANDLE
        api.SetInformationJobObject.argtypes = [w.HANDLE, ctypes.c_int, w.LPVOID, w.DWORD]
        api.SetInformationJobObject.restype = w.BOOL
        api.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
        api.OpenProcess.restype = w.HANDLE
        api.AssignProcessToJobObject.argtypes = [w.HANDLE, w.HANDLE]
        api.AssignProcessToJobObject.restype = w.BOOL
        api.TerminateJobObject.argtypes = [w.HANDLE, w.UINT]
        api.TerminateJobObject.restype = w.BOOL
        api.CloseHandle.argtypes = [w.HANDLE]
        api.CloseHandle.restype = w.BOOL
        self.handle = api.CreateJobObjectW(None, None)
        if not self.handle:
            raise ctypes.WinError(ctypes.get_last_error())
        process = None
        try:
            info = Extended()
            info.Limits.Flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
            if not api.SetInformationJobObject(self.handle, 9, ctypes.byref(info), ctypes.sizeof(info)):
                raise ctypes.WinError(ctypes.get_last_error())
            process = api.OpenProcess(0x0100 | 0x0001, False, int(pid))
            if not process or not api.AssignProcessToJobObject(self.handle, process):
                raise ctypes.WinError(ctypes.get_last_error())
        except BaseException:
            self.close()
            raise
        finally:
            if process:
                api.CloseHandle(process)

    def cancel(self):
        if self.handle and not self.kernel.TerminateJobObject(self.handle, 130):
            raise self.ctypes.WinError(self.ctypes.get_last_error())

    def close(self):
        if self.handle:
            self.kernel.CloseHandle(self.handle)
            self.handle = None


def main():
    if len(sys.argv) < 3 or sys.argv[1] != '--worker':
        return 2
    if sys.stdin.buffer.readline() != b'go\n':
        return 130
    # Parent owns our job before this point. Children inherit the same job.
    child = subprocess.Popen(sys.argv[2:], stdin=subprocess.DEVNULL,
                             stdout=sys.stdout, stderr=sys.stderr,
                             creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    return child.wait()


if __name__ == '__main__':
    raise SystemExit(main())
