#!/usr/bin/env python3
"""
verify.py - run the ORIGINAL PARA.COM on a real 8086 CPU emulator (Unicorn)
with the same BIOS code (libbios_harness.so) and the same scripted input
as build/paratrooper_test, and print the same trace format.

Usage: python3 verify.py PARA.COM libbios_harness.so MAX_INTS > reference.txt
Then diff the output against `paratrooper_test MAX_INTS`.
"""
import sys, ctypes
from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_INTR, UC_HOOK_INSN, UcError
from unicorn.x86_const import *

PSP = 0x0800
com = open(sys.argv[1], 'rb').read()
lib = ctypes.CDLL(sys.argv[2])
max_ints = int(sys.argv[3])
lib.harness_set_max(ctypes.c_ulong(max_ints))


class Cpu(ctypes.Structure):
    _fields_ = [('ax', ctypes.c_uint16), ('bx', ctypes.c_uint16), ('cx', ctypes.c_uint16),
                ('dx', ctypes.c_uint16), ('si', ctypes.c_uint16), ('di', ctypes.c_uint16),
                ('bp', ctypes.c_uint16), ('sp', ctypes.c_uint16), ('cs', ctypes.c_uint16),
                ('ds', ctypes.c_uint16), ('es', ctypes.c_uint16), ('ss', ctypes.c_uint16),
                ('ip', ctypes.c_uint16), ('cf', ctypes.c_uint8), ('zf', ctypes.c_uint8),
                ('sf', ctypes.c_uint8), ('of', ctypes.c_uint8), ('df', ctypes.c_uint8)]


cpu = Cpu.in_dll(lib, 'r')
cmem = (ctypes.c_uint8 * 0x100000).in_dll(lib, 'mem')
lib.machine_reset()           # same BIOS data area / video state as the C build

uc = Uc(UC_ARCH_X86, UC_MODE_16)
uc.mem_map(0, 0x100000)
uc.mem_write(0, bytes(cmem))  # BIOS data area + loaded program from machine_reset

REGS = [('ax', UC_X86_REG_AX), ('bx', UC_X86_REG_BX), ('cx', UC_X86_REG_CX), ('dx', UC_X86_REG_DX),
        ('si', UC_X86_REG_SI), ('di', UC_X86_REG_DI), ('bp', UC_X86_REG_BP), ('sp', UC_X86_REG_SP),
        ('cs', UC_X86_REG_CS), ('ds', UC_X86_REG_DS), ('es', UC_X86_REG_ES), ('ss', UC_X86_REG_SS)]

DATA = (PSP + 0x11) * 16


def copy_to_c(u):
    for name, rid in REGS:
        setattr(cpu, name, u.reg_read(rid))
    f = u.reg_read(UC_X86_REG_EFLAGS)
    cpu.cf, cpu.zf, cpu.sf, cpu.of, cpu.df = f & 1, (f >> 6) & 1, (f >> 7) & 1, (f >> 11) & 1, (f >> 10) & 1
    for start, length in ((DATA, 0x10000), (0xB8000, 0x4000)):
        ctypes.memmove(ctypes.addressof(cmem) + start, bytes(u.mem_read(start, length)), length)


def copy_from_c(u):
    for name, rid in REGS:
        if name not in ('cs', 'ss'):
            u.reg_write(rid, getattr(cpu, name))
    f = u.reg_read(UC_X86_REG_EFLAGS)
    f &= ~((1 << 0) | (1 << 6) | (1 << 7) | (1 << 11) | (1 << 10))
    f |= cpu.cf | cpu.zf << 6 | cpu.sf << 7 | cpu.of << 11 | cpu.df << 10
    u.reg_write(UC_X86_REG_EFLAGS, f)
    u.mem_write(0xB8000, bytes(cmem[0xB8000:0xB8000 + 0x4000]))


def on_int(u, intno, user):
    copy_to_c(u)
    lib.bios_int(ctypes.c_uint8(intno))   # exits the process after MAX_INTS
    copy_from_c(u)


def on_in(u, port, size, user):
    return lib.port_in(ctypes.c_uint16(port)) & 0xFF


def on_out(u, port, size, value, user):
    lib.port_out(ctypes.c_uint16(port), ctypes.c_uint8(value & 0xFF))


uc.hook_add(UC_HOOK_INTR, on_int)
uc.hook_add(UC_HOOK_INSN, on_in, None, 1, 0, UC_X86_INS_IN)
uc.hook_add(UC_HOOK_INSN, on_out, None, 1, 0, UC_X86_INS_OUT)

# Start at the real .COM entry (PSP:0100), including the original start stub.
for name, rid in REGS:
    if name in ('cs', 'ds', 'es', 'ss'):
        uc.reg_write(rid, PSP)
uc.reg_write(UC_X86_REG_SP, 0xFFFE)
uc.reg_write(UC_X86_REG_AX, 0)
try:
    uc.emu_start(PSP * 16 + 0x100, 0xFFFFF)
except UcError as e:
    print("UNICORN ERROR", e, hex(uc.reg_read(UC_X86_REG_IP)), hex(uc.reg_read(UC_X86_REG_CS)))
