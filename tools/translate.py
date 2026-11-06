#!/usr/bin/env python3
"""
translate.py - static translator from the Paratrooper (1982) .COM binary to C.

Usage:  python3 translate.py PARA.COM OUT_DIR

What it does:
  1. Finds every reachable instruction in the code segment by following
     control flow (recursive descent), including the two jump tables.
  2. Emits one C statement group per 8086 instruction into game.c,
     using the helpers in cpu.h. Each instruction gets a label, so jumps
     become `goto`, and calls/returns use the emulated stack.
  3. Emits the program image (data + code bytes) into image.c, so the
     final program does not need the original .COM file at runtime.

Layout of the .COM file (loaded at PSP:0100):
  0x0000-0x000F  start stub: jumps to segment PSP+0x2C4, offset 0
  0x0010-0x2B3F  data segment (DS = PSP+0x11, so DS:0 = file offset 0x10)
  0x2B40-end     code segment (CS = PSP+0x2C4, CS:0 = file offset 0x2B40)
"""
import sys, os
import capstone
from capstone import x86

CODE_FILE_OFFSET = 0x2B40
DATA_FILE_OFFSET = 0x10

# Jump tables found by hand (DS offsets, entry count) and their targets.
#   jmp [bx+0x1ac7]  bx = 0..7 * 2       (round state machine)
#   jmp [si+0x2159]  si = 1..8 * 2       (sound effect handlers)
JUMP_TABLES = [(0x1AC7, 0, 8), (0x2159, 1, 9)]

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)
md.detail = True


def u16(b, o):
    return b[o] | (b[o + 1] << 8)


def discover(code, data):
    targets = [0]
    for base, first, count in JUMP_TABLES:
        for k in range(first, count):
            targets.append(u16(data, base + 2 * k))
    seen = {}
    work = list(targets)
    while work:
        a = work.pop()
        while a not in seen and a < len(code):
            ins = next(md.disasm(code[a:a + 16], a), None)
            if ins is None:
                raise SystemExit(f"cannot decode at {a:#x}")
            seen[a] = ins
            if ins.group(capstone.CS_GRP_JUMP) or ins.group(capstone.CS_GRP_CALL):
                op = ins.operands[0]
                if op.type == x86.X86_OP_IMM:
                    work.append(op.imm & 0xFFFF)
            if ins.mnemonic in ('jmp', 'ret', 'retf', 'iret'):
                break
            a += ins.size
    return seen, set(targets)


REG = {}
for n in ['ax', 'bx', 'cx', 'dx', 'al', 'ah', 'bl', 'bh', 'cl', 'ch', 'dl', 'dh',
          'si', 'di', 'bp', 'sp', 'cs', 'ds', 'es', 'ss']:
    REG[n] = n.upper()


class Gen:
    def __init__(self, ins):
        self.ins = ins

    def reg(self, rid):
        return REG[self.ins.reg_name(rid)]

    def ea(self, op):
        m = op.mem
        parts = []
        if m.base:
            parts.append(self.reg(m.base))
        if m.index:
            parts.append(self.reg(m.index))
        if m.disp or not parts:
            parts.append(f"0x{m.disp & 0xFFFF:04X}")
        if m.segment:
            seg = self.reg(m.segment)
        elif m.base and self.ins.reg_name(m.base) == 'bp':
            seg = 'SS'
        else:
            seg = 'DS'
        return seg, "(uint16_t)(" + " + ".join(parts) + ")"

    def rd(self, op, size=None):
        size = size or op.size
        if op.type == x86.X86_OP_REG:
            return self.reg(op.reg)
        if op.type == x86.X86_OP_IMM:
            return f"0x{op.imm & (0xFF if size == 1 else 0xFFFF):0{2 * size}X}"
        seg, off = self.ea(op)
        return f"{'rb' if size == 1 else 'rw'}({seg}, {off})"

    def wr(self, op, expr):
        if op.type == x86.X86_OP_REG:
            return f"{self.reg(op.reg)} = {expr};"
        seg, off = self.ea(op)
        return f"{'wb' if op.size == 1 else 'ww'}({seg}, {off}, {expr});"


def label(a):
    return f"L_{a:04X}"


JCC = {
    'je': 'ZF', 'jne': '!ZF', 'jb': 'CF', 'jae': '!CF', 'jbe': '(CF || ZF)',
    'ja': '(!CF && !ZF)', 'jl': '(SF != OF)', 'jge': '(SF == OF)',
    'jle': '(ZF || SF != OF)', 'jg': '(!ZF && SF == OF)', 'js': 'SF', 'jns': '!SF',
    'jo': 'OF', 'jno': '!OF',
}
ALU = {'add': 'add', 'sub': 'sub', 'cmp': 'sub', 'and': '&', 'or': '|', 'xor': '^', 'test': '&'}
SHIFT = {'shl': 'shl', 'sal': 'shl', 'shr': 'shr', 'sar': 'sar', 'rol': 'rol', 'ror': 'ror'}


def mem_ops(ins):
    return sum(1 for o in ins.operands if o.type == x86.X86_OP_MEM)


def base_cycles(ins):
    """Rough 4.77 MHz 8088 cycle cost, good enough for pacing busy loops."""
    m = ins.mnemonic
    memn = mem_ops(ins)
    word = any(o.size == 2 for o in ins.operands)
    ea = 9 * memn + (4 if (memn and word) else 0)
    if m == 'mov':
        return (10 if memn else 2 if ins.operands[1].type == x86.X86_OP_REG else 4) + ea
    if m in ALU:
        return (16 if memn and ins.operands[0].type == x86.X86_OP_MEM and m not in ('cmp', 'test') else 4) + ea
    if m in ('inc', 'dec', 'neg', 'not'):
        return (15 if memn else 3) + ea
    if m in SHIFT:
        return (15 if memn else 2) + ea
    if m in ('push',):
        return 15
    if m in ('pop',):
        return 12
    if m in ('mul', 'imul'):
        return 75 if ins.operands[0].size == 1 else 125
    if m in ('div', 'idiv'):
        return 105 if ins.operands[0].size == 1 else 175
    if m in ('in', 'out'):
        return 10
    if m == 'xchg':
        return 4 + ea
    return 4


def translate_one(ins, addrs, next_addr):
    g = Gen(ins)
    m = ins.mnemonic
    ops = ins.operands
    raw = bytes(ins.bytes)
    out = []
    E = out.append

    # Strip prefixes to find the real opcode (rep / segment override).
    i = 0
    rep = None
    segov = None
    while raw[i] in (0xF2, 0xF3, 0x26, 0x2E, 0x36, 0x3E):
        if raw[i] in (0xF2, 0xF3):
            rep = raw[i]
        else:
            segov = {0x26: 'ES', 0x2E: 'CS', 0x36: 'SS', 0x3E: 'DS'}[raw[i]]
        i += 1
    opc = raw[i]

    def goto(t):
        t &= 0xFFFF
        if t in addrs:
            return f"goto {label(t)};"
        return f"r.ip = 0x{t:04X}; goto dispatch;"

    # ---- string instructions ----
    if 0xA4 <= opc <= 0xAF and opc not in (0xA8, 0xA9):
        w = opc & 1
        sz = 2 if w else 1
        step = f"(DF ? -{sz} : {sz})"
        src = segov or 'DS'
        rdm, wrm = ('rw', 'ww') if w else ('rb', 'wb')
        acc = 'AX' if w else 'AL'
        subf = 'sub16' if w else 'sub8'
        kind = opc & 0xFE
        if kind == 0xA4:   # movs
            body = f"{wrm}(ES, DI, {rdm}({src}, SI)); SI += {step}; DI += {step};"
            cyc = 26 if w else 18
        elif kind == 0xA6:  # cmps
            body = f"{subf}({rdm}({src}, SI), {rdm}(ES, DI)); SI += {step}; DI += {step};"
            cyc = 30 if w else 22
        elif kind == 0xAA:  # stos
            body = f"{wrm}(ES, DI, {acc}); DI += {step};"
            cyc = 15 if w else 11
        elif kind == 0xAC:  # lods
            body = f"{acc} = {rdm}({src}, SI); SI += {step};"
            cyc = 16 if w else 12
        else:              # scas
            body = f"{subf}({acc}, {rdm}(ES, DI)); DI += {step};"
            cyc = 19 if w else 15
        if rep is None:
            E(f"CYC({cyc}); {body}")
        else:
            cond = ''
            if kind in (0xA6, 0xAE):
                cond = ' if (!ZF) break;' if rep == 0xF3 else ' if (ZF) break;'
            E(f"CYC(9); while (CX) {{ {body} CX--; CYC({cyc - 6});{cond} }}")
        return out, False

    if opc == 0x98:   # cbw (capstone prints cwde)
        E("CYC(2); AX = (uint16_t)(int16_t)(int8_t)AL;")
        return out, False
    if opc == 0x99:   # cwd
        E("CYC(5); DX = (AX & 0x8000) ? 0xFFFF : 0;")
        return out, False

    cyc = base_cycles(ins)

    if m == 'mov':
        E(f"CYC({cyc}); {g.wr(ops[0], g.rd(ops[1], ops[0].size))}")
    elif m in ALU:
        sz = ops[0].size
        a, b = g.rd(ops[0]), g.rd(ops[1], sz)
        bits = 8 if sz == 1 else 16
        f = ALU[m]
        if f in ('add', 'sub'):
            expr = f"{f}{bits}({a}, {b})"
        else:
            expr = f"logic{bits}((uint{bits}_t)({a} {f} {b}))"
        if m in ('cmp', 'test'):
            E(f"CYC({cyc}); {expr};")
        else:
            E(f"CYC({cyc}); {g.wr(ops[0], expr)}")
    elif m in ('inc', 'dec', 'neg'):
        bits = 8 if ops[0].size == 1 else 16
        E(f"CYC({cyc}); {g.wr(ops[0], f'{m}{bits}({g.rd(ops[0])})')}")
    elif m == 'not':
        bits = 8 if ops[0].size == 1 else 16
        E(f"CYC({cyc}); {g.wr(ops[0], f'(uint{bits}_t)~{g.rd(ops[0])}')}")
    elif m in SHIFT:
        bits = 8 if ops[0].size == 1 else 16
        cnt = g.rd(ops[1], 1) if len(ops) > 1 else '1'
        if len(ops) > 1 and ops[1].type == x86.X86_OP_REG:
            cyc += 6  # by CL: plus 4 per bit, added at runtime
            E(f"CYC({cyc} + 4 * CL);")
        else:
            E(f"CYC({cyc});")
        E(g.wr(ops[0], f"{SHIFT[m]}{bits}({g.rd(ops[0])}, {cnt})"))
    elif m in ('mul', 'idiv'):
        bits = 8 if ops[0].size == 1 else 16
        E(f"CYC({cyc}); {m}{bits}({g.rd(ops[0])});")
    elif m == 'xchg':
        E(f"CYC({cyc}); {{ uint16_t t = {g.rd(ops[0])}; {g.wr(ops[0], g.rd(ops[1]))} {g.wr(ops[1], 't')} }}")
    elif m == 'push':
        E(f"CYC({cyc}); push16({g.rd(ops[0])});")
    elif m == 'pop':
        E(f"CYC({cyc}); {g.wr(ops[0], 'pop16()')}")
    elif m in ('cld', 'std'):
        E(f"CYC(2); DF = {1 if m == 'std' else 0};")
    elif m in ('cli', 'sti', 'nop'):
        E("CYC(3);")
    elif m == 'int':
        E(f"CYC(51); bios_int(0x{ops[0].imm:02X});")
    elif m == 'in':
        port = g.rd(ops[1], 2) if ops[1].type == x86.X86_OP_IMM else 'DX'
        E(f"CYC({cyc}); AL = port_in({port});")
    elif m == 'out':
        port = g.rd(ops[0], 2) if ops[0].type == x86.X86_OP_IMM else 'DX'
        E(f"CYC({cyc}); port_out({port}, AL);")
    elif m in JCC:
        t = ops[0].imm
        E(f"CYC(4); if ({JCC[m]}) {{ CYC(12); {goto(t)} }}")
    elif m == 'jcxz':
        E(f"CYC(6); if (CX == 0) {{ CYC(12); {goto(ops[0].imm)} }}")
    elif m in ('loop', 'loopne', 'loope'):
        cond = {'loop': '', 'loopne': ' && !ZF', 'loope': ' && ZF'}[m]
        E(f"CYC(5); if (--CX{cond}) {{ CYC(12); {goto(ops[0].imm)} }}")
    elif m == 'jmp':
        if ops[0].type == x86.X86_OP_IMM:
            E(f"CYC(15); {goto(ops[0].imm)}")
        else:
            E(f"CYC(18); r.ip = {g.rd(ops[0])}; goto dispatch;")
        return out, True
    elif m == 'call':
        if ops[0].type != x86.X86_OP_IMM:
            raise SystemExit(f"indirect call at {ins.address:#x}")
        E(f"CYC(23); push16(0x{next_addr:04X}); {goto(ops[0].imm)}")
        return out, True
    elif m == 'ret':
        if ops:
            raise SystemExit(f"ret imm at {ins.address:#x}")
        E("CYC(20); r.ip = pop16(); goto dispatch;")
        return out, True
    else:
        raise SystemExit(f"unsupported instruction at {ins.address:#x}: {m} {ins.op_str}")
    return out, False


def main():
    com = open(sys.argv[1], 'rb').read()
    outdir = sys.argv[2]
    code = com[CODE_FILE_OFFSET:]
    data = com[DATA_FILE_OFFSET:]
    seen, entries = discover(code, data)
    addrs = sorted(seen)
    aset = set(addrs)
    # Every translated instruction can be reached through `dispatch`, so an
    # indirect jump to any known code address works.
    dispatch_targets = set(addrs)

    lines = []
    lines.append("/* game.c - GENERATED by tools/translate.py from the original PARA.COM.")
    lines.append(" * Do not edit by hand: re-run the translator instead.")
    lines.append(" * Each block is one original 8086 instruction (code segment offset). */")
    lines.append('#include "cpu.h"')
    lines.append("")
    lines.append("void game_run(uint16_t entry)")
    lines.append("{")
    lines.append("    r.ip = entry;")
    lines.append("dispatch:")
    lines.append("    switch (r.ip) {")
    for a in sorted(dispatch_targets):
        lines.append(f"    case 0x{a:04X}: goto {label(a)};")
    lines.append('    default: machine_fatal("jump to untranslated address", r.ip); return;')
    lines.append("    }")
    for idx, a in enumerate(addrs):
        ins = seen[a]
        nxt = a + ins.size
        stmts, terminal = translate_one(ins, aset, nxt)
        lines.append(f"{label(a)}: /* {a:04X}: {ins.mnemonic} {ins.op_str} */")
        for s in stmts:
            lines.append("    " + s)
        following = addrs[idx + 1] if idx + 1 < len(addrs) else None
        if not terminal and following != nxt:
            if nxt in aset:
                lines.append(f"    goto {label(nxt)};")
            else:
                lines.append(f"    r.ip = 0x{nxt:04X}; goto dispatch;")
    lines.append("}")
    open(os.path.join(outdir, 'game.c'), 'w').write("\n".join(lines) + "\n")

    img = []
    img.append("/* image.c - GENERATED by tools/translate.py: the original program image")
    img.append(" * (loaded at PSP:0100), containing the game's graphics, text and tables. */")
    img.append("#include <stdint.h>")
    img.append(f"const unsigned image_size = {len(com)};")
    img.append("const uint8_t image[] = {")
    for k in range(0, len(com), 16):
        img.append("    " + ", ".join(f"0x{b:02X}" for b in com[k:k + 16]) + ",")
    img.append("};")
    open(os.path.join(outdir, 'image.c'), 'w').write("\n".join(img) + "\n")
    print(f"translated {len(addrs)} instructions, {len(dispatch_targets)} dispatch targets")


if __name__ == '__main__':
    main()
