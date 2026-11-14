/* Quark — object files: a program compiled ahead of time, for platforms where nothing may be made executable
 * while running (iOS, some consoles and embedded systems) or for hosts that link everything. quark object writes a COFF (Windows
 * x64) or ELF (Linux x86-64, ARM64) object holding:
 *   .text  the machine code, made in object mode: every address the code needs (the runtime, the host's
 *          functions, the data, the budget...) is read from a table, through a pc-relative load, so the code has
 *          no relocation the loader would have to write into it;
 *   .data  a QkStaticProgram (include/quark.h) and what it points to: the table (filled by qkModuleStatic when
 *          the host starts, as qkLoad does in memory), what each entry is, the text literals, the functions
 *          with their slots, the host functions' names.
 * The host links the object with libquark and calls qkModuleStatic(vm, &symbol). */
#include "qk.h"

#include <stdio.h>
#include <stdlib.h>

bool qkObjectMode;

typedef struct DataReloc { /* an 8-byte address in .data: to .text or .data + addend */
    uint32_t at;
    bool toText;
    uint64_t addend;
} DataReloc;

typedef struct Out {
    QkBuf data;
    DataReloc* relocs;
    int relocCount, relocCap;
} Out;

static void u64(QkBuf* b, uint64_t v) { qkBufAdd(b, &v, 8); }
static void u32(QkBuf* b, uint32_t v) { qkBufAdd(b, &v, 4); }
static void u16(QkBuf* b, uint16_t v) { qkBufAdd(b, &v, 2); }
static void align8(QkBuf* b) {
    while (b->len % 8) qkBufByte(b, 0);
}

static void pointer(Out* o, size_t at, bool toText, uint64_t addend) {
    DataReloc r = {(uint32_t)at, toText, addend};
    QK_PUSH(o->relocs, o->relocCount, o->relocCap, r);
    memcpy(o->data.data + at, &addend, 8); /* COFF keeps the addend in place; ELF's rela ignores it */
}

static size_t text(Out* o, const char* s) {
    size_t at = o->data.len;
    qkBufAdd(&o->data, s, strlen(s) + 1);
    return at;
}

/* The .data section: the QkStaticProgram first (the symbol's place), then all it points to. */
static void layout(Out* o, const MachineCode* mc, const IrProgram* ir) {
    enum { MAGIC, CODE, CODELEN, ENTER, ESCAPE, TABLE, TABLECOUNT, RELOCS, DATA, DATALEN, FNCOUNT, FNS, EXTERNCOUNT, EXTERNS, FIELDS };
    for (int i = 0; i < FIELDS; i++) u64(&o->data, 0);
    uint64_t head[FIELDS] = {0};
    head[MAGIC] = 0x316B72617551ULL; /* "Quark1" */
    head[CODELEN] = mc->code.len;
    head[ENTER] = (uint64_t)mc->enter;
    head[ESCAPE] = (uint64_t)mc->escape;
    head[TABLECOUNT] = (uint64_t)mc->relocCount;
    head[DATALEN] = ir->data.len;
    head[FNCOUNT] = (uint64_t)ir->count;
    head[EXTERNCOUNT] = (uint64_t)ir->externCount;
    memcpy(o->data.data, head, sizeof head);
    pointer(o, 8 * CODE, true, 0);
    size_t table = o->data.len; /* zeros: qkModuleStatic fills them */
    for (int i = 0; i < mc->relocCount; i++) u64(&o->data, 0);
    pointer(o, 8 * TABLE, false, table);
    size_t relocs = o->data.len; /* each table entry's kind and target */
    for (int i = 0; i < mc->relocCount; i++) {
        u64(&o->data, (uint64_t)mc->relocs[i].kind);
        u64(&o->data, (uint64_t)mc->relocs[i].target);
    }
    pointer(o, 8 * RELOCS, false, relocs);
    size_t data = o->data.len;
    qkBufAdd(&o->data, ir->data.data, ir->data.len);
    align8(&o->data);
    pointer(o, 8 * DATA, false, data);
    size_t fns = o->data.len; /* name, offset, parameter slots, result slots, slot kinds */
    for (int i = 0; i < ir->count; i++)
        for (int k = 0; k < 5; k++) u64(&o->data, 0);
    pointer(o, 8 * FNS, false, fns);
    size_t externs = o->data.len;
    for (int i = 0; i < ir->externCount; i++) u64(&o->data, 0);
    pointer(o, 8 * EXTERNS, false, externs);
    for (int i = 0; i < ir->count; i++) {
        const IrFn* f = &ir->fns[i];
        size_t at = fns + 40 * (size_t)i;
        size_t name = text(o, f->name);
        char* kinds = qkNew((size_t)f->paramCount + 2);
        for (int j = 0; j < f->paramCount; j++) kinds[j] = f->kinds[f->params[j]];
        size_t kindsAt = text(o, kinds);
        uint64_t nums[3] = {(uint64_t)mc->fnAt[i], (uint64_t)f->paramCount, (uint64_t)f->resultCount};
        memcpy(o->data.data + at + 8, nums, sizeof nums);
        pointer(o, at, false, name);
        pointer(o, at + 32, false, kindsAt);
    }
    for (int i = 0; i < ir->externCount; i++) pointer(o, externs + 8 * (size_t)i, false, text(o, ir->externs[i].name));
    align8(&o->data);
    o->relocs[0].at = o->relocs[0].at; /* (the table's place is what the code's loads use) */
    o->data.len = o->data.len;
    (void)table;
}

static size_t tableAt(const Out* o) {
    uint64_t at;
    memcpy(&at, o->data.data + 8 * 5, 8);
    return (size_t)at;
}

/* ---- COFF (Windows x64) ------------------------------------------------------------------------------- */

static void coff(const MachineCode* mc, Out* o, const char* symbol, QkBuf* out) {
    size_t table = tableAt(o);
    QkBuf code = {0};
    qkBufAdd(&code, mc->code.data, mc->code.len);
    for (int i = 0; i < mc->relocCount; i++) { /* mov reg, [rip + disp32]: the addend is the entry's place */
        int32_t a = (int32_t)(table + 8 * (size_t)i);
        memcpy(code.data + mc->relocs[i].at, &a, 4);
    }
    int textRelocs = mc->relocCount, dataRelocs = o->relocCount;
    uint32_t headers = 20 + 2 * 40;
    uint32_t textAt = headers, dataAt = textAt + (uint32_t)code.len;
    uint32_t textRelAt = dataAt + (uint32_t)o->data.len, dataRelAt = textRelAt + 10 * (uint32_t)textRelocs;
    uint32_t symAt = dataRelAt + 10 * (uint32_t)dataRelocs;
    u16(out, 0x8664);
    u16(out, 2);
    u32(out, 0);
    u32(out, symAt);
    u32(out, 3); /* .text, .data, the program */
    u16(out, 0);
    u16(out, 0);
    const char* names[2] = {".text", ".data"};
    uint32_t sizes[2] = {(uint32_t)code.len, (uint32_t)o->data.len}, raws[2] = {textAt, dataAt}, rels[2] = {textRelAt, dataRelAt};
    uint32_t counts[2] = {(uint32_t)textRelocs, (uint32_t)dataRelocs};
    uint32_t flags[2] = {0x60500020u, 0xC0500040u}; /* code, execute, read, 16-aligned; data, read, write, 16-aligned */
    for (int s = 0; s < 2; s++) {
        char name[8] = {0};
        memcpy(name, names[s], strlen(names[s]));
        qkBufAdd(out, name, 8);
        u32(out, 0);
        u32(out, 0);
        u32(out, sizes[s]);
        u32(out, raws[s]);
        u32(out, rels[s]);
        u32(out, 0);
        u16(out, (uint16_t)counts[s]);
        u16(out, 0);
        u32(out, flags[s]);
    }
    qkBufAdd(out, code.data, code.len);
    qkBufAdd(out, o->data.data, o->data.len);
    for (int i = 0; i < textRelocs; i++) { /* REL32 to .data (symbol 1) */
        u32(out, (uint32_t)mc->relocs[i].at);
        u32(out, 1);
        u16(out, 4);
    }
    for (int i = 0; i < dataRelocs; i++) { /* ADDR64 to .text (0) or .data (1) */
        u32(out, o->relocs[i].at);
        u32(out, o->relocs[i].toText ? 0 : 1);
        u16(out, 1);
    }
    for (int s = 0; s < 2; s++) { /* section symbols */
        char name[8] = {0};
        memcpy(name, names[s], strlen(names[s]));
        qkBufAdd(out, name, 8);
        u32(out, 0);
        u16(out, (uint16_t)(s + 1));
        u16(out, 0);
        qkBufByte(out, 3); /* static */
        qkBufByte(out, 0);
    }
    size_t len = strlen(symbol); /* the program: by its name, in the string table when longer than 8 */
    if (len <= 8) {
        char name[8] = {0};
        memcpy(name, symbol, len);
        qkBufAdd(out, name, 8);
    } else {
        u32(out, 0);
        u32(out, 4);
    }
    u32(out, 0);
    u16(out, 2);
    u16(out, 0);
    qkBufByte(out, 2); /* external */
    qkBufByte(out, 0);
    u32(out, len <= 8 ? 4 : (uint32_t)(4 + len + 1));
    if (len > 8) qkBufAdd(out, symbol, len + 1);
    qkBufFree(&code);
}

/* ---- ELF (Linux x86-64, ARM64) ----------------------------------------------------------------------- */

static void elf(const MachineCode* mc, Out* o, const char* symbol, QkBuf* out) {
    const bool arm = qkTargetArch == QK_ARCH_ARM64;
    const uint16_t machine = arm ? 183 : 62;
    size_t table = tableAt(o);
    QkBuf rtext = {0}, rdata = {0}, sym = {0}, str = {0}, shstr = {0};
    qkBufByte(&str, 0);
    for (int i = 0; i < mc->relocCount; i++) { /* the loads of the table's entries */
        uint64_t addend = table + 8 * (uint64_t)i;
        if (arm) { /* adrp, then ldr: the page, then the place in it */
            u64(&rtext, (uint64_t)mc->relocs[i].at);
            u64(&rtext, (uint64_t)2 << 32 | 275);
            u64(&rtext, addend);
            u64(&rtext, (uint64_t)mc->relocs[i].at + 4);
            u64(&rtext, (uint64_t)2 << 32 | 286);
            u64(&rtext, addend);
        } else { /* rip + disp32: from the end of the field */
            u64(&rtext, (uint64_t)mc->relocs[i].at);
            u64(&rtext, (uint64_t)2 << 32 | 2);
            u64(&rtext, addend - 4);
        }
    }
    for (int i = 0; i < o->relocCount; i++) {
        u64(&rdata, o->relocs[i].at);
        u64(&rdata, (uint64_t)(o->relocs[i].toText ? 1 : 2) << 32 | (arm ? 257 : 1));
        u64(&rdata, o->relocs[i].addend);
    }
    /* symbols: none, .text, .data (local sections), then the program */
    for (int i = 0; i < 24; i++) qkBufByte(&sym, 0);
    for (int s = 1; s <= 2; s++) {
        u32(&sym, 0);
        qkBufByte(&sym, 3); /* local, section */
        qkBufByte(&sym, 0);
        u16(&sym, (uint16_t)s);
        u64(&sym, 0);
        u64(&sym, 0);
    }
    u32(&sym, (uint32_t)str.len);
    qkBufAdd(&str, symbol, strlen(symbol) + 1);
    qkBufByte(&sym, 0x11); /* global, object */
    qkBufByte(&sym, 0);
    u16(&sym, 2);
    u64(&sym, 0);
    u64(&sym, 8 * 14);
    const char* secNames[] = {"", ".text", ".data", ".rela.text", ".rela.data", ".symtab", ".strtab", ".shstrtab", ".note.GNU-stack"};
    uint32_t nameAt[9];
    for (int i = 0; i < 9; i++) {
        nameAt[i] = (uint32_t)shstr.len;
        qkBufAdd(&shstr, secNames[i], strlen(secNames[i]) + 1);
    }
    const QkBuf* contents[9] = {NULL, &mc->code, &o->data, &rtext, &rdata, &sym, &str, &shstr, NULL};
    uint64_t offsets[9] = {0};
    QkBuf body = {0};
    size_t at = 64;
    for (int i = 1; i < 9; i++) {
        while ((at + body.len) % 16) qkBufByte(&body, 0);
        offsets[i] = at + body.len;
        if (contents[i]) qkBufAdd(&body, contents[i]->data, contents[i]->len);
    }
    while ((at + body.len) % 8) qkBufByte(&body, 0);
    uint64_t shoff = at + body.len;
    static const uint8_t ident[16] = {0x7F, 'E', 'L', 'F', 2, 1, 1, 0};
    qkBufAdd(out, ident, 16);
    u16(out, 1);
    u16(out, machine);
    u32(out, 1);
    u64(out, 0);
    u64(out, 0);
    u64(out, shoff);
    u32(out, 0);
    u16(out, 64);
    u16(out, 0);
    u16(out, 0);
    u16(out, 64);
    u16(out, 9);
    u16(out, 7);
    qkBufAdd(out, body.data, body.len);
    /* section headers: name, type, flags, address, offset, size, link, info, alignment, entry size */
    static const uint32_t types[9] = {0, 1, 1, 4, 4, 2, 3, 3, 1};
    static const uint64_t secFlags[9] = {0, 6, 3, 0x40, 0x40, 0, 0, 0, 0};
    for (int i = 0; i < 9; i++) {
        uint64_t size = contents[i] ? contents[i]->len : 0;
        uint32_t link = i == 3 || i == 4 ? 5 : i == 5 ? 6 : 0;
        uint32_t info = i == 3 ? 1 : i == 4 ? 2 : i == 5 ? 3 : 0;
        uint64_t entsize = i == 3 || i == 4 ? 24 : i == 5 ? 24 : 0;
        u32(out, nameAt[i]);
        u32(out, types[i]);
        u64(out, secFlags[i]);
        u64(out, 0);
        u64(out, i ? offsets[i] : 0);
        u64(out, size);
        u32(out, link);
        u32(out, info);
        u64(out, i == 1 || i == 2 ? 16 : i ? 8 : 0);
        u64(out, entsize);
    }
    qkBufFree(&rtext);
    qkBufFree(&rdata);
    qkBufFree(&sym);
    qkBufFree(&str);
    qkBufFree(&shstr);
    qkBufFree(&body);
}

/* ---- Mach-O (macOS, iOS: ARM64 and x86-64) ------------------------------------------------------------
 * __TEXT,__text then __DATA,__data. The code's loads of the table are relocated against a local symbol at the
 * data's start: ARM64 an ADDEND before each PAGE21 (adrp) and PAGEOFF12 (ldr), x86-64 a SIGNED (rip + disp32)
 * with the offset in place. The data's pointers are UNSIGNED, by section, with the address in place. */
static uint32_t machoReloc(uint32_t symbol, bool pcrel, uint32_t length, bool external, uint32_t type) {
    return symbol | (uint32_t)pcrel << 24 | length << 25 | (uint32_t)external << 27 | type << 28;
}

static void macho(const MachineCode* mc, Out* o, const char* symbol, QkBuf* out) {
    const bool arm = qkTargetArch == QK_ARCH_ARM64;
    size_t table = tableAt(o);
    uint64_t dataAddr = (mc->code.len + 15) & ~(uint64_t)15;
    QkBuf code = {0}, data = {0}, rtext = {0}, rdata = {0}, str = {0};
    qkBufAdd(&code, mc->code.data, mc->code.len);
    qkBufAdd(&data, o->data.data, o->data.len);
    for (int i = 0; i < mc->relocCount; i++) {
        uint32_t at = (uint32_t)mc->relocs[i].at, off = (uint32_t)(table + 8 * (size_t)i);
        if (arm) { /* adrp at, ldr at + 4: each with its addend first */
            u32(&rtext, at);
            u32(&rtext, machoReloc(off, false, 2, false, 10));
            u32(&rtext, at);
            u32(&rtext, machoReloc(0, true, 2, true, 3));
            u32(&rtext, at + 4);
            u32(&rtext, machoReloc(off, false, 2, false, 10));
            u32(&rtext, at + 4);
            u32(&rtext, machoReloc(0, false, 2, true, 4));
        } else {
            memcpy(code.data + at, &off, 4);
            u32(&rtext, at);
            u32(&rtext, machoReloc(0, true, 2, true, 1));
        }
    }
    for (int i = 0; i < o->relocCount; i++) { /* the address in place: the section's own plus the offset */
        uint64_t v = o->relocs[i].toText ? o->relocs[i].addend : dataAddr + o->relocs[i].addend;
        memcpy(data.data + o->relocs[i].at, &v, 8);
        u32(&rdata, o->relocs[i].at);
        u32(&rdata, machoReloc(o->relocs[i].toText ? 1 : 2, false, 3, false, 0));
    }
    qkBufByte(&str, 0);
    uint32_t localName = (uint32_t)str.len;
    qkBufAdd(&str, "ltmp1", 6);
    uint32_t globalName = (uint32_t)str.len;
    qkBufByte(&str, '_');
    qkBufAdd(&str, symbol, strlen(symbol) + 1);
    while (str.len % 8) qkBufByte(&str, 0);
    uint32_t sizeofcmds = (72 + 2 * 80) + 24 + 24 + 80;
    uint32_t textOff = (32 + sizeofcmds + 15) & ~15u;
    uint32_t dataOff = textOff + (uint32_t)dataAddr;
    uint32_t relText = (dataOff + (uint32_t)data.len + 7) & ~7u;
    uint32_t relData = relText + (uint32_t)rtext.len;
    uint32_t symOff = relData + (uint32_t)rdata.len;
    uint32_t strOff = symOff + 2 * 16;
    /* the header */
    u32(out, 0xFEEDFACF);
    u32(out, arm ? 0x0100000C : 0x01000007);
    u32(out, arm ? 0 : 3);
    u32(out, 1); /* an object */
    u32(out, 4);
    u32(out, sizeofcmds);
    u32(out, 0x2000); /* subsections via symbols */
    u32(out, 0);
    /* LC_SEGMENT_64, its two sections */
    u32(out, 0x19);
    u32(out, 72 + 2 * 80);
    for (int i = 0; i < 16; i++) qkBufByte(out, 0);
    u64(out, 0);
    u64(out, dataAddr + data.len);
    u64(out, textOff);
    u64(out, dataAddr + data.len);
    u32(out, 7);
    u32(out, 7);
    u32(out, 2);
    u32(out, 0);
    const char* sect[2][2] = {{"__text", "__TEXT"}, {"__data", "__DATA"}};
    uint64_t addr[2] = {0, dataAddr}, size[2] = {code.len, data.len};
    uint32_t off[2] = {textOff, dataOff}, rel[2] = {relText, relData};
    uint32_t nrel[2] = {(uint32_t)rtext.len / 8, (uint32_t)rdata.len / 8}, flags[2] = {0x80000400u, 0};
    for (int s = 0; s < 2; s++) {
        char name[16] = {0}, seg[16] = {0};
        memcpy(name, sect[s][0], strlen(sect[s][0]));
        memcpy(seg, sect[s][1], strlen(sect[s][1]));
        qkBufAdd(out, name, 16);
        qkBufAdd(out, seg, 16);
        u64(out, addr[s]);
        u64(out, size[s]);
        u32(out, off[s]);
        u32(out, 4); /* 16-aligned */
        u32(out, rel[s]);
        u32(out, nrel[s]);
        u32(out, flags[s]);
        u32(out, 0);
        u32(out, 0);
        u32(out, 0);
    }
    /* LC_BUILD_VERSION: macOS 11 */
    u32(out, 0x32);
    u32(out, 24);
    u32(out, 1);
    u32(out, 11 << 16);
    u32(out, 11 << 16);
    u32(out, 0);
    /* LC_SYMTAB */
    u32(out, 0x2);
    u32(out, 24);
    u32(out, symOff);
    u32(out, 2);
    u32(out, strOff);
    u32(out, (uint32_t)str.len);
    /* LC_DYSYMTAB: one local, one defined external */
    u32(out, 0xB);
    u32(out, 80);
    u32(out, 0);
    u32(out, 1);
    u32(out, 1);
    u32(out, 1);
    u32(out, 2);
    u32(out, 0);
    for (int i = 0; i < 12; i++) u32(out, 0);
    while (out->len < textOff) qkBufByte(out, 0);
    qkBufAdd(out, code.data, code.len);
    while (out->len < dataOff) qkBufByte(out, 0);
    qkBufAdd(out, data.data, data.len);
    while (out->len < relText) qkBufByte(out, 0);
    qkBufAdd(out, rtext.data, rtext.len);
    qkBufAdd(out, rdata.data, rdata.len);
    /* the symbols: the data's start (local), the program (external) */
    u32(out, localName);
    qkBufByte(out, 0x0E);
    qkBufByte(out, 2);
    u16(out, 0);
    u64(out, dataAddr);
    u32(out, globalName);
    qkBufByte(out, 0x0F);
    qkBufByte(out, 2);
    u16(out, 0);
    u64(out, dataAddr);
    qkBufAdd(out, str.data, str.len);
    qkBufFree(&code);
    qkBufFree(&data);
    qkBufFree(&rtext);
    qkBufFree(&rdata);
    qkBufFree(&str);
}

bool qkWriteObject(const MachineCode* mc, const IrProgram* ir, const char* symbol, QkBuf* out) {
    Out o = {0};
    layout(&o, mc, ir);
    if (qkTargetOs == QK_OS_MACOS) macho(mc, &o, symbol, out);
    else if (qkTargetOs == QK_OS_WINDOWS && qkTargetArch == QK_ARCH_X64) coff(mc, &o, symbol, out);
    else if (qkTargetOs == QK_OS_LINUX) elf(mc, &o, symbol, out);
    else {
        qkBufFree(&o.data);
        return false; /* Windows on ARM: not yet */
    }
    qkBufFree(&o.data);
    return true;
}
