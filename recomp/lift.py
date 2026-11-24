#!/usr/bin/env python3
"""
Static Recompiler (x86-32 PE -> Native C) for Moorhuhn Adventure 2: Der Fluch des Goldes (mha2.exe).
Uses Capstone disassembly + REA/Ghidra function inventory + recursive CFG/vtable/switch recovery.
"""
import os
import sys
import json
import struct
import argparse
import collections
import capstone
from capstone import x86 as cs_x86

EXE_PATH = "game_files/mha2.exe"
JOURNAL_PATH = "work/decompiled/decompile-journal.jsonl"
OUT_DIR = "recomp/gen"
NUM_CHUNKS = 32

# Register mapping tables
REG32_MAP = {
    cs_x86.X86_REG_EAX: "c->eax",
    cs_x86.X86_REG_ECX: "c->ecx",
    cs_x86.X86_REG_EDX: "c->edx",
    cs_x86.X86_REG_EBX: "c->ebx",
    cs_x86.X86_REG_ESP: "c->esp",
    cs_x86.X86_REG_EBP: "c->ebp",
    cs_x86.X86_REG_ESI: "c->esi",
    cs_x86.X86_REG_EDI: "c->edi",
}
REG16_MAP = {
    cs_x86.X86_REG_AX: "c->eax",
    cs_x86.X86_REG_CX: "c->ecx",
    cs_x86.X86_REG_DX: "c->edx",
    cs_x86.X86_REG_BX: "c->ebx",
    cs_x86.X86_REG_SP: "c->esp",
    cs_x86.X86_REG_BP: "c->ebp",
    cs_x86.X86_REG_SI: "c->esi",
    cs_x86.X86_REG_DI: "c->edi",
}
REG8_LO_MAP = {
    cs_x86.X86_REG_AL: "c->eax",
    cs_x86.X86_REG_CL: "c->ecx",
    cs_x86.X86_REG_DL: "c->edx",
    cs_x86.X86_REG_BL: "c->ebx",
}
REG8_HI_MAP = {
    cs_x86.X86_REG_AH: "c->eax",
    cs_x86.X86_REG_CH: "c->ecx",
    cs_x86.X86_REG_DH: "c->edx",
    cs_x86.X86_REG_BH: "c->ebx",
}
ST_REGS = {
    cs_x86.X86_REG_ST0: 0,
    cs_x86.X86_REG_ST1: 1,
    cs_x86.X86_REG_ST2: 2,
    cs_x86.X86_REG_ST3: 3,
    cs_x86.X86_REG_ST4: 4,
    cs_x86.X86_REG_ST5: 5,
    cs_x86.X86_REG_ST6: 6,
    cs_x86.X86_REG_ST7: 7,
}

JCC_COND = {
    "je":  "(c->zf)",
    "jz":  "(c->zf)",
    "jne": "(!c->zf)",
    "jnz": "(!c->zf)",
    "jb":  "(c->cf)",
    "jc":  "(c->cf)",
    "jnae":"(c->cf)",
    "jae": "(!c->cf)",
    "jnb": "(!c->cf)",
    "jnc": "(!c->cf)",
    "jbe": "(c->cf || c->zf)",
    "jna": "(c->cf || c->zf)",
    "ja":  "(!c->cf && !c->zf)",
    "jnbe":"(!c->cf && !c->zf)",
    "jl":  "(c->sf != c->of)",
    "jnge":"(c->sf != c->of)",
    "jge": "(c->sf == c->of)",
    "jnl": "(c->sf == c->of)",
    "jle": "(c->zf || (c->sf != c->of))",
    "jng": "(c->zf || (c->sf != c->of))",
    "jg":  "(!c->zf && (c->sf == c->of))",
    "jnle":"(!c->zf && (c->sf == c->of))",
    "js":  "(c->sf)",
    "jns": "(!c->sf)",
    "jo":  "(c->of)",
    "jno": "(!c->of)",
    "jp":  "(c->pf)",
    "jpe": "(c->pf)",
    "jnp": "(!c->pf)",
    "jpo": "(!c->pf)",
    "jecxz": "(c->ecx == 0)",
    "jcxz":  "((c->ecx & 0xFFFFu) == 0)",
}

SETCC_COND = {
    "sete":  "(c->zf)",
    "setz":  "(c->zf)",
    "setne": "(!c->zf)",
    "setnz": "(!c->zf)",
    "setb":  "(c->cf)",
    "setc":  "(c->cf)",
    "setae": "(!c->cf)",
    "setnb": "(!c->cf)",
    "setbe": "(c->cf || c->zf)",
    "setna": "(c->cf || c->zf)",
    "seta":  "(!c->cf && !c->zf)",
    "setnbe":"(!c->cf && !c->zf)",
    "setl":  "(c->sf != c->of)",
    "setge": "(c->sf == c->of)",
    "setle": "(c->zf || (c->sf != c->of))",
    "setg":  "(!c->zf && (c->sf == c->of))",
    "sets":  "(c->sf)",
    "setns": "(!c->sf)",
    "seto":  "(c->of)",
    "setno": "(!c->of)",
    "setp":  "(c->pf)",
    "setnp": "(!c->pf)",
}

INVALID_START_MNEMONICS = {
    "int", "int3", "hlt", "in", "out", "insb", "insw", "insd", "outsb", "outsw", "outsd",
    "arpl", "bound", "daa", "das", "aaa", "aas", "aam", "aad", "into",
    "iret", "iretd", "lds", "les", "salc", "icebp", "ud2", "cli", "sti",
    "lgdt", "lidt", "lldt", "ltr", "sgdt", "sidt", "sldt", "str", "invd",
    "wbinvd", "invlpg", "wrmsr", "rdmsr", "rdpmc", "lmsw", "smsw",
}


class PEImage:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.raw = f.read()
        e_lfanew = struct.unpack_from("<I", self.raw, 0x3c)[0]
        n_sec = struct.unpack_from("<H", self.raw, e_lfanew + 6)[0]
        opt_size = struct.unpack_from("<H", self.raw, e_lfanew + 20)[0]
        self.image_base = struct.unpack_from("<I", self.raw, e_lfanew + 24 + 28)[0]
        self.entry_va = self.image_base + struct.unpack_from("<I", self.raw, e_lfanew + 24 + 16)[0]
        self.sections = []
        sec_off = e_lfanew + 24 + opt_size
        for i in range(n_sec):
            s = self.raw[sec_off + i * 40 : sec_off + (i + 1) * 40]
            name = s[:8].rstrip(b"\x00").decode("latin1")
            vsize, vaddr, rsize, roff = struct.unpack_from("<IIII", s, 8)
            self.sections.append((name, self.image_base + vaddr, vsize, roff, rsize))
            if name == ".text":
                self.text_va = self.image_base + vaddr
                self.text_end = self.text_va + vsize
                self.text_bytes = self.raw[roff : roff + vsize]
            elif name == ".rdata":
                self.rdata_va = self.image_base + vaddr
                self.rdata_bytes = self.raw[roff : roff + min(vsize, rsize)]
            elif name == ".data":
                self.data_va = self.image_base + vaddr
                self.data_bytes = self.raw[roff : roff + min(vsize, rsize)]

        # Parse imports
        imp_rva, _ = struct.unpack_from("<II", self.raw, e_lfanew + 24 + 96 + 8)
        self.imports = []  # (import_id, iat_va, dll, name)
        self.iat_map = {}  # iat_va -> (import_id, dll, name)
        off = self.rva2off(imp_rva)
        imp_id = 0
        while off is not None:
            ilt, ts, fwd, name_rva, iat = struct.unpack_from("<IIIII", self.raw, off)
            if not (ilt or name_rva or iat):
                break
            noff = self.rva2off(name_rva)
            dll = self.raw[noff : self.raw.find(b"\x00", noff)].decode("latin1")
            toff = self.rva2off(ilt or iat)
            idx = 0
            while toff is not None:
                val = struct.unpack_from("<I", self.raw, toff)[0]
                if val == 0:
                    break
                iat_va = self.image_base + iat + idx * 4
                if val & 0x80000000:
                    fname = f"Ordinal_{val & 0xffff}"
                else:
                    foff = self.rva2off(val)
                    fname = self.raw[foff + 2 : self.raw.find(b"\x00", foff + 2)].decode("latin1")
                self.imports.append((imp_id, iat_va, dll, fname))
                self.iat_map[iat_va] = (imp_id, dll, fname)
                imp_id += 1
                toff += 4
                idx += 1
            off += 20

    def rva2off(self, rva):
        va = self.image_base + rva
        return self.va2off(va)

    def va2off(self, va):
        for _, sva, vsz, roff, rsz in self.sections:
            if sva <= va < sva + rsz:
                return roff + (va - sva)
        return None

    def read_u32(self, va):
        off = self.va2off(va)
        if off is None or off + 4 > len(self.raw):
            return None
        return struct.unpack_from("<I", self.raw, off)[0]


def make_cs():
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    return md


def is_valid_code_start(pe, md, va):
    if not (pe.text_va <= va < pe.text_end):
        return False
    off = va - pe.text_va
    b = pe.text_bytes[off]
    if b in (0xcc, 0x00):
        return False
    insns = list(md.disasm(pe.text_bytes[off : off + 16], va, count=1))
    if not insns:
        return False
    return insns[0].mnemonic not in INVALID_START_MNEMONICS


def recover_switch_targets(pe, md, table_va, func_va, max_entries=256):
    def is_tgt(v):
        if v is None:
            return False
        if not (pe.text_va <= v < pe.text_end):
            return False
        if v < func_va - 0x400 or v > func_va + 0x4000:
            return False
        return is_valid_code_start(pe, md, v)

    targets = []
    # Forward scan starting at i=0, also allow 1-based tables (dummy entry at i=0, valid at i=1)
    start_i = 0
    v0 = pe.read_u32(table_va)
    if not is_tgt(v0):
        v1 = pe.read_u32(table_va + 4)
        if is_tgt(v1):
            start_i = 1
        else:
            start_i = None
    if start_i is not None:
        cur = table_va + start_i * 4
        for _ in range(max_entries):
            v = pe.read_u32(cur)
            if not is_tgt(v):
                break
            targets.append(v)
            cur += 4
    # Also scan negative indices (i = -1, -2, ...) for MSVC memcpy/memmove negative-indexed tables
    cur = table_va - 4
    for _ in range(16):
        v = pe.read_u32(cur)
        if not is_tgt(v):
            break
        targets.append(v)
        cur -= 4
    return targets


def scan_msvc_eh_tables(pe, md):
    """Scan .rdata for MSVC C++ EH FuncInfo (magic 0x19930520) unwind/catch handlers."""
    found = set()
    rdata = pe.rdata_bytes
    for off in range(0, len(rdata) - 24, 4):
        magic = struct.unpack_from("<I", rdata, off)[0]
        if magic != 0x19930520:
            continue
        max_state, p_unwind, n_try, p_try = struct.unpack_from("<IIII", rdata, off + 4)
        if 0 < max_state <= 256 and p_unwind:
            for i in range(max_state):
                action_va = pe.read_u32(p_unwind + i * 8 + 4)
                if action_va and is_valid_code_start(pe, md, action_va):
                    found.add(action_va)
        if 0 < n_try <= 64 and p_try:
            for i in range(n_try):
                n_catches = pe.read_u32(p_try + i * 20 + 12)
                p_handlers = pe.read_u32(p_try + i * 20 + 16)
                if n_catches and n_catches <= 32 and p_handlers:
                    for j in range(n_catches):
                        h_va = pe.read_u32(p_handlers + j * 16 + 12)
                        if h_va and is_valid_code_start(pe, md, h_va):
                            found.add(h_va)
    return found


def follows_function_end(pe, va):
    """True if va directly follows padding or a return (`ret`, `ret imm16`).

    `ret imm16` (C2 xx xx) usually ends in 0x00, so checking only the previous
    byte misses e.g. vtable-only methods placed right after a `ret 4`."""
    off = va - pe.text_va
    if off <= 0:
        return True
    if pe.text_bytes[off - 1] in (0x90, 0xcc, 0xc3):
        return True
    return off >= 3 and pe.text_bytes[off - 3] == 0xc2


def discover_functions(pe, md, use_ghidra=True):
    func_entries = set()
    func_entries.add(pe.entry_va)

    # 1. Optional Ghidra function inventory from REA decompile-journal.jsonl (not required!)
    ghidra_ranges = {}
    if use_ghidra and JOURNAL_PATH and os.path.exists(JOURNAL_PATH):
        for line in open(JOURNAL_PATH):
            j = json.loads(line)
            if j.get("kind") == "function" and j.get("status") != "external":
                va = int(j["address"], 16)
                if pe.text_va <= va < pe.text_end:
                    func_entries.add(va)
                    ranges = (j.get("body") or {}).get("ranges") or []
                    ghidra_ranges[va] = [(int(r["start"], 16), int(r["end"], 16)) for r in ranges]

    # 2. All 32-bit aligned pointers in .rdata and .data into .text (vtables, _initterm, callbacks)
    # Reject only obvious ASCII 4-byte strings (all printable ASCII/NUL) or invalid opcodes
    for buf in (pe.rdata_bytes, pe.data_bytes):
        for off in range(0, len(buf) - 3, 4):
            v = struct.unpack_from("<I", buf, off)[0]
            if not is_valid_code_start(pe, md, v):
                continue
            # Check if bytes of v itself are 3 printable uppercase letters + 0x00 (e.g. "MEM\0", "SOF\0")
            b0, b1, b2, b3 = v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >> 24) & 0xff
            if b3 == 0 and all(0x20 <= b <= 0x7e for b in (b0, b1, b2)):
                if not ((v & 0xf) == 0 or follows_function_end(pe, v)):
                    continue
            func_entries.add(v)

    # 3. Standalone MSVC C++ EH FuncInfo unwind/catch handler scan
    eh_funcs = scan_msvc_eh_tables(pe, md)
    func_entries.update(eh_funcs)

    # 4. Recursive sweep to find all direct call targets, callback immediates, and switch tables
    seen_insns = set()
    switch_tables = {}  # jmp_va -> (table_va, [targets])

    def run_sweep(initial_queue):
        queue = list(initial_queue)
        while queue:
            va = queue.pop()
            cur = va
            while pe.text_va <= cur < pe.text_end and cur not in seen_insns:
                seen_insns.add(cur)
                off = cur - pe.text_va
                insns = list(md.disasm(pe.text_bytes[off : off + 16], cur, count=1))
                if not insns:
                    break
                ins = insns[0]
                m = ins.mnemonic

                # Check immediate code pointers (push/mov imm32)
                if m in ("push", "mov") and len(ins.operands) >= 1:
                    op = ins.operands[-1]
                    if op.type == cs_x86.X86_OP_IMM:
                        imm = op.imm & 0xffffffff
                        if pe.text_va <= imm < pe.text_end and imm not in func_entries and is_valid_code_start(pe, md, imm):
                            if (imm & 0xf) == 0 or follows_function_end(pe, imm):
                                func_entries.add(imm)
                                queue.append(imm)

                if m == "call":
                    op = ins.operands[0]
                    if op.type == cs_x86.X86_OP_IMM:
                        tgt = op.imm & 0xffffffff
                        if pe.text_va <= tgt < pe.text_end:
                            if tgt not in func_entries:
                                func_entries.add(tgt)
                            if tgt not in seen_insns:
                                queue.append(tgt)
                    cur += ins.size
                elif m == "jmp":
                    op = ins.operands[0]
                    if op.type == cs_x86.X86_OP_IMM:
                        tgt = op.imm & 0xffffffff
                        if pe.text_va <= tgt < pe.text_end and tgt not in seen_insns:
                            queue.append(tgt)
                    elif op.type == cs_x86.X86_OP_MEM:
                        # Check switch table: [index*4 + disp] with base == 0
                        if op.mem.base == 0 and op.mem.index != 0 and op.mem.scale == 4 and op.mem.disp != 0:
                            tva = op.mem.disp & 0xffffffff
                            tgts = recover_switch_targets(pe, md, tva, cur)
                            if tgts:
                                switch_tables[cur] = (tva, tgts)
                                for t in tgts:
                                    if t not in seen_insns:
                                        queue.append(t)
                    break
                elif m in JCC_COND:
                    op = ins.operands[0]
                    if op.type == cs_x86.X86_OP_IMM:
                        tgt = op.imm & 0xffffffff
                        if pe.text_va <= tgt < pe.text_end and tgt not in seen_insns:
                            queue.append(tgt)
                    cur += ins.size
                elif m in ("ret", "retn", "int3", "ud2", "hlt"):
                    break
                else:
                    cur += ins.size

    run_sweep(func_entries)

    # 5. Standalone gap/prologue scan for unreached 16-byte aligned functions after 0x90/0xCC padding
    #    (static ctors/dtors, CRT helpers, inlined MSVC prologues)
    text = pe.text_bytes
    text_len = len(text)
    gap_seeds = []
    # Restrict alignment scan to before any switch data tables, and validate clean prologue
    for off in range(16, text_len - 16, 16):
        va = pe.text_va + off
        if va in seen_insns or va in func_entries:
            continue
        prev = text[off - 1]
        if prev not in (0x90, 0xcc):
            continue
        b0, b1, b2 = text[off], text[off + 1], text[off + 2]
        # Recognize standard MSVC x86 function prologues:
        #   55 8b ec       push ebp; mov ebp, esp
        #   6a ff 68       push -1; push <SEH>
        #   64 a1 00 00    mov eax, fs:[0]
        #   83 ec xx       sub esp, imm8
        #   81 ec xx       sub esp, imm32
        #   56 8b f1       push esi; mov esi, ecx
        #   56 6a 01       push esi; push 1 (MSVC vector/scalar deleting dtor)
        #   56 57          push esi; push edi
        #   c7 01          mov dword ptr [ecx], <vtable>
        #   68 xx xx 4x 00 e8  push <dtor>; call atexit (static ctor)
        is_pro = (
            (b0 == 0x55 and b1 == 0x8b and b2 == 0xec)
            or (b0 == 0x6a and b1 == 0xff and b2 == 0x68)
            or (b0 == 0x64 and b1 == 0xa1 and b2 == 0x00)
            or (b0 == 0x83 and b1 == 0xec)
            or (b0 == 0x81 and b1 == 0xec)
            or (b0 == 0x56 and b1 in (0x8b, 0x6a, 0x57))
            or (b0 == 0xc7 and b1 == 0x01)
            or (b0 == 0x68 and text[off + 5] == 0xe8)
        )
        if is_pro and is_valid_code_start(pe, md, va):
            func_entries.add(va)
            gap_seeds.append(va)
    if gap_seeds:
        run_sweep(gap_seeds)

    return sorted(func_entries), switch_tables


# ---- Operand helpers ----
def mem_addr_expr(op):
    parts = []
    if op.mem.segment == cs_x86.X86_REG_FS:
        parts.append("c->teb_addr")
    if op.mem.base != 0:
        parts.append(REG32_MAP[op.mem.base])
    if op.mem.index != 0:
        idx = REG32_MAP[op.mem.index]
        if op.mem.scale == 1:
            parts.append(idx)
        else:
            parts.append(f"({idx} * {op.mem.scale}u)")
    disp = op.mem.disp
    if disp != 0 or not parts:
        parts.append(f"0x{disp & 0xffffffff:x}u")
    if len(parts) == 1:
        return parts[0]
    return "(uint32_t)(" + " + ".join(parts) + ")"


def read_op(op, size_override=None):
    sz = size_override or op.size
    if op.type == cs_x86.X86_OP_IMM:
        mask = {1: 0xff, 2: 0xffff, 4: 0xffffffff}[sz]
        return f"0x{op.imm & mask:x}u"
    elif op.type == cs_x86.X86_OP_REG:
        r = op.reg
        if r in REG32_MAP:
            return REG32_MAP[r]
        if r in REG16_MAP:
            return f"r16({REG16_MAP[r]})"
        if r in REG8_LO_MAP:
            return f"r8_lo({REG8_LO_MAP[r]})"
        if r in REG8_HI_MAP:
            return f"r8_hi({REG8_HI_MAP[r]})"
        raise ValueError(f"Unsupported read reg: {r}")
    elif op.type == cs_x86.X86_OP_MEM:
        addr = mem_addr_expr(op)
        return {1: f"mem_r8({addr})", 2: f"mem_r16({addr})", 4: f"mem_r32({addr})", 8: f"mem_r64({addr})"}[sz]
    raise ValueError(f"Unsupported op type: {op.type}")


def write_op(op, val_expr, size_override=None):
    sz = size_override or op.size
    if op.type == cs_x86.X86_OP_REG:
        r = op.reg
        if r in REG32_MAP:
            return f"{REG32_MAP[r]} = (uint32_t)({val_expr});"
        if r in REG16_MAP:
            return f"w16(&{REG16_MAP[r]}, (uint16_t)({val_expr}));"
        if r in REG8_LO_MAP:
            return f"w8_lo(&{REG8_LO_MAP[r]}, (uint8_t)({val_expr}));"
        if r in REG8_HI_MAP:
            return f"w8_hi(&{REG8_HI_MAP[r]}, (uint8_t)({val_expr}));"
        raise ValueError(f"Unsupported write reg: {r}")
    elif op.type == cs_x86.X86_OP_MEM:
        addr = mem_addr_expr(op)
        return {
            1: f"mem_w8({addr}, (uint8_t)({val_expr}));",
            2: f"mem_w16({addr}, (uint16_t)({val_expr}));",
            4: f"mem_w32({addr}, (uint32_t)({val_expr}));",
            8: f"mem_w64({addr}, (uint64_t)({val_expr}));",
        }[sz]
    raise ValueError(f"Unsupported write op type: {op.type}")


def read_fpu_mem(op):
    addr = mem_addr_expr(op)
    if op.size == 4:
        return f"(long double)mem_rf32({addr})"
    elif op.size == 8:
        return f"(long double)mem_rf64({addr})"
    elif op.size == 10:
        return f"mem_rf80({addr})"
    raise ValueError(f"Unsupported FPU mem size {op.size}")


def lift_instruction(ins, pe, func_va, func_entries_set, local_insns_set, switch_tables, flags_dead=False):
    m = ins.mnemonic
    ops = ins.operands
    next_va = ins.address + ins.size
    lines = []

    # NOP / WAIT / FNCLEX / CLD / STD / SAHF / LAHF / CDQ / CWDE / CBW / CWD / LEAVE
    if m in ("nop", "wait", "fwait", "fnclex", "fclex", "fnop"):
        return []
    if m == "cld":
        return ["c->df = 0;"]
    if m == "std":
        return ["c->df = 1;"]
    if m == "sahf":
        return ["alu_sahf(c);"]
    if m == "lahf":
        return ["alu_lahf(c);"]
    if m == "cdq":
        return ["c->edx = ((int32_t)c->eax < 0) ? 0xFFFFFFFFu : 0u;"]
    if m == "cwde":
        return ["c->eax = (uint32_t)(int32_t)(int16_t)(uint16_t)c->eax;"]
    if m == "cbw":
        return ["w16(&c->eax, (uint16_t)(int16_t)(int8_t)(uint8_t)c->eax);"]
    if m == "cwd":
        return ["w16(&c->edx, ((int16_t)(uint16_t)c->eax < 0) ? 0xFFFFu : 0u);"]
    if m == "leave":
        return ["c->esp = c->ebp; c->ebp = pop32(c);"]
    if m == "clc":
        return ["c->cf = 0;"]
    if m == "stc":
        return ["c->cf = 1;"]
    if m == "cmc":
        return ["c->cf ^= 1u;"]
    if m == "pushfd":
        # Bit 1 is reserved-1; IF (bit 9) is always set in Windows user mode (0x202 baseline)
        return ["push32(c, (c->cf) | 0x202u | (c->pf<<2) | (c->zf<<6) | (c->sf<<7) | (c->df<<10) | (c->of<<11));"]
    if m == "popfd":
        return ["{ uint32_t f = pop32(c); c->cf = f&1; c->pf = (f>>2)&1; c->zf = (f>>6)&1; c->sf = (f>>7)&1; c->df = (f>>10)&1; c->of = (f>>11)&1; }"]
    if m == "pushal":
        return ["{ uint32_t t = c->esp; push32(c, c->eax); push32(c, c->ecx); push32(c, c->edx); push32(c, c->ebx); push32(c, t); push32(c, c->ebp); push32(c, c->esi); push32(c, c->edi); }"]
    if m == "popal":
        return ["c->edi = pop32(c); c->esi = pop32(c); c->ebp = pop32(c); c->esp += 4; c->ebx = pop32(c); c->edx = pop32(c); c->ecx = pop32(c); c->eax = pop32(c);"]

    # MOV / MOVZX / MOVSX / LEA / XCHG / PUSH / POP / BSWAP
    if m == "mov":
        return [write_op(ops[0], read_op(ops[1], ops[0].size))]
    if m == "movzx":
        return [write_op(ops[0], read_op(ops[1]))]
    if m == "movsx":
        src = read_op(ops[1])
        st = {1: "(int32_t)(int8_t)", 2: "(int32_t)(int16_t)"}[ops[1].size]
        return [write_op(ops[0], f"(uint32_t)({st}({src}))")]
    if m == "lea":
        return [write_op(ops[0], mem_addr_expr(ops[1]))]
    if m == "xchg" or m == "lock xchg":
        sz = ops[0].size
        ut = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t"}[sz]
        if ops[0].type == cs_x86.X86_OP_MEM:
            a = mem_addr_expr(ops[0])
            return [f"{{ uint32_t _a = {a}; {ut} _t = __atomic_exchange_n(({ut} *)(g_mem + _a), ({ut})({read_op(ops[1])}), __ATOMIC_SEQ_CST); {write_op(ops[1], '_t')} }}"]
        elif ops[1].type == cs_x86.X86_OP_MEM:
            a = mem_addr_expr(ops[1])
            return [f"{{ uint32_t _a = {a}; {ut} _t = __atomic_exchange_n(({ut} *)(g_mem + _a), ({ut})({read_op(ops[0])}), __ATOMIC_SEQ_CST); {write_op(ops[0], '_t')} }}"]
        else:
            return [f"{{ {ut} _t = ({ut})({read_op(ops[0])}); {write_op(ops[0], read_op(ops[1]))} {write_op(ops[1], '_t')} }}"]
    if m == "bswap":
        r = read_op(ops[0])
        return [write_op(ops[0], f"__builtin_bswap32({r})")]
    if m == "push":
        if ops[0].size == 2:
            return [f"push16(c, (uint16_t)({read_op(ops[0])}));"]
        return [f"push32(c, (uint32_t)({read_op(ops[0], 4)}));"]
    if m == "pop":
        sz = ops[0].size
        ut = {2: "uint16_t", 4: "uint32_t"}[sz]
        pop_fn = f"pop{sz*8}(c)"
        if ops[0].type == cs_x86.X86_OP_MEM and (ops[0].mem.base == cs_x86.X86_REG_ESP or ops[0].mem.index == cs_x86.X86_REG_ESP):
            # Intel SDM: POP [ESP+disp] (16- or 32-bit) increments ESP BEFORE computing effective address!
            return [f"{{ {ut} _v = {pop_fn}; {write_op(ops[0], '_v')} }}"]
        return [write_op(ops[0], pop_fn)]

    # ALU binary ops: add, adc, sub, sbb, and, or, xor
    if m in ("add", "adc", "sub", "sbb", "and", "or", "xor"):
        sz = ops[0].size
        bits = sz * 8
        ut = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t"}[sz]
        if m in ("and", "or", "xor"):
            opch = {"and": "&", "or": "|", "xor": "^"}[m]
            fn = f"alu_logic{bits}"
            if ops[0].type == cs_x86.X86_OP_MEM:
                if flags_dead:
                    return [f"{{ uint32_t _a = {mem_addr_expr(ops[0])}; mem_w{bits}(_a, ({ut})(mem_r{bits}(_a) {opch} ({ut})({read_op(ops[1], sz)}))); }}"]
                return [f"{{ uint32_t _a = {mem_addr_expr(ops[0])}; mem_w{bits}(_a, {fn}(c, ({ut})(mem_r{bits}(_a) {opch} ({ut})({read_op(ops[1], sz)})))); }}"]
            if flags_dead:
                return [write_op(ops[0], f"({ut})({read_op(ops[0])} {opch} ({ut})({read_op(ops[1], sz)}))")]
            return [write_op(ops[0], f"{fn}(c, ({ut})({read_op(ops[0])} {opch} ({ut})({read_op(ops[1], sz)})))")]
        else:
            nf = flags_dead and m in ("add", "sub")
            fn = f"nf_{m}{bits}" if nf else f"alu_{m}{bits}"
            ctx = "" if nf else "c, "
            if ops[0].type == cs_x86.X86_OP_MEM:
                return [f"{{ uint32_t _a = {mem_addr_expr(ops[0])}; mem_w{bits}(_a, {fn}({ctx}mem_r{bits}(_a), ({ut})({read_op(ops[1], sz)}))); }}"]
            return [write_op(ops[0], f"{fn}({ctx}{read_op(ops[0])}, ({ut})({read_op(ops[1], sz)}))")]

    # LOCK ADD / SUB / INC / DEC / XADD / CMPXCHG
    if m == "lock inc":
        sz = ops[0].size
        bits = sz * 8
        ut = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t"}[sz]
        return [f"{{ uint32_t _a = {mem_addr_expr(ops[0])}; {ut} _old = __atomic_fetch_add(({ut} *)(g_mem + _a), ({ut})1u, __ATOMIC_SEQ_CST); alu_inc{bits}(c, _old); }}"]
    if m == "lock dec":
        sz = ops[0].size
        bits = sz * 8
        ut = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t"}[sz]
        return [f"{{ uint32_t _a = {mem_addr_expr(ops[0])}; {ut} _old = __atomic_fetch_sub(({ut} *)(g_mem + _a), ({ut})1u, __ATOMIC_SEQ_CST); alu_dec{bits}(c, _old); }}"]
    if m == "lock add":
        sz = ops[0].size
        bits = sz * 8
        ut = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t"}[sz]
        return [f"{{ uint32_t _a = {mem_addr_expr(ops[0])}; {ut} _add = ({ut})({read_op(ops[1], sz)}); {ut} _old = __atomic_fetch_add(({ut} *)(g_mem + _a), _add, __ATOMIC_SEQ_CST); alu_add{bits}(c, _old, _add); }}"]

    # CMP / TEST
    if m in ("cmp", "test") and flags_dead:
        return []  # flags never read: compare/test has no other effect
    if m == "cmp":
        sz = ops[0].size
        bits = sz * 8
        ut = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t"}[sz]
        return [f"alu_sub{bits}(c, ({ut})({read_op(ops[0])}), ({ut})({read_op(ops[1], sz)}));"]
    if m == "test":
        sz = ops[0].size
        bits = sz * 8
        ut = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t"}[sz]
        return [f"alu_logic{bits}(c, ({ut})(({ut})({read_op(ops[0])}) & ({ut})({read_op(ops[1], sz)})));"]

    # Unary ALU: inc, dec, neg, not
    if m in ("inc", "dec", "neg"):
        sz = ops[0].size
        bits = sz * 8
        fn = f"nf_{m}{bits}" if flags_dead else f"alu_{m}{bits}"
        ctx = "" if flags_dead else "c, "
        if ops[0].type == cs_x86.X86_OP_MEM:
            return [f"{{ uint32_t _a = {mem_addr_expr(ops[0])}; mem_w{bits}(_a, {fn}({ctx}mem_r{bits}(_a))); }}"]
        return [write_op(ops[0], f"{fn}({ctx}{read_op(ops[0])})")]
    if m == "not":
        sz = ops[0].size
        bits = sz * 8
        if ops[0].type == cs_x86.X86_OP_MEM:
            return [f"{{ uint32_t _a = {mem_addr_expr(ops[0])}; mem_w{bits}(_a, ~mem_r{bits}(_a)); }}"]
        return [write_op(ops[0], f"~({read_op(ops[0])})")]

    # Shifts / Rotates
    if m in ("shl", "sal", "shr", "sar", "rol", "ror", "rcr", "rcl"):
        nm = "shl" if m == "sal" else m
        sz = ops[0].size
        bits = sz * 8
        cnt = read_op(ops[1], 1) if len(ops) == 2 else "1u"
        if nm in ("rcr", "rcl") and bits != 32:
            raise ValueError(f"Unsupported {nm} width {bits}")
        nf = flags_dead and nm in ("shl", "shr", "sar")
        fn = f"nf_{nm}{bits}" if nf else f"alu_{nm}{bits}"
        ctx = "" if nf else "c, "
        if ops[0].type == cs_x86.X86_OP_MEM:
            return [f"{{ uint32_t _a = {mem_addr_expr(ops[0])}; mem_w{bits}(_a, {fn}({ctx}mem_r{bits}(_a), {cnt})); }}"]
        return [write_op(ops[0], f"{fn}({ctx}{read_op(ops[0])}, {cnt})")]

    if m in ("shld", "shrd"):
        sz = ops[0].size
        if sz != 4:
            raise ValueError(f"Unsupported {m} width {sz}")
        fn = f"alu_{m}32"
        cnt = read_op(ops[2], 1)
        if ops[0].type == cs_x86.X86_OP_MEM:
            return [f"{{ uint32_t _a = {mem_addr_expr(ops[0])}; mem_w32(_a, {fn}(c, mem_r32(_a), {read_op(ops[1])}, {cnt})); }}"]
        return [write_op(ops[0], f"{fn}(c, {read_op(ops[0])}, {read_op(ops[1])}, {cnt})")]

    # MUL / IMUL / DIV / IDIV
    if m == "mul":
        bits = ops[0].size * 8
        return [f"alu_mul{bits}(c, {read_op(ops[0])});"]
    if m == "imul":
        if len(ops) == 1:
            bits = ops[0].size * 8
            return [f"alu_imul1_{bits}(c, {read_op(ops[0])});"]
        elif len(ops) == 2:
            bits = ops[0].size * 8
            fn = f"nf_imul2_{bits}(" if flags_dead else f"alu_imul2_{bits}(c, "
            return [write_op(ops[0], f"{fn}{read_op(ops[0])}, {read_op(ops[1], ops[0].size)})")]
        elif len(ops) == 3:
            bits = ops[0].size * 8
            fn = f"nf_imul2_{bits}(" if flags_dead else f"alu_imul2_{bits}(c, "
            return [write_op(ops[0], f"{fn}{read_op(ops[1])}, {read_op(ops[2], ops[0].size)})")]
    if m == "div":
        bits = ops[0].size * 8
        return [f"alu_div{bits}(c, {read_op(ops[0])});"]
    if m == "idiv":
        bits = ops[0].size * 8
        return [f"alu_idiv{bits}(c, {read_op(ops[0])});"]

    # Bit test: bt, bts, btr, btc, bsf, bsr
    if m in ("bt", "bts", "btr", "btc"):
        sz = ops[0].size
        bits = sz * 8
        ut = {2: "uint16_t", 4: "uint32_t"}[sz]
        it = {2: "int16_t", 4: "int32_t"}[sz]
        mask = bits - 1
        shift = 4 if sz == 2 else 5
        bit = read_op(ops[1])
        if ops[0].type == cs_x86.X86_OP_MEM:
            addr = mem_addr_expr(ops[0])
            if ops[1].type == cs_x86.X86_OP_IMM:
                base_code = [f"{{ uint32_t _a = {addr}; {ut} _m = ({ut})(1u << (({bit}) & {mask}u)); {ut} _v = mem_r{bits}(_a); c->cf = (_v & _m) != 0;"]
            else:
                base_code = [f"{{ int32_t _b = (int32_t)({it})({bit}); uint32_t _a = {addr} + (uint32_t)((_b >> {shift}) * {sz}); {ut} _m = ({ut})(1u << (_b & {mask})); {ut} _v = mem_r{bits}(_a); c->cf = (_v & _m) != 0;"]
            if m == "bts":
                base_code.append(f"mem_w{bits}(_a, ({ut})(_v | _m));")
            elif m == "btr":
                base_code.append(f"mem_w{bits}(_a, ({ut})(_v & ~_m));")
            elif m == "btc":
                base_code.append(f"mem_w{bits}(_a, ({ut})(_v ^ _m));")
            base_code.append("}")
            return [" ".join(base_code)]
        else:
            v = read_op(ops[0])
            code = f"{{ {ut} _m = ({ut})(1u << (({bit}) & {mask}u)); {ut} _v = ({ut})({v}); c->cf = (_v & _m) != 0;"
            if m == "bts":
                code += f" {write_op(ops[0], '(_v | _m)')}"
            elif m == "btr":
                code += f" {write_op(ops[0], '(_v & ~_m)')}"
            elif m == "btc":
                code += f" {write_op(ops[0], '(_v ^ _m)')}"
            code += " }"
            return [code]
    if m in ("bsf", "bsr"):
        src = read_op(ops[1])
        fn = "__builtin_ctz" if m == "bsf" else "31u - (uint32_t)__builtin_clz"
        return [f"{{ uint32_t _s = {src}; c->zf = (_s == 0); if (_s) {{ {write_op(ops[0], f'{fn}(_s)')} }} }}"]

    # SETcc / CMOVcc
    if m in SETCC_COND:
        return [write_op(ops[0], f"({SETCC_COND[m]} ? 1u : 0u)")]
    if m.startswith("cmov"):
        cc = "j" + m[4:]
        if cc in JCC_COND:
            return [f"if ({JCC_COND[cc]}) {{ {write_op(ops[0], read_op(ops[1]))} }}"]

    # String ops
    STR_MAP = {
        "movsb": "str_movsb(c);", "movsw": "str_movsw(c);", "movsd": "str_movsd(c);",
        "rep movsb": "rep_movsb(c);", "rep movsw": "rep_movsw(c);", "rep movsd": "rep_movsd(c);",
        "stosb": "str_stosb(c);", "stosw": "str_stosw(c);", "stosd": "str_stosd(c);",
        "rep stosb": "rep_stosb(c);", "rep stosw": "rep_stosw(c);", "rep stosd": "rep_stosd(c);",
        "repe cmpsb": "repe_cmpsb(c);", "repe cmpsw": "repe_cmpsw(c);", "repe cmpsd": "repe_cmpsd(c);",
        "repne scasb": "repne_scasb(c);", "repe scasb": "repe_scasb(c);",
    }
    if m in STR_MAP:
        return [STR_MAP[m]]

    # Control flow: CALL, RET, JMP, Jcc
    if m == "call":
        op = ops[0]
        if op.type == cs_x86.X86_OP_IMM:
            tgt = op.imm & 0xffffffff
            if tgt in HOOKABLE_FUNCS:
                return [f"push32(c, 0x{next_va:08x}u); g_code_table[0x{tgt - pe.text_va:x}u](c);"]
            return [f"push32(c, 0x{next_va:08x}u); sub_{tgt:08x}(c);"]
        elif op.type == cs_x86.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0 and (op.mem.disp & 0xffffffff) in pe.iat_map:
            imp_id, dll, fname = pe.iat_map[op.mem.disp & 0xffffffff]
            return [f"push32(c, 0x{next_va:08x}u); win32_dispatch(c, {imp_id}u); /* {dll}!{fname} */"]
        else:
            return [f"{{ uint32_t _t = {read_op(op)}; push32(c, 0x{next_va:08x}u); dispatch_call(c, _t); }}"]

    if m in ("ret", "retn"):
        imm = (ops[0].imm & 0xffff) if len(ops) == 1 else 0
        return [f"c->esp += {4 + imm}u; return;"]

    if m == "jmp":
        op = ops[0]
        if op.type == cs_x86.X86_OP_IMM:
            tgt = op.imm & 0xffffffff
            if tgt in local_insns_set:
                return [f"goto L_{tgt:08x};"]
            elif tgt in HOOKABLE_FUNCS:
                return [f"g_code_table[0x{tgt - pe.text_va:x}u](c); return;"]
            elif tgt in func_entries_set:
                return [f"sub_{tgt:08x}(c); return;"]
            else:
                return [f"dispatch_call(c, 0x{tgt:08x}u); return;"]
        elif op.type == cs_x86.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0 and (op.mem.disp & 0xffffffff) in pe.iat_map:
            imp_id, dll, fname = pe.iat_map[op.mem.disp & 0xffffffff]
            return [f"win32_dispatch(c, {imp_id}u); return; /* {dll}!{fname} */"]
        elif ins.address in switch_tables:
            _, tgts = switch_tables[ins.address]
            unique_tgts = sorted(set(tgts))
            s = [f"switch ({read_op(op)}) {{"]
            for t in unique_tgts:
                if t in local_insns_set:
                    s.append(f"  case 0x{t:08x}u: goto L_{t:08x};")
                else:
                    s.append(f"  case 0x{t:08x}u: dispatch_call(c, 0x{t:08x}u); return;")
            s.append(f"  default: dispatch_call(c, {read_op(op)}); return;")
            s.append("}")
            return s
        else:
            return [f"dispatch_call(c, {read_op(op)}); return;"]

    if m in JCC_COND:
        tgt = ops[0].imm & 0xffffffff
        cond = JCC_COND[m]
        if tgt in local_insns_set:
            return [f"if ({cond}) goto L_{tgt:08x};"]
        elif tgt in HOOKABLE_FUNCS:
            return [f"if ({cond}) {{ g_code_table[0x{tgt - pe.text_va:x}u](c); return; }}"]
        elif tgt in func_entries_set:
            return [f"if ({cond}) {{ sub_{tgt:08x}(c); return; }}"]
        else:
            return [f"if ({cond}) {{ dispatch_call(c, 0x{tgt:08x}u); return; }}"]

    if m == "loop":
        tgt = ops[0].imm & 0xffffffff
        return [f"c->ecx--; if (c->ecx != 0) goto L_{tgt:08x};"]

    # ---- x87 FPU ----
    if m == "fld":
        if ops[0].type == cs_x86.X86_OP_REG:
            i = ST_REGS[ops[0].reg]
            return [f"{{ long double _v = fpu_st(c, {i}); fpu_push(c, _v); }}"]
        return [f"fpu_push(c, {read_fpu_mem(ops[0])});"]
    if m == "fild":
        addr = mem_addr_expr(ops[0])
        cast = {2: "(int16_t)mem_r16", 4: "(int32_t)mem_r32", 8: "(int64_t)mem_r64"}[ops[0].size]
        return [f"fpu_push(c, (long double)({cast}({addr})));"]
    if m == "fldz":
        return ["fpu_push(c, 0.0L);"]
    if m == "fld1":
        return ["fpu_push(c, 1.0L);"]
    if m == "fldpi":
        return ["fpu_push(c, 3.14159265358979323846L);"]
    if m == "fldln2":
        return ["fpu_push(c, 0.69314718055994530942L);"]
    if m == "fldl2e":
        return ["fpu_push(c, 1.44269504088896340736L);"]
    if m == "fldl2t":
        return ["fpu_push(c, 3.32192809488736234787L);"]
    if m == "fldlg2":
        return ["fpu_push(c, 0.30102999566398119521L);"]

    if m in ("fst", "fstp"):
        pop_code = " fpu_pop(c);" if m == "fstp" else ""
        if ops[0].type == cs_x86.X86_OP_REG:
            i = ST_REGS[ops[0].reg]
            return [f"*fpu_st_ptr(c, {i}) = fpu_st(c, 0);{pop_code}"]
        addr = mem_addr_expr(ops[0])
        if ops[0].size == 4:
            return [f"mem_wf32({addr}, (float)fpu_st(c, 0));{pop_code}"]
        elif ops[0].size == 8:
            return [f"mem_wf64({addr}, (double)fpu_st(c, 0));{pop_code}"]
        elif ops[0].size == 10:
            return [f"mem_wf80({addr}, fpu_st(c, 0));{pop_code}"]
        raise ValueError(f"Unsupported {m} size {ops[0].size}")

    if m in ("fist", "fistp"):
        pop_code = " fpu_pop(c);" if m == "fistp" else ""
        addr = mem_addr_expr(ops[0])
        wfn = {2: "mem_w16", 4: "mem_w32", 8: "mem_w64"}[ops[0].size]
        ut = {2: "uint16_t", 4: "uint32_t", 8: "uint64_t"}[ops[0].size]
        bits = ops[0].size * 8
        return [f"{wfn}({addr}, ({ut})fpu_to_int(c, fpu_st(c, 0), {bits}));{pop_code}"]

    if m in ("fadd", "faddp", "fmul", "fmulp", "fsub", "fsubp", "fsubr", "fsubrp", "fdiv", "fdivp", "fdivr", "fdivrp"):
        pop = m.endswith("p")
        base_m = m[:-1] if pop else m
        b0 = ins.bytes[0]
        if len(ops) == 0:
            # Default ST(1) op= ST(0); pop
            idx = 1
            if base_m == "fadd":
                return ["*fpu_st_ptr(c, 1) += fpu_st(c, 0); fpu_pop(c);"]
            if base_m == "fmul":
                return ["*fpu_st_ptr(c, 1) *= fpu_st(c, 0); fpu_pop(c);"]
            if base_m == "fsub":
                return ["*fpu_st_ptr(c, 1) = fpu_st(c, 1) - fpu_st(c, 0); fpu_pop(c);"]
            if base_m == "fsubr":
                return ["*fpu_st_ptr(c, 1) = fpu_st(c, 0) - fpu_st(c, 1); fpu_pop(c);"]
            if base_m == "fdiv":
                return ["*fpu_st_ptr(c, 1) = fpu_st(c, 1) / fpu_st(c, 0); fpu_pop(c);"]
            if base_m == "fdivr":
                return ["*fpu_st_ptr(c, 1) = fpu_st(c, 0) / fpu_st(c, 1); fpu_pop(c);"]
        elif len(ops) == 1 and ops[0].type == cs_x86.X86_OP_MEM:
            val = read_fpu_mem(ops[0])
            if base_m == "fadd":  return [f"*fpu_st_ptr(c, 0) += {val};"]
            if base_m == "fmul":  return [f"*fpu_st_ptr(c, 0) *= {val};"]
            if base_m == "fsub":  return [f"*fpu_st_ptr(c, 0) -= {val};"]
            if base_m == "fsubr": return [f"*fpu_st_ptr(c, 0) = ({val}) - fpu_st(c, 0);"]
            if base_m == "fdiv":  return [f"*fpu_st_ptr(c, 0) /= {val};"]
            if base_m == "fdivr": return [f"*fpu_st_ptr(c, 0) = ({val}) / fpu_st(c, 0);"]
        else:
            # Register-register x87 op. Distinguish dst/src using opcode byte (0xD8 -> dst=ST0, 0xDC/0xDE -> dst=STi)
            if len(ops) == 1:
                i = ST_REGS[ops[0].reg]
                dst_is_sti = (b0 in (0xDC, 0xDE))
            else:
                d_idx = ST_REGS[ops[0].reg]
                s_idx = ST_REGS[ops[1].reg]
                dst_is_sti = (d_idx != 0) or (b0 in (0xDC, 0xDE))
                i = d_idx if dst_is_sti else s_idx
            pop_s = " fpu_pop(c);" if pop else ""
            if not dst_is_sti:
                # D8: ST(0) = ST(0) op ST(i) (or reversed)
                if base_m == "fadd":  return [f"*fpu_st_ptr(c, 0) += fpu_st(c, {i});{pop_s}"]
                if base_m == "fmul":  return [f"*fpu_st_ptr(c, 0) *= fpu_st(c, {i});{pop_s}"]
                if base_m == "fsub":  return [f"*fpu_st_ptr(c, 0) = fpu_st(c, 0) - fpu_st(c, {i});{pop_s}"]
                if base_m == "fsubr": return [f"*fpu_st_ptr(c, 0) = fpu_st(c, {i}) - fpu_st(c, 0);{pop_s}"]
                if base_m == "fdiv":  return [f"*fpu_st_ptr(c, 0) = fpu_st(c, 0) / fpu_st(c, {i});{pop_s}"]
                if base_m == "fdivr": return [f"*fpu_st_ptr(c, 0) = fpu_st(c, {i}) / fpu_st(c, 0);{pop_s}"]
            else:
                # DC/DE: ST(i) = ST(i) op ST(0) (or reversed for fsubr/fdivr!)
                if base_m == "fadd":  return [f"*fpu_st_ptr(c, {i}) += fpu_st(c, 0);{pop_s}"]
                if base_m == "fmul":  return [f"*fpu_st_ptr(c, {i}) *= fpu_st(c, 0);{pop_s}"]
                if base_m == "fsub":  return [f"*fpu_st_ptr(c, {i}) = fpu_st(c, {i}) - fpu_st(c, 0);{pop_s}"]
                if base_m == "fsubr": return [f"*fpu_st_ptr(c, {i}) = fpu_st(c, 0) - fpu_st(c, {i});{pop_s}"]
                if base_m == "fdiv":  return [f"*fpu_st_ptr(c, {i}) = fpu_st(c, {i}) / fpu_st(c, 0);{pop_s}"]
                if base_m == "fdivr": return [f"*fpu_st_ptr(c, {i}) = fpu_st(c, 0) / fpu_st(c, {i});{pop_s}"]

    if m in ("fiadd", "fisub", "fisubr", "fimul", "fidiv", "fidivr"):
        addr = mem_addr_expr(ops[0])
        cast = {2: "(int16_t)mem_r16", 4: "(int32_t)mem_r32"}[ops[0].size]
        val = f"(long double)({cast}({addr}))"
        if m == "fiadd":  return [f"*fpu_st_ptr(c, 0) += {val};"]
        if m == "fimul":  return [f"*fpu_st_ptr(c, 0) *= {val};"]
        if m == "fisub":  return [f"*fpu_st_ptr(c, 0) -= {val};"]
        if m == "fisubr": return [f"*fpu_st_ptr(c, 0) = ({val}) - fpu_st(c, 0);"]
        if m == "fidiv":  return [f"*fpu_st_ptr(c, 0) /= {val};"]
        if m == "fidivr": return [f"*fpu_st_ptr(c, 0) = ({val}) / fpu_st(c, 0);"]

    if m in ("fcom", "fcomp", "fucom", "fucomp"):
        pop_s = " fpu_pop(c);" if m.endswith("p") else ""
        if len(ops) == 0:
            return [f"fpu_cmp(c, fpu_st(c, 0), fpu_st(c, 1));{pop_s}"]
        elif ops[0].type == cs_x86.X86_OP_REG:
            i = ST_REGS[ops[0].reg]
            return [f"fpu_cmp(c, fpu_st(c, 0), fpu_st(c, {i}));{pop_s}"]
        else:
            return [f"fpu_cmp(c, fpu_st(c, 0), {read_fpu_mem(ops[0])});{pop_s}"]
    if m in ("fcompp", "fucompp"):
        return ["fpu_cmp(c, fpu_st(c, 0), fpu_st(c, 1)); fpu_pop(c); fpu_pop(c);"]
    if m == "ftst":
        return ["fpu_cmp(c, fpu_st(c, 0), 0.0L);"]
    if m == "fxch":
        if len(ops) == 0:
            i = 1
        elif len(ops) == 1:
            i = ST_REGS[ops[0].reg]
        else:
            # Capstone emits 2 operands [ST(0), ST(i)] (or [ST(i), ST(0)]) for D9 C8+i
            r0 = ST_REGS[ops[0].reg]
            r1 = ST_REGS[ops[1].reg]
            i = r1 if r0 == 0 else r0
        if i == 0:
            return ["(void)0; /* fxch st(0) */"]
        return [f"{{ long double _t = fpu_st(c, 0); *fpu_st_ptr(c, 0) = fpu_st(c, {i}); *fpu_st_ptr(c, {i}) = _t; }}"]
    if m == "fchs":
        return ["*fpu_st_ptr(c, 0) = -fpu_st(c, 0);"]
    if m == "fabs":
        return ["*fpu_st_ptr(c, 0) = fabsl(fpu_st(c, 0));"]
    if m == "fsqrt":
        return ["*fpu_st_ptr(c, 0) = sqrtl(fpu_st(c, 0));"]
    if m == "fsin":
        return ["{ long double _a = fpu_st(c, 0); if (isfinite(_a) && fabsl(_a) < 0x1p63L) { *fpu_st_ptr(c, 0) = sinl(_a); c->fpu_sw &= (uint16_t)~0x0400u; } else { c->fpu_sw |= 0x0400u; } }"]
    if m == "fcos":
        return ["{ long double _a = fpu_st(c, 0); if (isfinite(_a) && fabsl(_a) < 0x1p63L) { *fpu_st_ptr(c, 0) = cosl(_a); c->fpu_sw &= (uint16_t)~0x0400u; } else { c->fpu_sw |= 0x0400u; } }"]
    if m == "fsincos":
        return ["{ long double _a = fpu_st(c, 0); if (isfinite(_a) && fabsl(_a) < 0x1p63L) { *fpu_st_ptr(c, 0) = sinl(_a); fpu_push(c, cosl(_a)); c->fpu_sw &= (uint16_t)~0x0400u; } else { c->fpu_sw |= 0x0400u; } }"]
    if m == "fptan":
        return ["{ long double _a = fpu_st(c, 0); if (isfinite(_a) && fabsl(_a) < 0x1p63L) { *fpu_st_ptr(c, 0) = tanl(_a); fpu_push(c, 1.0L); c->fpu_sw &= (uint16_t)~0x0400u; } else { c->fpu_sw |= 0x0400u; } }"]
    if m == "fpatan":
        return ["*fpu_st_ptr(c, 1) = atan2l(fpu_st(c, 1), fpu_st(c, 0)); fpu_pop(c);"]
    if m == "f2xm1":
        return ["*fpu_st_ptr(c, 0) = exp2l(fpu_st(c, 0)) - 1.0L;"]
    if m == "fyl2x":
        return ["*fpu_st_ptr(c, 1) = fpu_st(c, 1) * log2l(fpu_st(c, 0)); fpu_pop(c);"]
    if m == "fyl2xp1":
        return ["*fpu_st_ptr(c, 1) = fpu_st(c, 1) * log2l(fpu_st(c, 0) + 1.0L); fpu_pop(c);"]
    if m == "fscale":
        return ["*fpu_st_ptr(c, 0) = ldexpl(fpu_st(c, 0), (int)truncl(fpu_st(c, 1)));"]
    if m == "frndint":
        return ["*fpu_st_ptr(c, 0) = fpu_round_ld(c, fpu_st(c, 0));"]
    if m == "fprem":
        return ["{ long double _a = fpu_st(c, 0), _b = fpu_st(c, 1); *fpu_st_ptr(c, 0) = fmodl(_a, _b); long double _qf = (_b != 0.0L) ? fabsl(truncl(_a / _b)) : 0.0L; uint32_t _uq = (_qf < 0x1p52L) ? (uint32_t)fmodl(_qf, 8.0L) : 0u; c->fpu_sw &= (uint16_t)~0x4700u; if (_uq & 1u) c->fpu_sw |= 0x0200u; if (_uq & 2u) c->fpu_sw |= 0x4000u; if (_uq & 4u) c->fpu_sw |= 0x0100u; }"]
    if m == "fprem1":
        return ["{ long double _a = fpu_st(c, 0), _b = fpu_st(c, 1); int _q = 0; *fpu_st_ptr(c, 0) = remquol(_a, _b, &_q); uint32_t _uq = (_q < 0) ? (0u - (uint32_t)_q) : (uint32_t)_q; c->fpu_sw &= (uint16_t)~0x4700u; if (_uq & 1u) c->fpu_sw |= 0x0200u; if (_uq & 2u) c->fpu_sw |= 0x4000u; if (_uq & 4u) c->fpu_sw |= 0x0100u; }"]
    if m in ("fnstsw", "fstsw"):
        return [write_op(ops[0], "fpu_get_sw(c)")]
    if m in ("fnstcw", "fstcw"):
        return [write_op(ops[0], "c->fpu_cw")]
    if m == "fldcw":
        return [f"c->fpu_cw = {read_op(ops[0], 2)};"]
    if m in ("fnsave", "fsave"):
        return [f"fpu_save(c, {mem_addr_expr(ops[0])});"]
    if m == "frstor":
        return [f"fpu_rstor(c, {mem_addr_expr(ops[0])});"]
    if m == "fninit" or m == "finit":
        return ["c->fpu_cw = 0x037Fu; c->fpu_sw = 0; c->fpu_top = 0;"]

    # Rare/fallback
    if m in ("int3", "ud2", "hlt"):
        return [f'fprintf(stderr, "TRAP {m} at 0x{ins.address:08x}\\n"); return;']

    raise ValueError(f"Unhandled instruction at 0x{ins.address:08x}: {m} {ins.op_str}")


LIFT_STATS = collections.Counter()


def stmts_set_flags(stmts):
    """True if emitted statements still compute flags via an alu_* helper."""
    return any("alu_" in st for st in stmts)


# Original-game functions patched at runtime via g_code_table (runtime/game_patches.c).
# Direct calls to these VAs are dispatched through g_code_table so runtime patches take effect.
HOOKABLE_FUNCS = {
    0x00444230,  # ObjMutex::TryLock (non-atomic spinlock + forced lock-steal fix)
    0x004443F0,  # MetaSpinLock::Lock (non-atomic spinlock + forced lock-steal fix)
    0x004A7A5E,  # CRT ceil(double) (native hardware replacement for RLE blitters)
    0x004A71CC,  # CRT __ftol (native hardware replacement for RLE blitters)
}

# Exact instruction VAs in 0x00474fd0 that dereference ESI before `test esi, esi; je`
ESI_NULL_GUARD_VAS = {0x0047530E, 0x00475317}

# Original-game bugfixes spliced in as C statements emitted *before* the instruction at
# the given VA (after its label, so every incoming edge is covered).
GUEST_BUGFIX_INSERTS = {
    # 0x0042a820 weighted random pick (ambient/idle variant pools, items vt 0x00507998):
    # if no item is available yet (resources still loading on the background loader
    # thread) the selection stays -1 and the original code calls arr[-1]->vfn(0xfa6) and
    # decrements arr[-1]->weight, i.e. dereferences/corrupts the heap block header.
    # Take the function's own empty-pool exit instead (stack: ebx, edi, esi, ebp pushed).
    0x0042A8E4: (
        "if ((int32_t)mem_r32(c->esi + 0xb0u) < 0) { "
        "mem_w32(c->esi + 0xb4u, 0u); c->eax = 0u; "
        "c->ebx = pop32(c); c->edi = pop32(c); c->esi = pop32(c); c->ebp = pop32(c); "
        "c->esp += 4u; return; } /* bugfix: no available item, skip instead of arr[-1] */"
    ),
    # 0x00442700 GUI focus manager, per-frame keyboard loop (this = esi; +0x1c focused
    # element, +0x24 Tab-navigation root, element +0xc = parent). Loading a game from the
    # "Kapitelauswahl" save screen (x/006 -> "Laden") closes the screen without restoring
    # the root (the Escape path does), so Tab keeps cycling the detached screen's invisible
    # slots/buttons in the room and Enter/Space/click "activates" them -> slot-select
    # message to the closed screen -> virtual call through its NULL +0x1c (crash at
    # 0x0041185d). A detached root (parent == NULL) gets no keyboard focus handling until
    # the next screen installs a new root.
    0x004427CF: (
        "{ uint32_t _root = mem_r32(c->esi + 0x24u); "
        "if (_root != 0u && mem_r32(_root + 0xcu) == 0u) { static uint32_t _logged; "
        "if (_logged != _root) { _logged = _root; fprintf(stderr, \"[fix] GUI focus root 0x%08x is detached "
        "(screen closed by loading a game) - keyboard focus disabled until the next screen\\n\", _root); } "
        "goto L_0044292b; } } /* bugfix: stale Tab-navigation root */"
    ),
    # 0x00440920 key-event -> key-binding dispatcher (registry std::map at this+0x20, keyed
    # by the event code vk | 0x10000 | extended bit). Scenes that steer the Moorhuhn with the
    # cursor keys (dark corridor behind the sawn grate and three more, set up in
    # 0x00408180 / 0x00413d50 / 0x00415f40 / 0x00417040) register the arrows as
    # 0x01010025..28. QoL: W/A/S/D are looked up as those arrow bindings. Only this
    # lookup sees the remapped code - the event itself (focus manager, text entry) still
    # carries W/A/S/D, and the game binds none of these letters itself.
    0x00440978: (
        "switch (c->eax) { case 0x10057u: c->eax = 0x1010026u; break; "  # W -> Up
        "case 0x10041u: c->eax = 0x1010025u; break; "                    # A -> Left
        "case 0x10053u: c->eax = 0x1010028u; break; "                    # S -> Down
        "case 0x10044u: c->eax = 0x1010027u; break; default: break; } "  # D -> Right
        "/* QoL: WASD as cursor keys for key bindings */"
    ),
    # 0x004115a0 save-screen (singleton 0x00526a28) message handler, this = screen + 8.
    # Defence in depth for the bug above: messages while the screen is closed (+0x1c ==
    # NULL) are consumed instead of reaching code that dereferences the screen.
    0x004115AF: (
        "if (mem_r32(c->esi - 8u + 0x1cu) == 0u) { fprintf(stderr, \"[fix] ignored message %u "
        "to the closed save screen\\n\", mem_r32(c->eax + 0x20u)); goto L_004116de; } "
        "/* bugfix: message to closed save screen */"
    ),
}

# Guest globals used as hand-rolled InterlockedIncrement-style spinlock counters
# (-1 = free, 0 = held) via NON-atomic `mov r32,[X]; inc/dec r32; mov [X],r32`.
# MSVC inlined the acquire/release into every ObjMutex method, so patching the
# out-of-line MetaSpinLock::Lock alone is not enough. Every such triple is lifted
# as one atomic RMW, and every other plain store to X (the original's forced
# "lock-steal" after 200 failed Sleep(1) retries) becomes a blocking acquire.
ATOMIC_COUNTER_GLOBALS = {
    0x0051B3B8,  # MetaSpinLock counter guarding all ObjMutex state (owner TID at 0x00527824)
}


def _is_abs_mem(op, va):
    return (op.type == cs_x86.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0
            and op.mem.segment == 0 and (op.mem.disp & 0xffffffff) == va)


def _touches_global(ins, va):
    return any(_is_abs_mem(op, va) for op in ins.operands)


def find_atomic_counter_rewrites(sorted_vas, local_insns, jump_targets):
    """Return {va: (c_stmts, covered_vas)} for spinlock-counter accesses.

    Raises ValueError on any access pattern to an ATOMIC_COUNTER_GLOBALS address
    that is not a recognized RMW triple or plain store, so new patterns are never
    silently lifted non-atomically.
    """
    rewrites = {}
    covered = set()
    for idx, va in enumerate(sorted_vas):
        if va in covered:
            continue
        ins = local_insns[va]
        for gva in ATOMIC_COUNTER_GLOBALS:
            if not _touches_global(ins, gva):
                continue
            ops = ins.operands
            # RMW triple: mov r32,[X] ; inc/dec r32 ; mov [X],r32 (contiguous, no labels inside)
            if (ins.mnemonic == "mov" and len(ops) == 2 and ops[0].type == cs_x86.X86_OP_REG
                    and ops[0].reg in REG32_MAP and _is_abs_mem(ops[1], gva) and idx + 2 < len(sorted_vas)):
                i1, i2 = local_insns[sorted_vas[idx + 1]], local_insns[sorted_vas[idx + 2]]
                reg = ops[0].reg
                if (i1.address == va + ins.size and i2.address == i1.address + i1.size
                        and i1.address not in jump_targets and i2.address not in jump_targets
                        and i1.mnemonic in ("inc", "dec") and i1.operands[0].type == cs_x86.X86_OP_REG
                        and i1.operands[0].reg == reg and i2.mnemonic == "mov"
                        and _is_abs_mem(i2.operands[0], gva) and i2.operands[1].type == cs_x86.X86_OP_REG
                        and i2.operands[1].reg == reg):
                    fetch = "__atomic_fetch_add" if i1.mnemonic == "inc" else "__atomic_fetch_sub"
                    alu = "alu_inc32" if i1.mnemonic == "inc" else "alu_dec32"
                    stmt = (f"{{ uint32_t _o = {fetch}((uint32_t *)(g_mem + 0x{gva:x}u), 1u, __ATOMIC_SEQ_CST); "
                            f"{write_op(ops[0], f'{alu}(c, _o)')} }} /* atomic {i1.mnemonic} spinlock counter */")
                    rewrites[va] = ([stmt], {i1.address, i2.address})
                    covered.update((i1.address, i2.address))
                    break
            # Plain store (forced lock-steal `mov [X], 0` / `mov [X], reg`): block until acquired instead
            if (ins.mnemonic == "mov" and len(ops) == 2 and _is_abs_mem(ops[0], gva)
                    and (ops[1].type == cs_x86.X86_OP_REG or (ops[1].type == cs_x86.X86_OP_IMM and ops[1].imm == 0))):
                rewrites[va] = ([f"guest_spin_acquire(0x{gva:08x}u); /* was forced lock-steal */"], set())
                break
            raise ValueError(f"Unrecognized access to atomic counter 0x{gva:08x} at 0x{va:08x}: "
                             f"{ins.mnemonic} {ins.op_str}")
    return rewrites



# ---- Flag liveness (dead flag elimination) ----
# Bits: CF=1 ZF=2 SF=4 OF=8 PF=16. A flag-setting instruction whose written flags are
# all dead (no read on any path before being overwritten) is emitted with a flag-free
# nf_* helper (runtime/cpu.h). Kill sets mirror exactly what the alu_* helpers write
# unconditionally (shift/rotate by CL or 0 and rep cmps/scas may write nothing -> no kill).
# Conservative boundaries treat ALL flags as live: call, ret, tail/indirect jumps,
# HOOKABLE edges and GUEST_BUGFIX_INSERTS (inserted early returns).
F_CF, F_ZF, F_SF, F_OF, F_PF = 1, 2, 4, 8, 16
F_ALL = 31
FLAG_FIELDS = (("cf", F_CF), ("zf", F_ZF), ("sf", F_SF), ("of", F_OF), ("pf", F_PF))
FLAG_ELIM = os.environ.get("MHA2_LIFT_NOFLAGELIM", "") == ""
FLAG_CHECK = os.environ.get("MHA2_LIFT_FLAGCHECK", "") != ""


def flags_in_expr(expr):
    return sum(bit for name, bit in FLAG_FIELDS if f"c->{name}" in expr)


def flag_effects(ins):
    """(use, kill) flag masks of one instruction as emitted by lift_instruction."""
    m, ops = ins.mnemonic, ins.operands
    if m in JCC_COND:
        return flags_in_expr(JCC_COND[m]), 0
    if m in SETCC_COND:
        return flags_in_expr(SETCC_COND[m]), 0
    if m.startswith("cmov") and ("j" + m[4:]) in JCC_COND:
        return flags_in_expr(JCC_COND["j" + m[4:]]), 0
    if m in ("add", "sub", "and", "or", "xor", "cmp", "test", "neg", "lock add"):
        return 0, F_ALL
    if m in ("adc", "sbb"):
        return F_CF, F_ALL
    if m in ("inc", "dec", "lock inc", "lock dec"):
        return 0, F_ZF | F_SF | F_OF | F_PF
    if m in ("shl", "sal", "shr", "sar", "shld", "shrd", "rol", "ror", "rcl", "rcr"):
        use = F_CF if m in ("rcl", "rcr") else 0
        cnt_op = ops[-1] if (len(ops) == 2 and m not in ("shld", "shrd")) or len(ops) == 3 else None
        if cnt_op is None:
            n = 1
        elif cnt_op.type == cs_x86.X86_OP_IMM:
            n = cnt_op.imm & 31
        else:
            return use, 0  # count in CL: may write nothing
        if n == 0:
            return use, 0
        base = F_CF if m in ("rol", "ror", "rcl", "rcr") else (F_CF | F_ZF | F_SF | F_PF)
        return use, base | (F_OF if n == 1 else 0)
    if m in ("mul", "imul"):
        return 0, F_CF | F_OF
    if m in ("bt", "bts", "btr", "btc"):
        return 0, F_CF
    if m in ("bsf", "bsr"):
        return 0, F_ZF
    if m in ("clc", "stc"):
        return 0, F_CF
    if m == "cmc":
        return F_CF, F_CF
    if m == "sahf":
        return 0, F_CF | F_ZF | F_SF | F_PF
    if m == "lahf":
        return F_CF | F_ZF | F_SF | F_PF, 0
    if m == "pushfd":
        return F_ALL, 0
    if m == "popfd":
        return 0, F_ALL
    if m in ("call", "ret", "retn"):
        return F_ALL, 0
    return 0, 0


def flag_successors(ins, local_set, switch_tables):
    """Local successor VAs, plus True if control may leave the function (all flags live)."""
    m, ops = ins.mnemonic, ins.operands
    nxt = ins.address + ins.size
    if m in ("ret", "retn"):
        return [], True
    if m in ("int3", "ud2", "hlt"):
        return [], False
    succ, leaves = [], False

    def edge(t):
        nonlocal leaves
        if t in local_set and t not in HOOKABLE_FUNCS:
            succ.append(t)
        else:
            leaves = True

    if m == "jmp":
        if ops[0].type == cs_x86.X86_OP_IMM:
            edge(ops[0].imm & 0xffffffff)
        else:
            leaves = True
            if ins.address in switch_tables:
                for t in set(switch_tables[ins.address][1]):
                    edge(t)
        return succ, leaves
    if m in JCC_COND or m == "loop":
        edge(ops[0].imm & 0xffffffff)
    edge(nxt)
    return succ, leaves


def compute_flag_liveness(sorted_vas, local_insns, local_set, switch_tables):
    """Backward dataflow; returns {va: live_out_mask}."""
    eff, succs = {}, {}
    for va in sorted_vas:
        ins = local_insns[va]
        use, kill = flag_effects(ins)
        if va in GUEST_BUGFIX_INSERTS:
            use = F_ALL
        eff[va] = (use, kill)
        succs[va] = flag_successors(ins, local_set, switch_tables)
    live_in = {va: 0 for va in sorted_vas}
    live_out = {va: 0 for va in sorted_vas}
    changed = True
    while changed:
        changed = False
        for va in reversed(sorted_vas):
            s_list, leaves = succs[va]
            out = F_ALL if leaves else 0
            for t in s_list:
                out |= live_in.get(t, F_ALL)
            use, kill = eff[va]
            inn = use | (out & ~kill)
            if out != live_out[va] or inn != live_in[va]:
                live_out[va], live_in[va] = out, inn
                changed = True
    return live_out, eff


def lift_function(pe, md, func_va, func_entries_set, switch_tables):
    # Collect all local instructions belonging to this function via intra-procedural BFS
    local_insns = {}  # va -> capstone insn
    jump_targets = set()
    queue = [func_va]
    visited = set()

    while queue:
        cur = queue.pop()
        while pe.text_va <= cur < pe.text_end and cur not in visited:
            visited.add(cur)
            off = cur - pe.text_va
            insns = list(md.disasm(pe.text_bytes[off : off + 16], cur, count=1))
            if not insns:
                break
            ins = insns[0]
            local_insns[cur] = ins
            m = ins.mnemonic
            nxt = cur + ins.size

            if m == "jmp":
                op = ins.operands[0]
                if op.type == cs_x86.X86_OP_IMM:
                    tgt = op.imm & 0xffffffff
                    # Tail-call if jumping to a different function entry
                    if tgt != func_va and tgt in func_entries_set and tgt not in jump_targets:
                        break
                    if pe.text_va <= tgt < pe.text_end:
                        jump_targets.add(tgt)
                        if tgt not in visited:
                            queue.append(tgt)
                elif cur in switch_tables:
                    _, tgts = switch_tables[cur]
                    for t in tgts:
                        jump_targets.add(t)
                        if t not in visited:
                            queue.append(t)
                break
            elif m in JCC_COND or m == "loop":
                op = ins.operands[0]
                if op.type == cs_x86.X86_OP_IMM:
                    tgt = op.imm & 0xffffffff
                    if pe.text_va <= tgt < pe.text_end and not (tgt != func_va and tgt in func_entries_set and abs(tgt - func_va) > 0x2000):
                        jump_targets.add(tgt)
                        if tgt not in visited:
                            queue.append(tgt)
                cur = nxt
            elif m in ("ret", "retn", "int3", "ud2", "hlt"):
                break
            else:
                cur = nxt

    sorted_vas = sorted(local_insns.keys())
    local_set = set(sorted_vas)
    if sorted_vas and sorted_vas[0] != func_va:
        jump_targets.add(func_va)
    # Pre-scan fallthrough gotos so every target gets a label
    for idx, va in enumerate(sorted_vas):
        ins = local_insns[va]
        m = ins.mnemonic
        if m not in ("jmp", "ret", "retn", "int3", "ud2", "hlt"):
            nxt = va + ins.size
            next_in_list = sorted_vas[idx + 1] if idx + 1 < len(sorted_vas) else None
            if nxt != next_in_list and nxt in local_set:
                jump_targets.add(nxt)

    flag_live_out, flag_eff = compute_flag_liveness(sorted_vas, local_insns, local_set, switch_tables)
    atomic_rewrites = find_atomic_counter_rewrites(sorted_vas, local_insns, jump_targets)
    atomic_covered = set()
    for _, cov in atomic_rewrites.values():
        atomic_covered |= cov

    out = [f"void sub_{func_va:08x}(CpuCtx *c) {{"]
    if sorted_vas and sorted_vas[0] != func_va:
        out.append(f"  goto L_{func_va:08x};")

    for idx, va in enumerate(sorted_vas):
        ins = local_insns[va]
        if va in jump_targets:
            out.append(f"L_{va:08x}:;")
        if va in GUEST_BUGFIX_INSERTS:
            out.append(f"  {GUEST_BUGFIX_INSERTS[va]}")
        if va in atomic_rewrites:
            stmts = atomic_rewrites[va][0]
        elif va in atomic_covered:
            stmts = []
        else:
            use, kill = flag_eff[va]
            flags_dead = FLAG_ELIM and kill != 0 and (kill & flag_live_out[va]) == 0
            stmts = lift_instruction(ins, pe, func_va, func_entries_set, local_set, switch_tables,
                                     flags_dead=flags_dead)
            if flags_dead and not stmts_set_flags(stmts):
                LIFT_STATS["flag_elim"] += 1
                if FLAG_CHECK:
                    stmts = stmts + [" ".join(f"c->{n} = FLAG_POISON;" for n, b in FLAG_FIELDS if kill & b)]
            if FLAG_CHECK and use and use != F_ALL:
                stmts = [f"flag_check(c, {use}u, 0x{va:08x}u);"] + stmts
        if va in ESI_NULL_GUARD_VAS:
            for st in stmts:
                out.append(f"  if (c->esi) {{ {st} }}")
        else:
            for st in stmts:
                out.append(f"  {st}")
        # If this instruction falls through and next sorted va is not its fallthrough, emit explicit goto
        m = ins.mnemonic
        falls_through = m not in ("jmp", "ret", "retn", "int3", "ud2", "hlt")
        if falls_through:
            nxt = va + ins.size
            next_in_list = sorted_vas[idx + 1] if idx + 1 < len(sorted_vas) else None
            if nxt != next_in_list:
                if nxt in local_set:
                    out.append(f"  goto L_{nxt:08x};")
                elif nxt in HOOKABLE_FUNCS:
                    out.append(f"  g_code_table[0x{nxt - pe.text_va:x}u](c); return;")
                elif nxt in func_entries_set:
                    out.append(f"  sub_{nxt:08x}(c); return;")
    out.append("}")
    return "\n".join(out), len(local_insns)


def main():
    global EXE_PATH, OUT_DIR, JOURNAL_PATH
    ap = argparse.ArgumentParser(description="Static x86-32 PE -> C recompiler for Moorhuhn Adventure 2 (mha2.exe)")
    ap.add_argument("--exe", default=EXE_PATH, help="Path to mha2.exe (default: game_files/mha2.exe)")
    ap.add_argument("--out", default=OUT_DIR, help="Output directory for generated C chunks (default: recomp/gen)")
    ap.add_argument("--standalone", action="store_true", help="Ignore Ghidra journal; discover all functions purely from PE")
    ap.add_argument("--journal", default=JOURNAL_PATH, help="Optional REA/Ghidra decompile-journal.jsonl")
    args = ap.parse_args()

    EXE_PATH = args.exe
    OUT_DIR = args.out
    JOURNAL_PATH = args.journal
    use_ghidra = (not args.standalone) and os.path.exists(JOURNAL_PATH)

    os.makedirs(OUT_DIR, exist_ok=True)
    pe = PEImage(EXE_PATH)
    md = make_cs()
    mode_str = "Ghidra+Standalone" if use_ghidra else "100% Standalone (no Ghidra)"
    print(f"Loaded PE {EXE_PATH} [{mode_str}]: image_base={pe.image_base:#x} entry={pe.entry_va:#x} .text={pe.text_va:#x}..{pe.text_end:#x} imports={len(pe.imports)}")

    func_entries, switch_tables = discover_functions(pe, md, use_ghidra=use_ghidra)
    func_entries_set = set(func_entries)
    print(f"Discovered {len(func_entries)} functions and {len(switch_tables)} switch tables")

    # 1. Write imports.h
    with open(os.path.join(OUT_DIR, "imports.h"), "w") as f:
        f.write("#ifndef MHA2_IMPORTS_H\n#define MHA2_IMPORTS_H\n#include <stdint.h>\n\n")
        f.write(f"#define MHA2_IMPORT_COUNT {len(pe.imports)}\n")
        f.write(f"#define MHA2_ENTRY_VA 0x{pe.entry_va:08x}u\n\n")
        f.write("typedef struct ImportEntry { uint32_t id; uint32_t iat_va; const char *dll; const char *name; } ImportEntry;\n\n")
        f.write("static const ImportEntry g_imports[MHA2_IMPORT_COUNT] = {\n")
        for imp_id, iat_va, dll, fname in pe.imports:
            f.write(f'  {{ {imp_id}u, 0x{iat_va:08x}u, "{dll}", "{fname}" }},\n')
        f.write("};\n\n")
        for imp_id, iat_va, dll, fname in pe.imports:
            safe = f"IMP_{dll.replace('.', '_').upper()}_{fname}"
            f.write(f"#define {safe} {imp_id}u\n")
        f.write("#endif\n")

    # 2. Write funcs.h
    with open(os.path.join(OUT_DIR, "funcs.h"), "w") as f:
        f.write("#ifndef MHA2_FUNCS_H\n#define MHA2_FUNCS_H\n#include \"cpu.h\"\n\n")
        for va in func_entries:
            f.write(f"void sub_{va:08x}(CpuCtx *c);\n")
        f.write("#endif\n")

    # 3. Write dispatch.c
    with open(os.path.join(OUT_DIR, "dispatch.c"), "w") as f:
        f.write('#include "cpu.h"\n#include "funcs.h"\n\n')
        f.write("RecompFn g_code_table[GUEST_TEXT_SIZE];\n\n")
        f.write("void init_code_table(void) {\n")
        for va in func_entries:
            f.write(f"  g_code_table[0x{va - pe.text_va:x}u] = sub_{va:08x};\n")
        f.write("}\n")

    # 4. Lift all functions across NUM_CHUNKS C files
    chunk_files = [open(os.path.join(OUT_DIR, f"chunk_{i:02d}.c"), "w") for i in range(NUM_CHUNKS)]
    for cf in chunk_files:
        cf.write('#include "cpu.h"\n#include "funcs.h"\n\n')

    total_lifted_insns = 0
    errors = []
    for idx, va in enumerate(func_entries):
        try:
            code, n_ins = lift_function(pe, md, va, func_entries_set, switch_tables)
            total_lifted_insns += n_ins
            cf = chunk_files[idx % NUM_CHUNKS]
            cf.write(code)
            cf.write("\n\n")
        except Exception as e:
            errors.append((va, str(e)))
            cf = chunk_files[idx % NUM_CHUNKS]
            cf.write(f'void sub_{va:08x}(CpuCtx *c) {{ (void)c; fprintf(stderr, "LIFT ERROR in 0x{va:08x}: {e}\\n"); abort(); }}\n\n')

    for cf in chunk_files:
        cf.close()

    print(f"Lifted {len(func_entries)} functions ({total_lifted_insns} instructions) into {NUM_CHUNKS} C chunks.")
    print(f"Lift errors: {len(errors)}")
    print(f"Dead flag eliminations: {LIFT_STATS['flag_elim']}" + ("" if FLAG_ELIM else " (disabled)") + (" [FLAGCHECK poison mode]" if FLAG_CHECK else ""))
    for va, err in errors[:30]:
        print(f"  ERROR 0x{va:08x}: {err}")


if __name__ == "__main__":
    main()
