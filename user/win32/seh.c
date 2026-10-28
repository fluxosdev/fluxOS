/*
 * Structured exception handling: what Windows does when a program faults
 * or raises an exception. The handlers registered on the chain at fs:0
 * are called in turn, each with the exception record and a CONTEXT of
 * the registers at the fault; one may fix the context and ask for
 * execution to go on, or (the usual case for C++ and __try) unwind the
 * stack through RtlUnwind and jump into its own code, never returning
 * here. A program's own filter from SetUnhandledExceptionFilter comes
 * last; without one, the program ends with a message.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32.h"

#define ExceptionContinueExecution 0
#define ExceptionContinueSearch 1
#define EXCEPTION_EXECUTE_HANDLER 1
#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_CONTINUE_EXECUTION ((uint32_t)-1)

gp_t unhandled_filter;
gp_t vectored_handlers[8];
int nvectored;
uint32_t gate_read_fs(void);

uint32_t guest_fs(void)
{
    return gate_win_fs;
}

static const char *code_name(uint32_t code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_BREAKPOINT: return "breakpoint";
    case EXCEPTION_SINGLE_STEP: return "single step";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer division by zero";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "floating-point division by zero";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case 0xE06D7363: return "C++ exception";
    default: return "exception";
    }
}

static void describe(const struct exception_record *rec, char *out, size_t size)
{
    struct module *m = NULL;
    for (struct module *q = modules; q; q = q->next)
        if (rec->address >= q->base && rec->address < q->base + q->size)
            m = q;
    size_t n = (size_t)snprintf(out, size, "%s (0x%08x) at address 0x%08x", code_name(rec->code), rec->code, rec->address);
    if (m)
        n += (size_t)snprintf(out + n, size - n, " (%s+0x%x)", m->name, rec->address - m->base);
    if (rec->code == EXCEPTION_ACCESS_VIOLATION && rec->nparams >= 2)
        snprintf(out + n, size - n, " %s address 0x%08x", rec->params[0] ? "writing" : "reading", rec->params[1]);
}

static void terminate(const struct exception_record *rec)
{
    char msg[256];
    describe(rec, msg, sizeof(msg));
    wlog("the program stopped: %s", msg);
    gui_exit();
    crt_flush_all();
    fflush(stderr);
    _exit(rec->code == EXCEPTION_BREAKPOINT ? 3 : (int)(0xC0 | (rec->code & 0x3F)));
}

static void regs_to_context(const struct guest_regs *r, struct context *c)
{
    memset(c, 0, sizeof(*c));
    c->flags = CONTEXT_FULL;
    c->gs = 0;
    c->fs = gate_win_fs;
    c->es = c->ds = c->ss = gate_win_ds;
    c->cs = gate_win_cs;
    c->edi = r->edi; c->esi = r->esi; c->ebx = r->ebx; c->edx = r->edx; c->ecx = r->ecx; c->eax = r->eax;
    c->ebp = r->ebp; c->eip = r->eip; c->esp = r->esp; c->eflags = r->eflags;
}

static void context_to_regs(const struct context *c, struct guest_regs *r)
{
    r->edi = c->edi; r->esi = c->esi; r->ebp = c->ebp; r->esp = c->esp; r->ebx = c->ebx;
    r->edx = c->edx; r->ecx = c->ecx; r->eax = c->eax; r->eip = c->eip; r->eflags = c->eflags;
}

/* Copies of the record and context the handlers can see. */
static gp_t guest_copy(const void *p, uint32_t size)
{
    gp_t g = galloc(size);
    if (g)
        memcpy(GP(g), p, size);
    return g;
}

/* Walk the handlers: 1 if one of them wants execution to continue, with ctx
 * updated; 0 if none took it. stack is where the handlers' frames may go (linear). */
static int dispatch(struct exception_record *rec, struct context *ctx, uint32_t stack)
{
    gp_t grec = guest_copy(rec, sizeof(*rec)), gctx = guest_copy(ctx, sizeof(*ctx));
    if (!grec || !gctx)
        return 0;
    struct { gp_t rec, ctx; } ptrs = { grec, gctx };
    gp_t gptrs = guest_copy(&ptrs, sizeof(ptrs));
    for (int i = 0; i < nvectored; i++) {
        uint32_t args[1] = { gptrs };
        uint32_t r = call_guest_on(vectored_handlers[i], 1, args, stack);
        if (r == EXCEPTION_CONTINUE_EXECUTION) {
            memcpy(ctx, GP(gctx), sizeof(*ctx));
            return 1;
        }
    }
    /* the chain at fs:0: {next, handler} records on the program's stack */
    gp_t reg = teb->except_list;
    int depth = 0;
    while (reg != 0xFFFFFFFFu && reg && depth++ < 64) {
        if (!vm_readable(reg, 8))
            break;
        uint32_t *r = GP(reg);
        gp_t next = r[0], handler = r[1];
        if (!handler || !vm_readable(handler, 1))
            break;
        gp_t dispatcher = galloc(4);
        uint32_t args[4] = { grec, reg, gctx, dispatcher };
        wtrace("SEH: handler %08x for frame %08x", handler, reg);
        uint32_t disp = call_guest_on(handler, 4, args, stack);
        if (disp == ExceptionContinueExecution) {
            memcpy(ctx, GP(gctx), sizeof(*ctx));
            return 1;
        }
        reg = next;
    }
    if (unhandled_filter) {
        uint32_t args[1] = { gptrs };
        uint32_t r = call_guest_on(unhandled_filter, 1, args, stack);
        if (r == EXCEPTION_CONTINUE_EXECUTION) {
            memcpy(ctx, GP(gctx), sizeof(*ctx));
            return 1;
        }
    }
    return 0;
}

void seh_fault(uint32_t code, gp_t fault_addr, int write, const struct guest_regs *regs, uint32_t fs)
{
    (void)fs;
    struct exception_record rec;
    memset(&rec, 0, sizeof(rec));
    rec.code = code;
    rec.address = regs->eip;
    if (code == EXCEPTION_ACCESS_VIOLATION) {
        rec.nparams = 2;
        rec.params[0] = (uint32_t)write;
        rec.params[1] = fault_addr;
        /* a stack overflow: the fault is just below the stack */
        if (fault_addr < guest_stack_limit && fault_addr + 0x10000 >= guest_stack_limit)
            rec.code = EXCEPTION_STACK_OVERFLOW;
    }
    struct context ctx;
    regs_to_context(regs, &ctx);
    static int depth;
    if (depth++ > 2)
        terminate(&rec);
    if (api_trace) {
        char msg[256];
        describe(&rec, msg, sizeof(msg));
        wlog("fault: %s", msg);
    }
    uint32_t stack = (regs->esp - 0x400) & ~15u;
    if (stack < guest_stack_limit + 0x1000 || stack > GUEST_TOP)
        stack = guest_stack_base - 0x400;
    if (dispatch(&rec, &ctx, stack + WIN_BASE)) {
        struct guest_regs r;
        context_to_regs(&ctx, &r);
        depth--;
        guest_resume(&r);
    }
    terminate(&rec);
}

void seh_raise(struct exception_record *rec, struct context *ctx)
{
    uint32_t stack = (ctx->esp - 0x400) & ~15u;
    if (stack < guest_stack_limit + 0x1000 || stack > GUEST_TOP)
        stack = guest_stack_base - 0x400;
    if (dispatch(rec, ctx, stack + WIN_BASE)) {
        struct guest_regs r;
        context_to_regs(ctx, &r);
        guest_resume(&r);
    }
    if (rec->flags & EXCEPTION_NONCONTINUABLE)
        terminate(rec);
    terminate(rec);
}

/* RtlUnwind: call the handlers from the top of the chain down to (not
 * including) target_frame with the unwinding flag, unlinking them, then
 * continue at target_ip (or return to the caller if there is none). */
void seh_unwind(gp_t target_frame, gp_t target_ip, struct exception_record *rec, uint32_t retval, struct context *ctx)
{
    struct exception_record local;
    if (!rec) {
        memset(&local, 0, sizeof(local));
        local.code = 0xC0000027;        /* STATUS_UNWIND */
        local.address = ctx->eip;
        rec = &local;
    }
    rec->flags |= EXCEPTION_UNWINDING;
    if (!target_frame)
        rec->flags |= EXCEPTION_EXIT_UNWIND;
    gp_t grec = guest_copy(rec, sizeof(*rec)), gctx = guest_copy(ctx, sizeof(*ctx));
    uint32_t stack = ((ctx->esp - 0x400) & ~15u) + WIN_BASE;
    int depth = 0;
    while (teb->except_list != 0xFFFFFFFFu && teb->except_list && depth++ < 64) {
        gp_t reg = teb->except_list;
        if (reg == target_frame)
            break;
        if (!vm_readable(reg, 8))
            break;
        uint32_t *r = GP(reg);
        gp_t next = r[0], handler = r[1];
        if (target_frame && reg > target_frame)
            break;                      /* past it: a bad frame */
        gp_t dispatcher = galloc(4);
        uint32_t args[4] = { grec, reg, gctx, dispatcher };
        wtrace("SEH: unwinding frame %08x (handler %08x)", reg, handler);
        teb->except_list = next;
        if (handler && vm_readable(handler, 1))
            call_guest_on(handler, 4, args, stack);
    }
    teb->except_list = target_frame ? target_frame : 0xFFFFFFFFu;
    if (target_ip) {
        struct guest_regs r;
        context_to_regs(ctx, &r);
        r.eip = target_ip;
        r.eax = retval;
        guest_resume(&r);
    }
}
