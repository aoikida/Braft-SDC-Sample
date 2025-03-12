/* ----------------------------------------------------------------------------
 Copyright (c) 2013,2014 Diogo Behrens

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
 --------------------------------------------------------------------------- */

#include <cstdio>
#include <cstdlib>
#include <vector>
#include <iostream>
#include <libgen.h> // basename
#include <string.h>
#include <stdarg.h>
#include <sys/time.h>
#include <stdlib.h> // rand_r
#include <cassert>
#include "pin.H"

using namespace std;

/* ----------------------------------------------------------------------------
 * types
 * ------------------------------------------------------------------------- */

typedef enum {NONE, CF, RVAL, WVAL, RADDR, WADDR, RREG, WREG, TXT, FIND} cmd_t;
typedef enum {IN, RA, WA, RR, WR, IT} trigger_t;

/* ----------------------------------------------------------------------------
 * definitions 
 * ------------------------------------------------------------------------- */

#define DIE(X) die(1, "%s:%d: *** %s\n", __FILE__, __LINE__, X);
#define ULLONG unsigned long long

/* ----------------------------------------------------------------------------
 * state
 * ------------------------------------------------------------------------- */

static struct {
    UINT64 instr;   // number of executed instructions
    UINT64 waddr;   // number of addr reads
    UINT64 raddr;   // number of addr writes
    UINT64 rreg;    // number of reg reads
    UINT64 wreg;    // number of reg writes
    UINT64 iter;    // number of iterations
} counters = {0,0,0,0,0,0};

static cmd_t cmd            = NONE; // command to be executed
static UINT64 trigger       = 0;    // trigger when the error happens
static trigger_t ttype      = IN;   // trigger type
static UINT64 tip           = 0;    // target IP address
static vector<string> func;         // functions to find
static vector<UINT64> cfunc;        // functions iteration counter
static vector<REG> regs;            // array of scratch registers

static FILE* log_file  = NULL;  // log file (NULL prints on screen)
static bool enabled    = false;
static double start_ts = 0;
static bool injected   = false; // whether error was injected or not
static unsigned seed   = 0;     // seed (0 no random)
static unsigned iseed  = 0;     // initial seed (for information later)
static ADDRINT mask    = 0x1;   // error mask (determined with seed)
static int sel         = -1;    // selector of registers (if set, ignore seed)
static bool detach     = false; // detach after inject
static unsigned target_thread = 0;     // target thread

/* ----------------------------------------------------------------------------
 * state and trampoline for text errors
 * ------------------------------------------------------------------------- */
static const unsigned char trampoline[] = {
    0x41, 0xff, 0xe5,     // jmpq *%r13
    0x00                  // string terminator
};

static unsigned char text[256];


/* ----------------------------------------------------------------------------
 * helper functions
 * ------------------------------------------------------------------------- */

/* select command from command line argument */
static cmd_t
cmd_select(const char* cmd) {
    if (strcmp(cmd, "CF")    == 0) return CF;
    if (strcmp(cmd, "WADDR") == 0) return WADDR;
    if (strcmp(cmd, "RADDR") == 0) return RADDR;
    if (strcmp(cmd, "WVAL")  == 0) return WVAL;
    if (strcmp(cmd, "RVAL")  == 0) return RVAL;
    if (strcmp(cmd, "RREG")  == 0) return RREG;
    if (strcmp(cmd, "WREG")  == 0) return WREG;
    if (strcmp(cmd, "TXT")   == 0) return TXT;
    if (strcmp(cmd, "FIND")  == 0) return FIND;
    return NONE;
}

/* select trigger type from command line argument */
static trigger_t
ttype_select(const char* ttype) {
    if (strcmp(ttype, "WA") == 0) return WA;
    if (strcmp(ttype, "RA") == 0) return RA;
    if (strcmp(ttype, "RR") == 0) return RR;
    if (strcmp(ttype, "WR") == 0) return WR;
    if (strcmp(ttype, "IN") == 0) return IN;
    if (strcmp(ttype, "IT") == 0) return IT;
    PIN_ExitProcess(1);
    return IT;
}

/* read current time in seconds */
static inline double
now()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return ((double) tv.tv_sec) + ((double)tv.tv_usec)/1000000.0;
}

/* log informantion */
static void
info(CONTEXT* ctx, THREADID id, ADDRINT ip, const char* fmt, ...)
{
    INT32 col, line;
    std::string fname;
    PIN_LockClient();
    PIN_GetSourceLocation(ip, &col, &line, &fname);
    PIN_UnlockClient();
    char* file = basename((char*) fname.c_str());

    char bfmt [1024];
    sprintf(bfmt,
            "[%s:%5d, IP = %p, i = %llu, wa = %llu, ra = %llu, "
            "rr = %llu, wr = %llu, it = %llu, t = %d]\n"
            "\t%s\n",
            file, line, (void*) ip,
            (ULLONG) counters.instr,
            (ULLONG) counters.waddr,
            (ULLONG) counters.raddr,
            (ULLONG) counters.rreg,
            (ULLONG) counters.wreg,
            (ULLONG) counters.iter,
            id,
            fmt);

    // write into the log file if log_file set, otherwise write to stderr
    if (log_file) {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(log_file, bfmt, ap);
        va_end(ap);
    } else {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(stderr, bfmt, ap);
        va_end(ap);
    }

    // breakpoint in debugger (if connected)
    if (ctx) PIN_ApplicationBreakpoint(ctx, id, FALSE, "fault injected");
}

/* fatal error happened, terminate */
static void
die(int retcode, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    PIN_ExitProcess(retcode);
}

/* get scratch registers */
static REG
get_reg(UINT32 index)
{
    while (index >= regs.size()) {
        REG reg = PIN_ClaimToolRegister();
        if (reg == REG_INVALID()) {
            DIE("No registers left");
        }
        regs.push_back(reg);
    }
    return regs[index];
}

/* return 1 if the current thread is the desired target thread */
static ADDRINT
right_thread(THREADID id)
{
    if (id == target_thread) return 1;
    else return 0;
}

/* disable further injections and, optionally, detaches Pin */
static VOID
mark_injected()
{
    injected = true;
    // keep track of counters or detach PIN?
    if (detach) PIN_Detach();
}

/* ----------------------------------------------------------------------------
 * count and find functions
 * ------------------------------------------------------------------------- */

/* count occurences of different events */
static VOID count_instr(THREADID id) { if (right_thread(id)) counters.instr++;}
static VOID count_waddr(THREADID id) { if (right_thread(id)) counters.waddr++;}
static VOID count_raddr(THREADID id) { if (right_thread(id)) counters.raddr++;}
static VOID count_rreg(THREADID id)  { if (right_thread(id)) counters.rreg++; }
static VOID count_wreg(THREADID id)  { if (right_thread(id)) counters.wreg++; }
static VOID count_iter(THREADID id)  { if (right_thread(id)) counters.iter++; }

/* count occurences of an IP address. Faster than IfCall and ThenCall */
static VOID
count_ip(THREADID id, ADDRINT ip) { if (ip == tip) count_iter(id); }

/* find any instance of an IP address */
static ADDRINT
find_ip(THREADID id, ADDRINT ip)
{ if (!right_thread(id)) return 0; else return (ip == tip); }

/* find the k-th iteration of an IP address using INSTR counter */
static ADDRINT
find_ip_instr(THREADID id, ADDRINT ip)
{ if (!right_thread(id)) return 0;
    else return (ip == tip) && (trigger <= counters.instr); }

/* find the k-th iteration of an IP address using RREG counter */
static ADDRINT
find_ip_rreg(THREADID id, ADDRINT ip)
{ if (!right_thread(id)) return 0;
    else return (ip == tip) && (trigger <= counters.rreg); }

/* find the k-th iteration of an IP address using WREG counter */
static ADDRINT
find_ip_wreg(THREADID id, ADDRINT ip)
{ if (!right_thread(id)) return 0;
    else return (ip == tip) && (trigger <= counters.wreg); }

/* find the k-th iteration of an IP address using WADDR counter */
static ADDRINT
find_ip_waddr(THREADID id, ADDRINT ip)
{ if (!right_thread(id)) return 0;
    else return (ip == tip) && (trigger <= counters.waddr); }

/* find the k-th iteration of an IP address using RADDR counter */
static ADDRINT
find_ip_raddr(THREADID id, ADDRINT ip)
{ if (!right_thread(id)) return 0;
    else return (ip == tip) && (trigger <= counters.raddr); }

/* find the k-th iteration of an IP address using RADDR counter */
static ADDRINT
find_ip_iter(THREADID id, ADDRINT ip)
{  if (!right_thread(id)) return 0;
    else return (ip == tip) && (trigger <= counters.iter); }

/* find INSTR counter */
static ADDRINT
find_instr(THREADID id, ADDRINT ip)
{ if (!right_thread(id)) return 0; else return (trigger <= counters.instr); }

/* find RREG counter */
static ADDRINT
find_rreg(THREADID id, ADDRINT ip)
{ if (!right_thread(id)) return 0; else return (trigger <= counters.rreg); }

/* find WREG counter */
static ADDRINT
find_wreg(THREADID id, ADDRINT ip)
{ if (!right_thread(id)) return 0; else return (trigger <= counters.wreg); }

/* find WADDR counter */
static ADDRINT
find_waddr(THREADID id, ADDRINT ip)
{ if (!right_thread(id)) return 0; else return (trigger <= counters.waddr); }

/* find RADDR counter */
static ADDRINT
find_raddr(THREADID id, ADDRINT ip)
{ if (!right_thread(id)) return 0; else return (trigger <= counters.raddr); }

/* print information of found IP address (and terminate) */
static VOID
found_ip(CONTEXT* ctx, THREADID id, ADDRINT ip, UINT32 raddr, UINT32 waddr,
         UINT32 rreg, UINT32 wreg, bool terminate)
{
    info(ctx, id, ip, "raddr = %d, waddr = %d, rreg = %d, wreg = %d",
         raddr,
         waddr,
         rreg,
         wreg
        );
    if (terminate) PIN_ExitApplication(0);
}

/* ----------------------------------------------------------------------------
 * fault injection
 * ----------------------------------------------------------------------------
 * inject_* functions are called when the trigger is found.
 * ------------------------------------------------------------------------- */

static VOID
inject_txt(THREADID id, CONTEXT* ctx, ADDRINT ins, ADDRINT next, UINT32 size)
{
    if (!right_thread(id)) return;
    if (injected) return;
    mark_injected();

    // get current IP from context
    ADDRINT ip = (ADDRINT) PIN_GetContextReg(ctx, REG_INST_PTR);

    // copy original function in text area
    memcpy(text, (void*) ip, size);

    // add an absolute jump back to the next instruction using r13
    memcpy(text + size, trampoline, sizeof(trampoline));

    // adapt mask to fit instruction size
    assert (size <= 8);
    if (size < 8) {
        ADDRINT tmp = mask % (1<<size);
        // if the mask became 0, use mask 1 instead (unless it was
        // given as 0 from user input)
        if (tmp == 0 && mask != 0) mask = 0x01;
        else mask = tmp;
    }

    // get target byte and make idx fit instruction size
    int idx;
    if (sel >= 0) {
        idx = sel;
    } else {
        if (!seed) idx = 0;
        else idx = rand_r(&seed);
    }
    idx %= size;

    // calculate corrupted byte
    unsigned char nword = text[idx] ^ mask;

    // log info
    info(ctx, id, ip, "ip' = %p, size = %u, mask = %llu, idx = %d, "
         "byte = %u, byte' = %u",
         next, size, (ULLONG) mask, idx, text[idx], nword);

    // corrupt code
    text[idx] = nword;

    // set IP to point to code area and set r13 to the return address
    ADDRINT aip = (ADDRINT) &text;
    PIN_SetContextReg(ctx, REG_INST_PTR, aip);
    PIN_SetContextReg(ctx, REG_R13, next);

    //info(id, ip, "ip = %p, ip' = %p", (void*) ip,(void*) aip);

    // jumpt to new context
    PIN_ExecuteAt(ctx);
}

static VOID
inject_cf(THREADID id, CONTEXT* ctx)
{
    if (!right_thread(id)) return;
    if (injected) return;
    mark_injected();

    ADDRINT ip  = (ADDRINT) PIN_GetContextReg(ctx, REG_INST_PTR);
    ADDRINT aip = ip ^ mask;
    PIN_SetContextReg(ctx, REG_INST_PTR, aip);

    info(ctx, id, ip, "ip = %p, ip' = %p", (void*) ip,(void*) aip);

    // jumpt to new context
    PIN_ExecuteAt(ctx);
}

static VOID
inject_value(CONTEXT* ctx, THREADID id, ADDRINT ip, ADDRINT addr,
             UINT32 access, UINT32 size, UINT32 op)
{
    if (!right_thread(id)) return;
    if (injected) return;
    mark_injected();

    uint64_t correct  = *(uint64_t*) addr;
    uint64_t error    = correct ^ mask;
    *(uint64_t*) addr = error;

    info(ctx, id, ip, "access = %s, size = %u, value = %llu, value' = %llu,"
         " addr = %p, op = %u",
         access & 2 ? "write" : "read", size,
         (ULLONG) correct, (ULLONG) error,
         (void*) addr, op);
}

static ADDRINT
inject_addr(CONTEXT* ctx, THREADID id, ADDRINT ip, REG reg, ADDRINT addr,
            UINT32 size, UINT32 access, UINT32 op)
{
    if (!right_thread(id)) return addr;
    if (injected) return addr;
    if (ttype == IN && !find_instr(id, ip)) return addr;
    if (ttype == IT && !find_ip_iter(id, ip)) return addr;
    mark_injected();

    ADDRINT addrp = addr ^ mask;

    info(NULL, id, ip, "access = %s, size = %u, addr = %p, addr' = %p, op = %u",
         access & 2 ? "write" : "read", size,
         (void*) addr,
         (void*) addrp,
         op);
    return addrp;
}

static VOID
inject_reg(THREADID id, ADDRINT ip, CONTEXT* ctx, REG reg)
{
    if (!right_thread(id)) return;
    if (injected) return;
    mark_injected();

    const string& rname = REG_StringShort(reg);
    reg = REG_FullRegName(reg);

    //avoid 0x100 if register is RFLAGS

    ADDRINT aip = (ADDRINT) PIN_GetContextReg(ctx, REG_INST_PTR);
    ADDRINT rv  = (ADDRINT) PIN_GetContextReg(ctx, reg);
    ADDRINT rvx = rv ^ mask;
    PIN_SetContextReg(ctx, reg, rvx);

    info(ctx, id, ip, "at ip %p, %s = %p, %s' = %p",
         (void*) aip,
         rname.c_str(), (void*) rv,
         rname.c_str(), (void*) rvx
        );

    // jump to new context
    PIN_ExecuteAt(ctx);
}

static VOID
inject_bp(CONTEXT* ctx, THREADID id)
{
    PIN_ApplicationBreakpoint(ctx, id, FALSE, "fault injected");
}

/* ----------------------------------------------------------------------------
 * instrumentation
 * ----------------------------------------------------------------------------
 * instrument_* functions are called the first time an instructions is
 * executed, ie, when the instruction is not yet in Pin's cache.  These
 * functions add conditional calls to the injection functions.  There is one
 * instrument_x function for each command.
 * ------------------------------------------------------------------------- */

// XXX: count_{r,w}{addr,reg} can be improved for speed
static VOID
instrument_count(INS ins, VOID* v)
{
    // count instructions
    INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR) count_instr,
                   IARG_THREAD_ID,
                   IARG_END);

    // count number of iterations of an IP
    if (tip != 0) {
        INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR) count_ip,
                       IARG_THREAD_ID,
                       IARG_INST_PTR,
                       IARG_END);
    }

    if (ttype == WA || ttype == RA) {
        // iterate over memory operands of the instruction
        for (UINT32 op = 0; op < INS_MemoryOperandCount(ins); op++) {

            // determine whether and how operand op is accessed
            // 0 = NONE, 1 = READ, 2 = WRITE, 3 = READ|WRITE)
            UINT32 access =
                (INS_MemoryOperandIsRead(ins, op) ? 1 : 0) |
                (INS_MemoryOperandIsWritten(ins, op) ? 2 : 0);

            // count read
            if (ttype == RA && (access & 1)) {
                INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR) count_raddr,
                               IARG_THREAD_ID,
                               IARG_END);
            }
            // count write
            if (ttype == WA && (access & 2)) {
                INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR) count_waddr,
                               IARG_THREAD_ID,
                               IARG_END);
            }
        }
    }

    if (ttype == RR) {
        // iterave over read registers
        if (INS_HasFallThrough(ins)) {
            for (UINT32 r = 0 ; r < INS_MaxNumRRegs(ins); ++r) {
                INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR) count_rreg,
                               IARG_THREAD_ID,
                               IARG_END);
            }
        }
    }

    if (ttype == WR) {
        // iterave over write registers
        if (INS_HasFallThrough(ins)) {
            for (UINT32 r = 0 ; r < INS_MaxNumWRegs(ins); ++r) {
                INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR) count_wreg,
                               IARG_THREAD_ID,
                               IARG_END);
            }
        }
    }
}

static VOID
insert_trigger(INS ins)
{
    if (trigger == 0) DIE("FATAL: no trigger set.");

    if (tip == 0) {
        // find trigger point
        switch (ttype) {
        case IN:
            INS_InsertIfCall(ins, IPOINT_BEFORE, (AFUNPTR) find_instr,
                             IARG_THREAD_ID,
                             IARG_INST_PTR,
                             IARG_END);
            break;
        case WA:
            INS_InsertIfCall(ins, IPOINT_BEFORE, (AFUNPTR) find_waddr,
                             IARG_THREAD_ID,
                             IARG_INST_PTR,
                             IARG_END);
            break;
        case RA:
            INS_InsertIfCall(ins, IPOINT_BEFORE, (AFUNPTR) find_raddr,
                             IARG_THREAD_ID,
                             IARG_INST_PTR,
                             IARG_END);
            break;
        case RR:
            INS_InsertIfCall(ins, IPOINT_BEFORE, (AFUNPTR) find_rreg,
                             IARG_THREAD_ID,
                             IARG_INST_PTR,
                             IARG_END);
            break;
        case WR:
            INS_InsertIfCall(ins, IPOINT_BEFORE, (AFUNPTR) find_wreg,
                             IARG_THREAD_ID,
                             IARG_INST_PTR,
                             IARG_END);
            break;
        case IT:
            DIE("Can't look for iteration without IP.");
        default:
            DIE("FATAL: not implemented");
        }
    } else {
        // find trigger point with specific IP
        switch (ttype) {
        case IN:
            INS_InsertIfCall(ins, IPOINT_BEFORE, (AFUNPTR) find_ip_instr,
                             IARG_THREAD_ID,
                             IARG_INST_PTR,
                             IARG_END);
            break;
        case WA:
            INS_InsertIfCall(ins, IPOINT_BEFORE, (AFUNPTR) find_ip_waddr,
                             IARG_THREAD_ID,
                             IARG_INST_PTR,
                             IARG_END);
            break;
        case RA:
            INS_InsertIfCall(ins, IPOINT_BEFORE, (AFUNPTR) find_ip_raddr,
                             IARG_THREAD_ID,
                             IARG_INST_PTR,
                             IARG_END);
            break;
        case RR:
            INS_InsertIfCall(ins, IPOINT_BEFORE, (AFUNPTR) find_ip_rreg,
                             IARG_THREAD_ID,
                             IARG_INST_PTR,
                             IARG_END);
            break;
        case WR:
            INS_InsertIfCall(ins, IPOINT_BEFORE, (AFUNPTR) find_ip_wreg,
                             IARG_THREAD_ID,
                             IARG_INST_PTR,
                             IARG_END);
            break;
        case IT:
            INS_InsertIfCall(ins, IPOINT_BEFORE, (AFUNPTR) find_ip_iter,
                             IARG_THREAD_ID,
                             IARG_INST_PTR,
                             IARG_END);
            break;
        default:
            DIE("FATAL: not implemented");
        }
    }
}

static VOID
instrument_find(INS ins, VOID* v)
{
    // find possible faults in this instruction
    UINT32 rregs = INS_MaxNumRRegs(ins);
    UINT32 wregs = INS_MaxNumWRegs(ins);
    UINT32 waddr = 0;
    UINT32 raddr = 0;
    for (UINT32 op = 0; op < INS_MemoryOperandCount(ins); op++) {
        if (INS_MemoryOperandIsRead(ins, op))
            raddr++;
        if (INS_MemoryOperandIsWritten(ins, op) && INS_HasFallThrough(ins))
            waddr++;
    }

    if (trigger != 0) {
        insert_trigger(ins);
        INS_InsertThenCall(ins, IPOINT_BEFORE, (AFUNPTR) found_ip,
                           IARG_CONTEXT,
                           IARG_THREAD_ID,
                           IARG_INST_PTR,
                           IARG_UINT32, raddr,
                           IARG_UINT32, waddr,
                           IARG_UINT32, rregs,
                           IARG_UINT32, wregs,
                           IARG_BOOL, true, /* terminate */
                           IARG_END);
    } else {
        if (tip == 0) DIE("No target IP set");

        // look for all occurences of an IP
        INS_InsertIfCall(ins, IPOINT_BEFORE, (AFUNPTR) find_ip,
                         IARG_THREAD_ID,
                         IARG_INST_PTR,
                         IARG_END);
        INS_InsertThenCall(ins, IPOINT_BEFORE, (AFUNPTR) found_ip,
                           IARG_THREAD_ID,
                           IARG_INST_PTR,
                           IARG_UINT32, raddr,
                           IARG_UINT32, waddr,
                           IARG_UINT32, rregs,
                           IARG_UINT32, wregs,
                           IARG_BOOL, false, /* dont terminate */
                           IARG_END);
    }
}

static VOID
instrument_cf(INS ins, VOID* v)
{
    if (trigger == 0) DIE("FATAL: no trigger set.");

    insert_trigger(ins);
    INS_InsertThenCall(ins, IPOINT_BEFORE, (AFUNPTR) inject_cf,
                       IARG_THREAD_ID,
                       IARG_CONTEXT,
                       IARG_END);
}

static VOID
instrument_txt(INS ins, VOID* v)
{
    if (trigger == 0) DIE("FATAL: no trigger set.");

    insert_trigger(ins);
    INS_InsertThenCall(ins, IPOINT_BEFORE, (AFUNPTR) inject_txt,
                       IARG_THREAD_ID,
                       IARG_CONTEXT,
                       IARG_ADDRINT, INS_Address(ins),
                       IARG_ADDRINT, INS_NextAddress(ins),
                       IARG_UINT32, INS_Size(ins),
                       IARG_END);
}

static VOID
instrument_addr(INS ins, VOID* v)
{
    if (trigger == 0) DIE("FATAL: no trigger set.");


    // find possible faults in this instruction
    UINT32 waddr = 0;
    UINT32 raddr = 0;
    for (UINT32 op = 0; op < INS_MemoryOperandCount(ins); op++) {
        if (INS_MemoryOperandIsRead(ins, op))
            raddr++;
        if (INS_MemoryOperandIsWritten(ins, op) && INS_HasFallThrough(ins))
            waddr++;
    }

    // get target operand
    UINT32 op;
    if (sel >= 0) {
        op = sel;
    } else {
        if (!seed) op = 0;
        else op = rand_r(&seed);
    }

    switch(cmd) {
    case RVAL:
        if (raddr == 0) return;
        op %= raddr;
        insert_trigger(ins);
        INS_InsertThenCall(ins, IPOINT_BEFORE, (AFUNPTR) inject_value,
                           IARG_CONTEXT,
                           IARG_THREAD_ID,
                           IARG_INST_PTR,
                           IARG_MEMORYOP_EA, op,
                           IARG_UINT32, 1,
                           IARG_UINT32, INS_MemoryOperandSize(ins, op),
                           IARG_UINT32, op,
                           IARG_END);
        break;
    case WVAL:
        if (waddr == 0) return;
        op %= waddr;
        insert_trigger(ins);
        INS_InsertThenCall(ins, IPOINT_AFTER, (AFUNPTR) inject_value,
                           IARG_CONTEXT,
                           IARG_THREAD_ID,
                           IARG_INST_PTR,
                           IARG_MEMORYOP_EA, op,
                           IARG_UINT32, 2,
                           IARG_UINT32, INS_MemoryOperandSize(ins, op),
                           IARG_UINT32, op,
                           IARG_END);
        break;
    case RADDR:
        if (raddr == 0) return;
        op %= raddr; /* fall through */
    case WADDR: {
        if (cmd == WADDR) {
            if (waddr == 0) return;
            op %= waddr;
        }
        // determine whether and how operand op is accessed
        // 0 = NONE, 1 = READ, 2 = WRITE, 3 = READ|WRITE)
        UINT32 access =
            (INS_MemoryOperandIsRead(ins, op) ? 1 : 0) |
            (INS_MemoryOperandIsWritten(ins, op) ? 2 : 0);

        REG scratch = get_reg(op);
        INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR) inject_addr,
                       IARG_CONTEXT,
                       IARG_THREAD_ID,
                       IARG_INST_PTR,
                       IARG_UINT32, REG(INS_OperandReg(ins, op)),
                       IARG_MEMORYOP_EA, op,
                       IARG_UINT32, INS_MemoryOperandSize(ins, op),
                       IARG_UINT32, access,
                       IARG_UINT32, op,
                       IARG_RETURN_REGS, scratch,
                       IARG_CALL_ORDER, CALL_ORDER_LAST,
                       IARG_END);
        INS_RewriteMemoryOperand(ins, op, scratch);
        insert_trigger(ins);
        INS_InsertThenCall(ins, IPOINT_AFTER, (AFUNPTR) inject_bp,
                           IARG_CONTEXT,
                           IARG_THREAD_ID,
                           IARG_END);
        break;
    }
    default:
        assert (0 && "invalid cmd in instrument_addr");
    }
}

static VOID
instrument_rreg(INS ins, VOID* v)
{
    if (trigger == 0) DIE("FATAL: no trigger set.");

    if (INS_MaxNumRRegs(ins)) {
        UINT32 r;
        if (sel >= 0) r = sel;
        else {
            r = 0;
        }
        r %= INS_MaxNumRRegs(ins);

        insert_trigger(ins);
        INS_InsertThenCall(ins, IPOINT_BEFORE, (AFUNPTR) inject_reg,
                           IARG_THREAD_ID,
                           IARG_INST_PTR,
                           IARG_CONTEXT,
                           IARG_UINT32, INS_RegR(ins, r),
                           IARG_END);
    }
}

static VOID
instrument_wreg(INS ins, VOID* v)
{
    if (trigger == 0)
        DIE("FATAL: no trigger set.");

    if (INS_HasFallThrough(ins) && INS_MaxNumWRegs(ins)) {
        UINT32 r;
        if (sel >= 0) r = sel;
        else {
            r = 0;
        }

        r %= INS_MaxNumWRegs(ins);

        insert_trigger(ins);
        INS_InsertThenCall(ins, IPOINT_AFTER, (AFUNPTR) inject_reg,
                           IARG_THREAD_ID,
                           IARG_INST_PTR,
                           IARG_CONTEXT,
                           IARG_UINT32, INS_RegW(ins, r),
                           IARG_END);
    }
}

/* ----------------------------------------------------------------------------
 * monitor function
 * ------------------------------------------------------------------------- */

static VOID
func_enter(CONTEXT* ctx, THREADID id, ADDRINT ip, UINT32 fu)
{
    if (!right_thread(id)) return;
    ++cfunc[fu];
    enabled = true;

    string m = "enter " + func[fu];
    info(NULL, id, ip, "%s iteration = %d", m.c_str(), cfunc[fu]);
}


static VOID
func_leave(CONTEXT* ctx, THREADID id, ADDRINT ip, UINT32 fu)
{
    if (!right_thread(id)) return;
    enabled = false;

    string m = "leave " + func[fu];
    info(NULL, id, ip, m.c_str());
}

static VOID
monitor_func(IMG img, VOID* v)
{
    for (UINT32 i = 0; i < func.size(); ++i) {
        cfunc.push_back(0);
        RTN foo = RTN_FindByName(img, func[i].c_str());
        if (RTN_Valid(foo)) {
            RTN_Open(foo);
            RTN_InsertCall(foo, IPOINT_BEFORE, (AFUNPTR) func_enter,
                           IARG_CONTEXT,
                           IARG_THREAD_ID,
                           IARG_INST_PTR,
                           IARG_UINT32, i,
                           IARG_END);

            RTN_InsertCall(foo, IPOINT_AFTER, (AFUNPTR) func_leave,
                           IARG_CONTEXT,
                           IARG_THREAD_ID,
                           IARG_INST_PTR,
                           IARG_UINT32, i,
                           IARG_END);
            RTN_Close(foo);
        }
    }
}

/* ----------------------------------------------------------------------------
 * knobs
 * ------------------------------------------------------------------------- */

KNOB<string> KnobLogFile(
    KNOB_MODE_WRITEONCE, "pintool",
    "log", "NONE",
    "bfi output file");

KNOB<UINT64> KnobTrigger(
    KNOB_MODE_WRITEONCE, "pintool",
    "trigger", "0",
    "trigger point");

KNOB<string> KnobTtype(
    KNOB_MODE_WRITEONCE, "pintool",
    "ttype", "IN",
    "trigger type (IN|RA|WA|WR|IT)");

KNOB<string> KnobCmd(
    KNOB_MODE_WRITEONCE, "pintool",
    "cmd", "NONE",
    "command to execute (NONE|CF|RVAL|WVAL|RADDR|WADDR|RREG|WREG|TXT|FIND)");

KNOB<string> KnobMethods(
    KNOB_MODE_APPEND, "pintool",
    "m", "",
    "monitors when program enters and leaves a function "
    "(multiple functions possible)");

KNOB<UINT64> KnobIP(
    KNOB_MODE_APPEND, "pintool",
    "ip", "",
    "target IP");

KNOB<UINT64> KnobThread(
    KNOB_MODE_WRITEONCE, "pintool",
    "thread", "0",
    "target thread (default 0)");

KNOB<BOOL> KnobDetach(
    KNOB_MODE_WRITEONCE, "pintool",
    "detach", "0",
    "detach PIN after injection (default 0)");

KNOB<UINT64> KnobSeed(
    KNOB_MODE_WRITEONCE, "pintool",
    "seed", "0xDEADBEEF",
    "seed to randomly select registers");

KNOB<UINT64> KnobMask(
    KNOB_MODE_WRITEONCE, "pintool",
    "mask", "0x01",
    "mask used to flip bits upon fault");

KNOB<UINT64> KnobSel(
    KNOB_MODE_WRITEONCE, "pintool",
    "sel", "-1",
    "selector of registers");

/* ----------------------------------------------------------------------------
 * fini
 * ------------------------------------------------------------------------- */

VOID
fini(INT32 code, VOID *v)
{
    if (log_file) {
        fprintf(log_file, "**********************\n");
        fprintf(log_file, "INSTR   = %llu\n",   (ULLONG) counters.instr);
        fprintf(log_file, "WADDR   = %llu\n",   (ULLONG) counters.waddr);
        fprintf(log_file, "RADDR   = %llu\n",   (ULLONG) counters.raddr);
        fprintf(log_file, "RREG    = %llu\n",   (ULLONG) counters.rreg );
        fprintf(log_file, "WREG    = %llu\n",   (ULLONG) counters.wreg );
        fprintf(log_file, "ITER    = %llu\n",   (ULLONG) counters.iter );
        fprintf(log_file, "TRIGGER = %llu\n",   (ULLONG) trigger);
        fprintf(log_file, "TTYPE   = %s\n",     KnobTtype.Value().c_str());
        fprintf(log_file, "COMMAND = %s\n",     KnobCmd.Value().c_str());
        fprintf(log_file, "SEL     = %d\n",     sel);
        fprintf(log_file, "SEED    = %d\n",     iseed);
        fprintf(log_file, "MASK    = 0x%llx\n", (ULLONG) mask);
        fprintf(log_file, "THREAD  = %u\n",     target_thread);
        fprintf(log_file, "ELAPSED = %.2fs\n",  (now() - start_ts));
        fclose(log_file);
    } else {
        fprintf(stderr, "**********************\n");
        fprintf(stderr, "INSTR   = %llu\n",   (ULLONG) counters.instr);
        fprintf(stderr, "WADDR   = %llu\n",   (ULLONG) counters.waddr);
        fprintf(stderr, "RADDR   = %llu\n",   (ULLONG) counters.raddr);
        fprintf(stderr, "RREG    = %llu\n",   (ULLONG) counters.rreg );
        fprintf(stderr, "WREG    = %llu\n",   (ULLONG) counters.wreg );
        fprintf(stderr, "ITER    = %llu\n",   (ULLONG) counters.iter );
        fprintf(stderr, "TRIGGER = %llu\n",   (ULLONG) trigger);
        fprintf(stderr, "TTYPE   = %s\n",     KnobTtype.Value().c_str());
        fprintf(stderr, "COMMAND = %s\n",     KnobCmd.Value().c_str());
        fprintf(stderr, "SEL     = %d\n",     sel);
        fprintf(stderr, "SEED    = %d\n",     iseed);
        fprintf(stderr, "MASK    = 0x%llx\n", (ULLONG) mask);
        fprintf(stderr, "THREAD  = %u\n",     target_thread);
        fprintf(stderr, "ELAPSED = %.2fs\n",  (now() - start_ts));
    }
}

/* ----------------------------------------------------------------------------
 * helper message and main
 * ------------------------------------------------------------------------- */

static INT32
usage()
{
    fprintf(stderr, "BFI: bit-flip injector\n");
    fprintf(stderr, "%s", KNOB_BASE::StringKnobSummary().c_str());
    return -1;
}

int
main(int argc, char * argv[])
{
    PIN_InitSymbols();

    // parse command line and get knob values
    if (PIN_Init(argc, argv)) return usage();
    if (KnobLogFile.Value().compare("NONE") != 0) {
        log_file = fopen(KnobLogFile.Value().c_str(), "w+");
    }

    detach  = KnobDetach.Value();
    target_thread  = KnobThread.Value();
    sel     = KnobSel.Value();
    seed    = KnobSeed.Value();
    iseed   = seed; // save that for later
    mask    = KnobMask.Value();
    trigger = KnobTrigger.Value();
    ttype   = ttype_select(KnobTtype.Value().c_str());
    cmd     = cmd_select(KnobCmd.Value().c_str());
    for (UINT32 i = 0; i < KnobMethods.NumberOfValues(); ++i) {
        func.push_back(KnobMethods.Value(i));
    }
    tip = KnobIP.Value();

    // XXX: add thread initializtion. If multiple threads created warn
    // the user to use the -thread knob.

    // add function monitors if -m knob set at least once
    IMG_AddInstrumentFunction(monitor_func, 0);

    // add trigger counting based on the trigger type -ttype
    INS_AddInstrumentFunction(instrument_count, 0);

    // add conditional command instruction called once trigger reached
    switch (cmd) {
    case FIND:
        INS_AddInstrumentFunction(instrument_find, 0);
        break;
    case CF:
        INS_AddInstrumentFunction(instrument_cf, 0);
        break;
    case WADDR:
    case WVAL:
    case RADDR:
    case RVAL:
        INS_AddInstrumentFunction(instrument_addr, 0);
        break;
    case RREG:
        INS_AddInstrumentFunction(instrument_rreg, 0);
        break;
    case WREG:
        INS_AddInstrumentFunction(instrument_wreg, 0);
        break;
    case TXT:
        INS_AddInstrumentFunction(instrument_txt, 0);
        break;
    case NONE:
        ; // nothing to be done
    }

    // register the cleanup function
    PIN_AddFiniFunction(fini, 0);

    // save time
    start_ts = now();

    // start program passed to pin tool after --
    PIN_StartProgram();
    return 0;
}

