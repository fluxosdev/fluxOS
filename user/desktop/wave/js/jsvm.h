/*
 * The bytecode compiler and virtual machine: js.c includes this in place of
 * a tree-walking interpreter, so that it shares js.c's static helpers.
 *
 * A script (or module, or eval'd source) is compiled in two passes.
 *
 * The analysis pass walks the whole syntax tree once, functions inside
 * functions included, and gives every scope its declarations: a function's
 * parameters, vars and function declarations, a block's let, const, class
 * and function declarations, a loop's let, a catch's parameter, a class's
 * own name. Each use of a name is resolved; one that reaches into another
 * function's scope marks the declaration captured. The top level of a
 * script or module is left dynamic: names there are looked up by name at
 * run time, as top-level let and const live on the global object, a
 * module's bindings alias other modules', and eval and new Function add to
 * the global scope.
 *
 * Code generation then happens for each function the first time it is
 * called. A declaration nothing captures lives in a register, a slot of
 * the function's frame on the C stack; a captured one in a slot of a heap
 * environment (struct env), which a scope creates only if it has any.
 * Uses compile to a register number, or to how many environments up and
 * which slot, so nothing is looked up by name. Expressions run on an
 * operand stack in the same frame.
 *
 * The machine (vm_run) runs one call: its frame, registers and operand
 * stack are locals, so a generator or async function, which runs on a
 * coroutine stack of its own (js.c), keeps everything there while it is
 * suspended, and the garbage collector sees it all by scanning stacks.
 * Calls between JavaScript functions recurse into vm_run. Exceptions go
 * through J->throwing as everywhere in the engine: a try pushes a handler,
 * and a throw goes to the innermost one in this frame or out of the call.
 * finally blocks have one shared body, called from each way out of their
 * try, including generator suspension and exceptional completion.
 */

/* ---- the analysis pass ---- */

enum { SC_FUNC, SC_BLOCK, SC_CLASS, SC_NAMED, SC_DYN };

struct vscope;
struct vfunc;
struct vproto;

struct vdecl {
    struct jsstr *name;
    struct vscope *scope;
    uint8_t konst;
    uint8_t captured;                   /* used from another function: in an env slot */
    int reg;                            /* otherwise its register */
    int slot;
};

struct vscope {
    struct vscope *parent;
    struct vfunc *fn;                   /* the function whose code enters it */
    uint8_t kind;
    uint8_t has_env;                    /* makes a heap environment when entered */
    struct vdecl **d;
    int nd, cap;
    int nslots;
};

struct vfunc {
    struct ast *ast;                    /* N_FUNC, or a root (script, module, eval) */
    struct vfunc *parent;
    struct vscope *scope;               /* the body's scope (SC_DYN for a root) */
    struct vscope *params;              /* parameters and defaults, before body vars exist */
    struct vscope *named;               /* a named function expression's own name */
    struct vdecl *args;                 /* `arguments`, made on entry when used */
    struct vscope **scopes;             /* every scope its code enters */
    int nscopes, scap;
    uint8_t is_root, arrow, arguments_param;
    uint8_t needs_env;                  /* the function's scope makes an env (this, super) */
    uint8_t dynamic_eval;
    uint8_t laid_out;
    int nregs;
    struct vproto *proto;               /* once compiled */
};

struct an {
    struct js *J;
    struct vfunc *fn;
    struct vscope *sc;
};

static struct vscope *sc_new(struct an *A, struct vscope *parent, int kind)
{
    struct vscope *s = js_alloc(A->J, sizeof(*s));
    s->parent = parent;
    s->fn = A->fn;
    s->kind = (uint8_t)kind;
    struct vfunc *f = A->fn;
    if (f->nscopes == f->scap) {
        int nc = f->scap ? f->scap * 2 : 4;
        struct vscope **ns = js_alloc(A->J, sizeof(*ns) * (size_t)nc);
        if (f->nscopes)
            memcpy(ns, f->scopes, sizeof(*ns) * (size_t)f->nscopes);
        f->scopes = ns;
        f->scap = nc;
    }
    f->scopes[f->nscopes++] = s;
    return s;
}

static struct vdecl *sc_find(struct vscope *s, struct jsstr *name)
{
    for (int i = 0; i < s->nd; i++)
        if (s->d[i]->name == name)
            return s->d[i];
    for (int i = 0; i < s->nd; i++)
        if (str_eq(s->d[i]->name, name))
            return s->d[i];
    return NULL;
}

/* Declare name in s (once: a second var of the same name is the same). */
static struct vdecl *sc_add(struct js *J, struct vscope *s, struct jsstr *name, int konst)
{
    struct vdecl *d = sc_find(s, name);
    if (d)
        return d;
    d = js_alloc(J, sizeof(*d));
    d->name = name;
    d->scope = s;
    d->konst = (uint8_t)konst;
    d->reg = d->slot = -1;
    if (s->nd == s->cap) {
        int nc = s->cap ? s->cap * 2 : 8;
        struct vdecl **nd = js_alloc(J, sizeof(*nd) * (size_t)nc);
        if (s->nd)
            memcpy(nd, s->d, sizeof(*nd) * (size_t)s->nd);
        s->d = nd;
        s->cap = nc;
    }
    s->d[s->nd++] = d;
    return d;
}

/* Every name a binding pattern declares. */
static void pattern_decls(struct js *J, struct vscope *s, struct ast *p, int konst)
{
    if (!p)
        return;
    switch (p->kind) {
    case N_IDENT:
        sc_add(J, s, p->str, konst);
        return;
    case N_ASSIGN:
        pattern_decls(J, s, p->a, konst);
        return;
    case N_SPREAD:
        pattern_decls(J, s, p->a, konst);
        return;
    case N_ARRAY:
        for (int i = 0; i < p->n; i++)
            if (p->list[i]->kind != N_HOLE)
                pattern_decls(J, s, p->list[i], konst);
        return;
    case N_OBJECT:
        for (int i = 0; i < p->n; i++)
            pattern_decls(J, s, p->list[i]->a, konst);
        return;
    default:
        return;
    }
}

/* The let and const (and class) names of a declaration statement. */
static void lexical_decls(struct js *J, struct vscope *s, struct ast *st)
{
    if (st->kind == S_EXPORT && st->op == EX_DECL)
        st = st->a;
    if (st->kind == S_VAR && st->flags != V_VAR) {
        for (int i = 0; i < st->n; i++) {
            struct ast *d = st->list[i];
            if (d->kind == N_DESTRUCT)
                pattern_decls(J, s, d->b, st->flags == V_CONST);
            else
                sc_add(J, s, d->str, st->flags == V_CONST);
        }
    } else if (st->kind == S_FUNCDECL) {
        sc_add(J, s, st->a->str, 0);
    }
}

/* Function declarations in blocks, which sloppy mode makes visible in the
 * whole function too (not entering nested functions). */
static void block_functions(struct js *J, struct vscope *fs, struct ast *s, int nested)
{
    if (!s)
        return;
    switch (s->kind) {
    case S_FUNCDECL:
        if (nested && !sc_find(fs, s->a->str))
            sc_add(J, fs, s->a->str, 0);
        return;
    case S_BLOCK:
        for (int i = 0; i < s->n; i++)
            block_functions(J, fs, s->list[i], 1);
        return;
    case S_IF:
        block_functions(J, fs, s->b, 1);
        block_functions(J, fs, s->c, 1);
        return;
    case S_FOR:
        block_functions(J, fs, s->d, 1);
        return;
    case S_FORIN:
    case S_FOROF:
        block_functions(J, fs, s->c, 1);
        return;
    case S_WHILE:
    case S_DOWHILE:
        block_functions(J, fs, s->b, 1);
        return;
    case S_TRY:
        block_functions(J, fs, s->a, 1);
        block_functions(J, fs, s->b, 1);
        block_functions(J, fs, s->c, 1);
        return;
    case S_SWITCH:
        for (int i = 0; i < s->n; i++)
            for (int k = 0; k < s->list[i]->n; k++)
                block_functions(J, fs, s->list[i]->list[k], 1);
        return;
    case S_LABEL:
        block_functions(J, fs, s->a, 1);
        return;
    default:
        return;
    }
}

/* The function `this` and super belong to: the nearest that is not an arrow. */
static struct vfunc *this_fn(struct vfunc *f)
{
    while (f && f->arrow)
        f = f->parent;
    return f;
}

static struct vdecl *an_resolve(struct an *A, struct jsstr *name)
{
    if (str_is(name, "arguments")) {
        struct vfunc *tf = this_fn(A->fn);
        if (tf && !tf->is_root && !tf->arguments_param)
            tf->args = sc_add(A->J, tf->params, name, 0);
    }
    for (struct vscope *s = A->sc; s; s = s->parent) {
        if (s->kind == SC_DYN)
            return NULL;
        struct vdecl *d = sc_find(s, name);
        if (d) {
            if (d->scope->fn != A->fn)
                d->captured = 1;
            return d;
        }
    }
    return NULL;
}

static void an_expr(struct an *A, struct ast *n);
static void an_stmt(struct an *A, struct ast *n);
static struct vfunc *an_function(struct an *A, struct ast *fn);

static void an_list(struct an *A, struct ast **list, int n)
{
    for (int i = 0; i < n; i++)
        an_stmt(A, list[i]);
}

/* A binding or assignment target: its names are uses, its defaults and keys expressions. */
static void an_target(struct an *A, struct ast *p)
{
    if (!p)
        return;
    switch (p->kind) {
    case N_IDENT:
        an_resolve(A, p->str);
        return;
    case N_ASSIGN:
        an_target(A, p->a);
        an_expr(A, p->b);
        return;
    case N_SPREAD:
        an_target(A, p->a);
        return;
    case N_ARRAY:
        for (int i = 0; i < p->n; i++)
            if (p->list[i]->kind != N_HOLE)
                an_target(A, p->list[i]);
        return;
    case N_OBJECT:
        for (int i = 0; i < p->n; i++) {
            struct ast *pr = p->list[i];
            if (pr->kind != N_SPREAD && pr->b)
                an_expr(A, pr->b);
            an_target(A, pr->a);
        }
        return;
    default:
        an_expr(A, p);
        return;
    }
}

static void an_class(struct an *A, struct ast *n)
{
    if (n->a)
        an_expr(A, n->a);
    struct vscope *saved = A->sc;
    struct vscope *cs = sc_new(A, A->sc, SC_CLASS);
    if (n->str)
        sc_add(A->J, cs, n->str, 1);
    n->vm = cs;
    A->sc = cs;
    /* Instance-field keys run at class definition, while their values run
     * for each instance. Give constructor initializers captured keys. */
    for (int i = 0; i < n->n; i++) {
        struct ast *m = n->list[i];
        if (m->kind == N_FIELD && !m->op && m->b) {
            char name[40];
            snprintf(name, sizeof(name), "%%field%d", i);
            struct ast *key = js_alloc(A->J, sizeof(*key));
            key->kind = N_IDENT;
            key->str = js_mkstr(A->J, name, (int)strlen(name));
            sc_add(A->J, cs, key->str, 1);
            m->c->b = key;
        }
    }
    an_function(A, n->b);
    for (int i = 0; i < n->n; i++) {
        struct ast *m = n->list[i];
        if (m->kind == N_FIELD && !m->op) {
            if (m->b)
                an_expr(A, m->b);
            continue;                   /* values are in the constructor */
        }
        if (m->b)
            an_expr(A, m->b);
        if (m->kind == N_FIELD && m->op == 1) {
            /* A static field's value: as a method of the class, so this is the class. */
            if (m->a) {
                struct ast *fn = js_alloc(A->J, sizeof(*fn));
                fn->kind = N_FUNC;
                fn->flags = F_EXPRBODY;
                fn->a = m->a;
                fn->line = m->line;
                m->d = fn;
                an_function(A, fn);
            }
            continue;
        }
        an_function(A, m->a);
    }
    A->sc = saved;
}

/* Too deep to analyse on this stack: a SyntaxError, and the rest is not run. */
static int an_too_deep(struct an *A)
{
    if (!js_stack_low(A->J))
        return 0;
    if (!A->J->throwing)
        js_throw_error(A->J, "SyntaxError: the script is nested too deeply");
    return 1;
}

static void an_expr(struct an *A, struct ast *n)
{
    if (!n || an_too_deep(A))
        return;
    switch (n->kind) {
    case N_NUM: case N_STR: case N_REGEX: case N_TRUE: case N_FALSE: case N_NULL: case N_HOLE:
    case N_IMPORT_META:
        return;
    case N_NEWTARGET: {
        struct vfunc *tf = this_fn(A->fn);
        if (!tf || tf->is_root) {
            js_throw_error(A->J, "SyntaxError: new.target outside a function");
            return;
        }
        tf->needs_env = 1;
        return;
    }
    case N_THIS: {
        struct vfunc *tf = this_fn(A->fn);
        if (tf && tf != A->fn && !tf->is_root)
            tf->needs_env = 1;          /* an arrow reads it from the function's env */
        return;
    }
    case N_SUPER: {
        struct vfunc *tf = this_fn(A->fn);
        if (tf && !tf->is_root)
            tf->needs_env = 1;          /* the home object is looked up in its env */
        return;
    }
    case N_IDENT:
        an_resolve(A, n->str);
        return;
    case N_TEMPLATE:
    case N_ARRAY:
    case N_SEQ:
        for (int i = 0; i < n->n; i++)
            an_expr(A, n->list[i]);
        return;
    case N_OBJECT:
        for (int i = 0; i < n->n; i++) {
            struct ast *p = n->list[i];
            if (p->kind != N_SPREAD && p->b)
                an_expr(A, p->b);
            an_expr(A, p->a);
        }
        return;
    case N_SPREAD: case N_UNARY: case N_AWAIT: case N_YIELD: case N_IMPORT:
        an_expr(A, n->a);
        return;
    case N_FUNC:
        an_function(A, n);
        return;
    case N_CLASS:
        an_class(A, n);
        return;
    case N_CALL:
    case N_NEW:
        if(n->kind==N_CALL && n->a->kind==N_IDENT && str_is(n->a->str,"eval")) {
            A->fn->dynamic_eval=1;A->fn->needs_env=1;
            for(struct vscope *scope=A->sc;scope && scope->kind!=SC_DYN;scope=scope->parent)
                for(int k=0;k<scope->nd;k++)scope->d[k]->captured=1;
        }
        an_expr(A, n->a);
        for (int i = 0; i < n->n; i++)
            an_expr(A, n->list[i]);
        return;
    case N_MEMBER:
        an_expr(A, n->a);
        return;
    case N_INDEX: case N_BINARY: case N_AND: case N_OR: case N_NULLISH:
        an_expr(A, n->a);
        an_expr(A, n->b);
        return;
    case N_PREINC: case N_POSTINC:
        an_target(A, n->a);
        return;
    case N_ASSIGN:
        an_target(A, n->a);
        an_expr(A, n->b);
        return;
    case N_COND:
        an_expr(A, n->a);
        an_expr(A, n->b);
        an_expr(A, n->c);
        return;
    default:
        an_expr(A, n->a);
        an_expr(A, n->b);
        return;
    }
}

static void an_block(struct an *A, struct ast *n)
{
    struct vscope *saved = A->sc;
    if (n->flags & F_SCOPED) {
        struct vscope *bs = sc_new(A, A->sc, SC_BLOCK);
        for (int i = 0; i < n->n; i++)
            lexical_decls(A->J, bs, n->list[i]);
        n->vm = bs;
        A->sc = bs;
    }
    an_list(A, n->list, n->n);
    A->sc = saved;
}

static void an_var(struct an *A, struct ast *n)
{
    for (int i = 0; i < n->n; i++) {
        struct ast *d = n->list[i];
        an_expr(A, d->a);
        if (d->kind == N_DESTRUCT)
            an_target(A, d->b);
        else
            an_resolve(A, d->str);
    }
}

static void an_stmt(struct an *A, struct ast *n)
{
    if (!n || an_too_deep(A))
        return;
    struct vscope *saved = A->sc;
    switch (n->kind) {
    case S_EMPTY: case S_BREAK: case S_CONTINUE: case S_IMPORT:
        return;
    case S_EXPR: case S_RETURN: case S_THROW:
        an_expr(A, n->a);
        return;
    case S_VAR:
        an_var(A, n);
        return;
    case S_FUNCDECL:
        an_function(A, n->a);
        return;
    case S_EXPORT:
        if (n->op == EX_DECL)
            an_stmt(A, n->a);
        else if (n->op == EX_DEFAULT)
            an_expr(A, n->a);
        return;
    case S_BLOCK:
        an_block(A, n);
        return;
    case S_IF:
        an_expr(A, n->a);
        an_stmt(A, n->b);
        an_stmt(A, n->c);
        return;
    case S_WHILE:
    case S_DOWHILE:
        an_expr(A, n->a);
        an_stmt(A, n->b);
        return;
    case S_FOR:
        if (n->a && n->a->kind == S_VAR && n->a->flags != V_VAR) {
            struct vscope *ls = sc_new(A, A->sc, SC_BLOCK);
            lexical_decls(A->J, ls, n->a);
            n->vm = ls;
            A->sc = ls;
        }
        if (n->a && n->a->kind == S_VAR)
            an_var(A, n->a);
        else
            an_expr(A, n->a);
        an_expr(A, n->b);
        an_expr(A, n->c);
        an_stmt(A, n->d);
        A->sc = saved;
        return;
    case S_FORIN:
    case S_FOROF:
        an_expr(A, n->b);
        if (n->a->kind == S_VAR && n->a->flags != V_VAR) {
            struct vscope *ls = sc_new(A, A->sc, SC_BLOCK);
            lexical_decls(A->J, ls, n->a);
            n->vm = ls;
            A->sc = ls;
        }
        if (n->a->kind == S_VAR)
            an_var(A, n->a);
        else
            an_target(A, n->a);
        an_stmt(A, n->c);
        A->sc = saved;
        return;
    case S_TRY:
        an_stmt(A, n->a);
        if (n->b) {
            struct vscope *cs = sc_new(A, A->sc, SC_BLOCK);
            if (n->str)
                sc_add(A->J, cs, n->str, 0);
            else if (n->d)
                pattern_decls(A->J, cs, n->d, 0);
            n->vm = cs;
            A->sc = cs;
            if (n->d)
                an_target(A, n->d);
            an_stmt(A, n->b);
            A->sc = saved;
        }
        an_stmt(A, n->c);
        return;
    case S_SWITCH: {
        an_expr(A, n->a);
        struct vscope *ss = sc_new(A, A->sc, SC_BLOCK);
        for (int i = 0; i < n->n; i++)
            for (int k = 0; k < n->list[i]->n; k++)
                lexical_decls(A->J, ss, n->list[i]->list[k]);
        n->vm = ss;
        A->sc = ss;
        for (int i = 0; i < n->n; i++)
            an_expr(A, n->list[i]->a);          /* case expressions use this scope too */
        for (int i = 0; i < n->n; i++)
            an_list(A, n->list[i]->list, n->list[i]->n);
        A->sc = saved;
        return;
    }
    case S_LABEL:
        an_stmt(A, n->a);
        return;
    default:
        an_expr(A, n);
        return;
    }
}

/* A function: its scope and declarations, then its body. */
static struct vfunc *an_function(struct an *A, struct ast *fn)
{
    struct js *J = A->J;
    struct vfunc *f = js_alloc(J, sizeof(*f));
    f->ast = fn;
    f->parent = A->fn;
    f->arrow = (fn->flags & F_ARROW) != 0;
    fn->vm = f;
    struct an inner = { J, f, A->sc };
    if (fn->b == fn) {                  /* a named function expression sees its name */
        struct vscope *ns = js_alloc(J, sizeof(*ns));
        ns->parent = A->sc;
        ns->fn = A->fn;
        ns->kind = SC_NAMED;
        sc_add(J, ns, fn->str, 0);
        f->named = ns;
        inner.sc = ns;
    }
    struct vscope *ps = sc_new(&inner, inner.sc, SC_FUNC);
    f->scope = f->params = ps;
    int separate = 0;
    for (int i = 0; i < fn->n; i++) {
        struct ast *p = fn->list[i];
        separate |= p->a != NULL || p->kind == N_DESTRUCT;
        if (p->kind == N_DESTRUCT)
            pattern_decls(J, ps, p->b, 0);
        else
            sc_add(J, ps, p->str, 0);
    }
    f->arguments_param = sc_find(ps, js_mkstr(J, "arguments", 9)) != NULL;
    inner.sc = ps;
    for (int i = 0; i < fn->n; i++) {
        struct ast *p = fn->list[i];
        an_expr(&inner, p->a);
        if (p->kind == N_DESTRUCT)
            an_target(&inner, p->b);
    }
    struct vscope *fs = separate && !(fn->flags & F_EXPRBODY) ? sc_new(&inner, ps, SC_FUNC) : ps;
    f->scope = fs;
    if (!(fn->flags & F_EXPRBODY) && fn->a) {
        struct fninfo *fi = fn->info;
        if (fi && !fi->prepared)
            prepare_body(J, fn->a, fi);
        for (int i = 0; fi && i < fi->nvars; i++)
            sc_add(J, fs, fi->vars[i], 0);
        for (int i = 0; fi && i < fi->nfuncs; i++)
            sc_add(J, fs, fi->funcs[i]->str, 0);
        for (int i = 0; i < fn->a->n; i++)
            lexical_decls(J, fs, fn->a->list[i]);
        for (int i = 0; i < fn->a->n; i++)
            block_functions(J, fs, fn->a->list[i], 0);
    }
    inner.sc = fs;
    if (fn->flags & F_EXPRBODY)
        an_expr(&inner, fn->a);
    else if (fn->a)
        an_list(&inner, fn->a->list, fn->a->n);
    return f;
}

/* A script, module or eval'd source: the top level stays dynamic. */
static struct vfunc *an_root(struct js *J, struct ast *prog)
{
    struct vfunc *f = js_alloc(J, sizeof(*f));
    f->ast = prog;
    f->is_root = 1;
    struct an A = { J, f, NULL };
    f->scope = sc_new(&A, NULL, SC_DYN);
    A.sc = f->scope;
    if (prog->kind == N_FUNC) {                 /* js_compile_handler's function expression */
        an_function(&A, prog);
    } else {
        an_list(&A, prog->list, prog->n);
        prog->vm = f;
    }
    return f;
}

/* Where each declaration lives, decided once its function's analysis is complete. */
static void lay_out(struct vfunc *f)
{
    if (f->laid_out)
        return;
    f->laid_out = 1;
    for (int i = 0; i < f->nscopes; i++) {
        struct vscope *s = f->scopes[i];
        for (int k = 0; k < s->nd; k++) {
            struct vdecl *d = s->d[k];
            if (d->captured)
                d->slot = s->nslots++;
            else
                d->reg = f->nregs++;
        }
        s->has_env = s->nslots > 0 || (s == f->params && f->needs_env && !f->is_root);
    }
    if (f->named) {
        struct vdecl *d = f->named->d[0];
        f->named->has_env = d->captured;
        if (d->captured)
            d->slot = f->named->nslots++;
    }
}

/* ---- bytecode ---- */

/*
 * Instructions are 32-bit words: an opcode, then its operands, each a
 * word. The comment gives the operands and what the instruction does to
 * the operand stack (top on the right). Stores leave the value stored.
 */
#define OPCODES(X) \
    X(NOP)          /*                  -- */                                       \
    X(UNDEF)        /*                  -- undefined */                             \
    X(NULL)         /*                  -- null */                                  \
    X(TRUE)         /*                  -- true */                                  \
    X(FALSE)        /*                  -- false */                                 \
    X(CONST)        /* k                -- k */                                     \
    X(POP)          /* v                -- */                                       \
    X(DUP)          /* v                -- v v */                                   \
    X(DUP2)         /* a b              -- a b a b */                               \
    X(SWAP)         /* a b              -- b a */                                   \
    X(ROT3)         /* a b c            -- c a b */                                 \
    X(ROT4)         /* a b c d          -- d a b c */                               \
    X(PICK)         /* n                -- the value n below the top */             \
    X(GETREG)       /* r                -- v */                                     \
    X(SETREG)       /* r          v     -- v */                                     \
    X(GETENV)       /* hops slot        -- v */                                     \
    X(SETENV)       /* hops slot  v     -- v */                                     \
    X(GETNAME)      /* hops name        -- v (ReferenceError if none) */            \
    X(TYPEOFNAME)   /* hops name        -- v (undefined if none) */                 \
    X(SETNAME)      /* hops name  v     -- v */                                     \
    X(DECLNAME)     /* hops name konst v -- v: a top-level let or const */         \
    X(VARNAME)      /* hops name  v     -- v: a top-level var's value */           \
    X(CONSTERR)     /* name             -- (throws) */                              \
    X(THIS)         /*                  -- this */                                  \
    X(NEWTARGET)    /* lexical constructor target */                              \
    X(THISDYN)      /*                  -- this, from the environments */           \
    X(PUSHENV)      /* nslots           -- */                                       \
    X(POPENV)       /*                  -- */                                       \
    X(COPYENV)      /*                  -- (a fresh copy for a loop's next turn) */ \
    X(FUNCENV)      /* nslots           -- (the function's own) */                 \
    X(CLOSURE)      /* fn               -- f */                                     \
    X(CLOSUREN)     /* fn               -- f, which sees its own name */            \
    X(ARGUMENTS)    /*                  -- arguments */                             \
    X(REST)         /* i                -- [args from i] */                         \
    X(ARG)          /* i                -- args[i] */                               \
    X(ARRAY)        /*                  -- [] */                                    \
    X(APUSH)        /*            a v   -- a */                                     \
    X(AHOLE)        /*            a     -- a */                                     \
    X(ASPREAD)      /*            a it  -- a */                                     \
    X(OBJECT)       /*                  -- {} */                                    \
    X(OPROP)        /* key        o v   -- o */                                     \
    X(OPROPC)       /*          o k v   -- o */                                     \
    X(OGET)         /* key        o f   -- o */                                     \
    X(OSET)         /* key        o f   -- o */                                     \
    X(OGETC)        /*          o k f   -- o */                                     \
    X(OSETC)        /*          o k f   -- o */                                     \
    X(OSPREAD)      /*          o src   -- o */                                     \
    X(GETPROP)      /* key        o     -- v */                                     \
    X(GETELEM)      /*            o k   -- v */                                     \
    X(SETPROP)      /* key        o v   -- v */                                     \
    X(SETELEM)      /*          o k v   -- v */                                     \
    X(DELPROP)      /* key        o     -- bool */                                  \
    X(DELELEM)      /*            o k   -- bool */                                  \
    X(SUPERGET)     /*            k     -- v */                                     \
    X(SUPERCALL)    /* argc     args... -- this */                                  \
    X(SUPERCALLS)   /*         argsarr  -- this */                                  \
    X(CALL)         /* argc name  f this args... -- r */                            \
    X(CALLS)        /* name   f this argsarr -- r */                                \
    X(NEW)          /* argc     f args... -- r */                                   \
    X(NEWS)         /*        f argsarr -- r */                                     \
    X(OPTCALL)      /* label   f this   -- f this, or undefined and jump */         \
    X(TEMPLATE)     /* tpl              -- strings */                               \
    X(REGEX)        /* node             -- a new RegExp */                          \
    X(ADD) X(SUB) X(MUL) X(DIV) X(MOD) X(EXP) X(SHL) X(SHR) X(USHR)                 \
    X(BAND) X(BOR) X(BXOR) X(EQ) X(NE) X(SEQ) X(SNE) X(LT) X(GT) X(LE) X(GE)         \
    X(INSTANCEOF) X(IN) /*      a b     -- r */                                     \
    X(NOT) X(NEG) X(PLUS) X(BNOT) X(TYPEOF) X(VOID) /* v -- r */                   \
    X(TONUM)        /*            v     -- number */                                \
    X(INC)          /*            v     -- number + 1 */                            \
    X(DEC)          /*            v     -- number - 1 */                            \
    X(TOKEY)        /*            v     -- string */                                \
    X(JUMP)         /* label */                                                     \
    X(LOOP)         /* label: a jump back, which counts as steps */                 \
    X(JFALSE)       /* label      v     -- */                                       \
    X(JTRUE)        /* label      v     -- */                                       \
    X(JFALSEK)      /* label      v     -- v if jumping, else -- */                 \
    X(JTRUEK)       /* label      v     -- v if jumping, else -- */                 \
    X(JNOTNULLK)    /* label      v     -- v if jumping, else -- */                 \
    X(JUNDEFK)      /* label      v     -- v if jumping (not undefined), else -- */ \
    X(JNULLISH)     /* label      v     -- v, or undefined and jump */              \
    X(SETFNAME)     /* name       f     -- f (names an anonymous function) */       \
    X(RETURN)       /*            v     -- */                                       \
    X(RETUNDEF)                                                                     \
    X(THROW)        /*            v     -- */                                       \
    X(TRY)          /* label: where a throw in what follows goes */                 \
    X(ENDTRY)                                                                       \
    X(FINCALL)      /* reg label: save return PC, run the shared finally body */     \
    X(FINRET)       /* reg: return from a shared finally body */                    \
    X(BINDENV)      /* slot/const name: expose a lexical slot to direct eval */      \
    X(CLEARREGS)    /* first count: release a scope's uncaptured local values */     \
    X(CATCH)        /*                  -- exception (not a generator's return) */  \
    X(CATCHANY)     /*                  -- exception */                             \
    X(RETHROW)      /*            v     -- */                                       \
    X(ITEROPEN)     /* it         v     -- */                                       \
    X(ITERNEXT)     /* it label         -- v, or jump when done */                  \
    X(ITERSTEP)     /* it               -- v (undefined when done) */               \
    X(ITERREST)     /* it               -- [the rest] */                            \
    X(ITERCLOSE)    /* it */                                                        \
    X(FORIN)        /* it         v     -- (the keys to walk) */                    \
    X(AITEROPEN)    /* it         v     -- (for await) */                           \
    X(AITERNEXT)    /* it label         -- v, or jump when done */                  \
    X(COERCIBLE)    /*            v     -- v (TypeError if null or undefined) */    \
    X(OBJREST)      /* r          v     -- rest (without the keys in array r) */    \
    X(KEYPUSH)      /* r          k     -- k (into array r) */                      \
    X(AWAIT) X(YIELD) X(YIELDSTAR) /*   v -- r */                                   \
    X(IMPORT)       /* base       spec  -- promise */                               \
    X(COMPLETION)   /*            v     -- (eval's result) */                       \
    X(EXPORTDEF)    /* hops       v     -- */                                       \
    X(CLASS)        /* cls extends [parent] -- F proto */                           \
    X(CMETHOD)      /* kind     F p k f -- F p */                                   \
    X(CFIELD)       /*          F p k f -- F p (f: the value's method, or undefined) */ \
    X(CSTATIC)      /*          F p f   -- F p */

enum {
#define X(name) I_##name,
    OPCODES(X)
#undef X
    NOPS
};

/* CMETHOD kinds */
enum { CM_METHOD, CM_GETTER, CM_SETTER, CM_STATIC = 4 };

struct vproto {
    uint32_t *code;
    jsval *k;                           /* constants: names, numbers, strings, functions */
    int ncode, nk;
    int nregs, maxsp, niters, maxh;
    struct vfunc *vf;
};

/* ---- code generation ---- */

/* What a break, continue or return crosses on its way out. */
enum { CT_LOOP, CT_SWITCH, CT_LABEL, CT_SCOPE, CT_ITER, CT_TRY, CT_FINALLY };

struct patches {
    int *at;
    int n, cap;
};

struct ctl {
    struct ctl *up;
    int kind;
    struct jsstr **labels;              /* loops, switches and labelled statements */
    int nlabels;
    struct patches brk, cont;
    int it;                             /* CT_ITER */
    struct ast *fin;                    /* CT_FINALLY: the finally block */
    struct vscope *sc;                  /* CT_FINALLY / CT_SCOPE: its scope */
    struct patches calls;              /* CT_FINALLY: calls to its shared body */
    int retreg;                         /* CT_FINALLY: the saved continuation PC */
};

struct cg {
    struct js *J;
    struct vfunc *vf;
    struct vscope *sc;
    uint32_t *code;
    int n, cap;
    jsval *k;
    int nk, kcap;
    int sp, maxsp, depth;
    int nregs, niters, h, maxh;
    struct ctl *ctl;
    struct jsstr **labels;              /* labels waiting for the statement they name */
    int nlabels;
};

static void cg_word(struct cg *C, uint32_t w)
{
    if (C->n == C->cap) {
        int nc = C->cap ? C->cap * 2 : 256;
        uint32_t *nw = js_alloc_atomic(C->J, sizeof(*nw) * (size_t)nc);
        if (C->n)
            memcpy(nw, C->code, sizeof(*nw) * (size_t)C->n);
        C->code = nw;
        C->cap = nc;
    }
    C->code[C->n++] = w;
}

static void cg_depth(struct cg *C, int delta)
{
    C->sp += delta;
    if (C->sp > C->maxsp)
        C->maxsp = C->sp;
}

/* An instruction that changes the stack's depth by delta. */
static void op(struct cg *C, int code, int delta)
{
    cg_word(C, (uint32_t)code);
    cg_depth(C, delta);
}

static void op1(struct cg *C, int code, int delta, int a)
{
    op(C, code, delta);
    cg_word(C, (uint32_t)a);
}

static void op2(struct cg *C, int code, int delta, int a, int b)
{
    op1(C, code, delta, a);
    cg_word(C, (uint32_t)b);
}

static int cg_const(struct cg *C, jsval v)
{
    for (int i = 0; i < C->nk; i++)     /* numbers by their bits: -0 is not 0 */
        if (C->k[i].t == v.t && (v.t == JS_NUM ? !memcmp(&C->k[i].u.n, &v.u.n, sizeof(double))
                                               : C->k[i].u.s == v.u.s))
            return i;
    if (C->nk == C->kcap) {
        int nc = C->kcap ? C->kcap * 2 : 16;
        jsval *nk = js_alloc(C->J, sizeof(*nk) * (size_t)nc);
        if (C->nk)
            memcpy(nk, C->k, sizeof(*nk) * (size_t)C->nk);
        C->k = nk;
        C->kcap = nc;
    }
    C->k[C->nk] = v;
    return C->nk++;
}

static int cg_str(struct cg *C, struct jsstr *s)
{
    return cg_const(C, strv(s));
}

/* An AST node (a function, a class, a template) as a constant. */
#define VK_NODE 0x7F                    /* a constant that is an AST node, not a value */

static int cg_node(struct cg *C, struct ast *n)
{
    jsval v = { VK_NODE, { .s = (struct jsstr *)n } };
    return cg_const(C, v);
}

static int cg_temp(struct cg *C)
{
    return C->nregs++;
}

/* A forward jump: emit it, and patch it to `here` later. */
static int jump(struct cg *C, int code, int delta)
{
    op(C, code, delta);
    cg_word(C, 0);
    return C->n - 1;
}

static void land(struct cg *C, int at)
{
    C->code[at] = (uint32_t)C->n;
}

static void patches_add(struct cg *C, struct patches *p, int at)
{
    if (p->n == p->cap) {
        int nc = p->cap ? p->cap * 2 : 4;
        int *na = js_alloc_atomic(C->J, sizeof(*na) * (size_t)nc);
        if (p->n)
            memcpy(na, p->at, sizeof(*na) * (size_t)p->n);
        p->at = na;
        p->cap = nc;
    }
    p->at[p->n++] = at;
}

static void patches_land(struct cg *C, struct patches *p)
{
    for (int i = 0; i < p->n; i++)
        C->code[p->at[i]] = (uint32_t)C->n;
}

static void ctl_push(struct cg *C, struct ctl *c, int kind)
{
    memset(c, 0, sizeof(*c));
    c->kind = kind;
    c->up = C->ctl;
    C->ctl = c;
}

static void ctl_pop(struct cg *C, struct ctl *c)
{
    C->ctl = c->up;
}

/* Environments between the current scope and s (excluded). */
static int hops_to(struct cg *C, struct vscope *s)
{
    int hops = 0;
    for (struct vscope *x = C->sc; x && x != s; x = x->parent)
        if (x->has_env)
            hops++;
    return hops;
}

/* Where a name is: a declaration, or NULL for the dynamic top level (with *dyn its scope). */
static struct vdecl *cg_resolve(struct cg *C, struct jsstr *name, struct vscope **dyn)
{
    for (struct vscope *s = C->sc; s; s = s->parent) {
        if (s->kind == SC_DYN) {
            *dyn = C->vf->dynamic_eval?NULL:s;
            return NULL;
        }
        struct vdecl *d = sc_find(s, name);
        if (d)
            return d;
    }
    *dyn = NULL;
    return NULL;
}

static void load_decl(struct cg *C, struct vdecl *d)
{
    if (d->captured)
        op2(C, I_GETENV, 1, hops_to(C, d->scope), d->slot);
    else
        op1(C, I_GETREG, 1, d->reg);
}

static void store_decl(struct cg *C, struct vdecl *d)
{
    if (d->captured)
        op2(C, I_SETENV, 0, hops_to(C, d->scope), d->slot);
    else
        op1(C, I_SETREG, 0, d->reg);
}

static void load_name(struct cg *C, struct jsstr *name, int for_typeof)
{
    struct vscope *dyn = NULL;
    struct vdecl *d = cg_resolve(C, name, &dyn);
    if (d)
        load_decl(C, d);
    else
        op2(C, for_typeof ? I_TYPEOFNAME : I_GETNAME, 1, dyn ? hops_to(C, dyn) : 0, cg_str(C, name));
}

/* How a store binds: an assignment, or the declaration's own value. */
enum { ST_ASSIGN, ST_VAR, ST_LET, ST_CONST };

static void store_name(struct cg *C, struct jsstr *name, int mode)
{
    struct vscope *dyn = NULL;
    struct vdecl *d = cg_resolve(C, name, &dyn);
    int hops = d ? 0 : dyn ? hops_to(C, dyn) : 0;
    if (d) {
        if (d->konst && mode == ST_ASSIGN)
            op1(C, I_CONSTERR, 0, cg_str(C, name));
        else
            store_decl(C, d);
    } else if (mode == ST_LET || mode == ST_CONST) {
        op(C, I_DECLNAME, 0);
        cg_word(C, (uint32_t)hops);
        cg_word(C, (uint32_t)cg_str(C, name));
        cg_word(C, mode == ST_CONST);
    } else {
        op2(C, mode == ST_VAR ? I_VARNAME : I_SETNAME, 0, hops, cg_str(C, name));
    }
}

static void clear_scope(struct cg *C, struct vscope *s)
{
    int first = -1, count = 0;
    for (int i = 0; i < s->nd; i++) {
        if (!s->d[i]->captured) {
            if (first < 0)
                first = s->d[i]->reg;
            count++;
        }
    }
    if (count)
        op2(C, I_CLEARREGS, 0, first, count);
}

/* An exception restores the handler's environment. Release register
 * locals from scopes it exited, keeping every enclosing scope's locals. */
static void clear_inactive_scopes(struct cg *C)
{
    for (int i = 0; i < C->vf->nscopes; i++) {
        struct vscope *s = C->vf->scopes[i], *active;
        for (active = C->sc; active && active != s; active = active->parent)
            ;
        if (!active)
            clear_scope(C, s);
    }
}

static void bind_scope_names(struct cg *C,struct vscope *scope)
{
    if(!scope || !scope->has_env)return;
    for(int k=0;k<scope->nd;k++)if(scope->d[k]->captured)
        op2(C,I_BINDENV,0,scope->d[k]->slot*2+scope->d[k]->konst,cg_str(C,scope->d[k]->name));
}
/* Enter a scope that may make an environment; returns the one left. */
static struct vscope *scope_enter(struct cg *C, struct vscope *s, struct ctl *c)
{
    struct vscope *saved = C->sc;
    if (!s)
        return saved;
    C->sc = s;
    clear_scope(C, s);
    if (s->has_env)
        op1(C, I_PUSHENV, 0, s->nslots);
    bind_scope_names(C,s);
    if (c) {
        ctl_push(C, c, CT_SCOPE);
        c->sc = s;
    }
    return saved;
}

static void scope_leave(struct cg *C, struct vscope *s, struct vscope *saved, struct ctl *c)
{
    if (s) {
        if (c)
            ctl_pop(C, c);
        clear_scope(C, s);
        if (s->has_env)
            op(C, I_POPENV, 0);
    }
    C->sc = saved;
}

/* ---- expressions ---- */

static void ex(struct cg *C, struct ast *n);
static void st(struct cg *C, struct ast *n);
static void bind(struct cg *C, struct ast *p, int mode);

/* Name an anonymous function after what it is assigned to. */
static void set_fname(struct cg *C, struct jsstr *name)
{
    op1(C, I_SETFNAME, 0, cg_str(C, name));
}

static int is_anon_fn(struct ast *n)
{
    return n && (n->kind == N_FUNC || n->kind == N_CLASS);
}

/* Arguments onto the stack; returns their count, or -1 if they were spread into an array. */
static int args(struct cg *C, struct ast *call, int from)
{
    int spread = 0;
    for (int i = from; i < call->n; i++)
        spread |= call->list[i]->kind == N_SPREAD;
    if (!spread) {
        for (int i = from; i < call->n; i++)
            ex(C, call->list[i]);
        return call->n - from;
    }
    op(C, I_ARRAY, 1);
    for (int i = from; i < call->n; i++) {
        struct ast *a = call->list[i];
        if (a->kind == N_SPREAD) {
            ex(C, a->a);
            op(C, I_ASPREAD, -1);
        } else {
            ex(C, a);
            op(C, I_APUSH, -1);
        }
    }
    return -1;
}

static void call_with(struct cg *C, int argc, int name)
{
    if (argc < 0)
        op1(C, I_CALLS, -2, name);
    else
        op2(C, I_CALL, -(argc + 1), argc, name);
}

static void ex_call(struct cg *C, struct ast *n)
{
    struct ast *callee = n->a;
    if (callee->kind == N_SUPER) {
        int argc = args(C, n, 0);
        if (argc < 0)
            op(C, I_SUPERCALLS, 0);
        else
            op1(C, I_SUPERCALL, 1 - argc, argc);
        return;
    }
    int name = -1, skip = -1;
    if ((callee->kind == N_MEMBER || callee->kind == N_INDEX) && callee->a->kind == N_SUPER) {
        if (callee->kind == N_MEMBER)
            op1(C, I_CONST, 1, cg_str(C, callee->str));
        else
            ex(C, callee->b);
        op(C, I_SUPERGET, 0);
        op(C, C->vf->arrow || C->vf->is_root ? I_THISDYN : I_THIS, 1);
    } else if (callee->kind == N_MEMBER || callee->kind == N_INDEX) {
        ex(C, callee->a);
        if (callee->flags & F_OPTIONAL)
            skip = jump(C, I_JNULLISH, 0);
        op(C, I_DUP, 1);
        if (callee->kind == N_MEMBER) {
            name = cg_str(C, callee->str);
            op1(C, I_GETPROP, 0, name);
        } else {
            ex(C, callee->b);
            op(C, I_GETELEM, -1);
        }
        op(C, I_SWAP, 0);
    } else {
        ex(C, callee);
        if (callee->kind == N_IDENT)
            name = cg_str(C, callee->str);
        op(C, I_UNDEF, 1);
    }
    int optional = -1;
    if (n->flags & F_OPTIONAL)
        optional = jump(C, I_OPTCALL, 0);
    int argc;
    if (n->op == 1) {                           /* a tagged template: strings, then each value */
        struct ast *tl = n->list[0];
        op1(C, I_TEMPLATE, 1, cg_node(C, tl));
        argc = 1;
        for (int i = 1; i < tl->n; i += 2, argc++)
            ex(C, tl->list[i]);
    } else {
        argc = args(C, n, 0);
    }
    if(callee->kind==N_IDENT && str_is(callee->str,"eval") && !(n->flags&F_OPTIONAL) && n->op!=1)name=-2;
    call_with(C, argc, name);
    /* A skipped call leaves undefined where its result would be. */
    if (optional >= 0)
        land(C, optional);
    if (skip >= 0)
        land(C, skip);
}

static void ex_new(struct cg *C, struct ast *n)
{
    ex(C, n->a);
    int argc = args(C, n, 0);
    if (argc < 0)
        op(C, I_NEWS, -1);
    else
        op1(C, I_NEW, -argc, argc);
}

static void ex_member(struct cg *C, struct ast *n)
{
    if (n->a->kind == N_SUPER) {
        if (n->kind == N_MEMBER)
            op1(C, I_CONST, 1, cg_str(C, n->str));
        else
            ex(C, n->b);
        op(C, I_SUPERGET, 0);
        return;
    }
    ex(C, n->a);
    int skip = -1;
    if (n->flags & F_OPTIONAL)
        skip = jump(C, I_JNULLISH, 0);
    if (n->kind == N_MEMBER) {
        op1(C, I_GETPROP, 0, cg_str(C, n->str));
    } else {
        ex(C, n->b);
        op(C, I_GETELEM, -1);
    }
    if (skip >= 0)
        land(C, skip);
}

/* Store the value on the stack into a simple target (a name, a member, an index)
 * whose object and key are already below it: o v, or o k v. */
static void store_target(struct cg *C, struct ast *t, int mode)
{
    if (t->kind == N_IDENT)
        store_name(C, t->str, mode);
    else if (t->kind == N_MEMBER)
        op1(C, I_SETPROP, -1, cg_str(C, t->str));
    else
        op(C, I_SETELEM, -2);
}

/* The object (and key) of a member target. */
static int target_base(struct cg *C, struct ast *t)
{
    if (t->kind == N_MEMBER) {
        ex(C, t->a);
        return 1;
    }
    if (t->kind == N_INDEX) {
        ex(C, t->a);
        ex(C, t->b);
        return 2;
    }
    return 0;
}

static void load_target(struct cg *C, struct ast *t, int nbase)
{
    if (t->kind == N_IDENT) {
        load_name(C, t->str, 0);
    } else if (nbase == 1) {
        op(C, I_DUP, 1);
        op1(C, I_GETPROP, 0, cg_str(C, t->str));
    } else {
        op(C, I_DUP2, 2);
        op(C, I_GETELEM, -1);
    }
}

static int binop(int o)
{
    switch (o) {
    case OP_ADD: return I_ADD;
    case OP_SUB: return I_SUB;
    case OP_MUL: return I_MUL;
    case OP_DIV: return I_DIV;
    case OP_MOD: return I_MOD;
    case OP_EXP: return I_EXP;
    case OP_SHL: return I_SHL;
    case OP_SHR: return I_SHR;
    case OP_USHR: return I_USHR;
    case OP_BAND: return I_BAND;
    case OP_BOR: return I_BOR;
    case OP_BXOR: return I_BXOR;
    case OP_EQ: return I_EQ;
    case OP_NE: return I_NE;
    case OP_SEQ: return I_SEQ;
    case OP_SNE: return I_SNE;
    case OP_LT: return I_LT;
    case OP_GT: return I_GT;
    case OP_LE: return I_LE;
    case OP_GE: return I_GE;
    case OP_INSTANCEOF: return I_INSTANCEOF;
    case OP_IN: return I_IN;
    }
    return I_NOP;
}

static void ex_assign(struct cg *C, struct ast *n)
{
    struct ast *t = n->a;
    if (t->kind == N_ARRAY || t->kind == N_OBJECT) {    /* [a, b] = [b, a] */
        ex(C, n->b);
        op(C, I_DUP, 1);
        bind(C, t, ST_ASSIGN);
        return;
    }
    int nbase = target_base(C, t);
    if (n->op == OP_NONE) {
        ex(C, n->b);
        if (t->kind == N_IDENT && is_anon_fn(n->b))
            set_fname(C, t->str);
        store_target(C, t, ST_ASSIGN);
        return;
    }
    load_target(C, t, nbase);
    if (n->op == OP_AND || n->op == OP_OR || n->op == OP_NULLISH) {
        int kept = C->sp;
        int keep = jump(C, n->op == OP_AND ? I_JFALSEK : n->op == OP_OR ? I_JTRUEK : I_JNOTNULLK, -1);
        ex(C, n->b);
        store_target(C, t, ST_ASSIGN);
        int done = jump(C, I_JUMP, 0);
        C->sp = kept;
        land(C, keep);                  /* the old value stays: drop the object and key under it */
        for (int i = 0; i < nbase; i++) {
            op(C, I_SWAP, 0);
            op(C, I_POP, -1);
        }
        land(C, done);
        return;
    }
    ex(C, n->b);
    op(C, binop(n->op), -1);
    store_target(C, t, ST_ASSIGN);
}

static void ex_incdec(struct cg *C, struct ast *n)
{
    struct ast *t = n->a;
    int nbase = target_base(C, t);
    load_target(C, t, nbase);
    op(C, I_TONUM, 0);
    int post = n->kind == N_POSTINC;
    if (post) {                         /* keep the old value under the object and key */
        op(C, I_DUP, 1);
        if (nbase)
            op(C, nbase == 1 ? I_ROT3 : I_ROT4, 0);
    }
    op(C, n->op == OP_INC ? I_INC : I_DEC, 0);
    store_target(C, t, ST_ASSIGN);
    if (post)
        op(C, I_POP, -1);
}

static void ex_unary(struct cg *C, struct ast *n)
{
    if (n->op == OP_TYPEOF && n->a->kind == N_IDENT) {
        load_name(C, n->a->str, 1);
        op(C, I_TYPEOF, 0);
        return;
    }
    if (n->op == OP_DELETE) {
        if (n->a->kind == N_MEMBER) {
            ex(C, n->a->a);
            op1(C, I_DELPROP, 0, cg_str(C, n->a->str));
        } else if (n->a->kind == N_INDEX) {
            ex(C, n->a->a);
            ex(C, n->a->b);
            op(C, I_DELELEM, -1);
        } else {
            op(C, I_TRUE, 1);
        }
        return;
    }
    ex(C, n->a);
    switch (n->op) {
    case OP_NOT: op(C, I_NOT, 0); return;
    case OP_BNOT: op(C, I_BNOT, 0); return;
    case OP_NEG: op(C, I_NEG, 0); return;
    case OP_PLUS: op(C, I_PLUS, 0); return;
    case OP_TYPEOF: op(C, I_TYPEOF, 0); return;
    case OP_VOID: op(C, I_VOID, 0); return;
    }
}

static void ex_object(struct cg *C, struct ast *n)
{
    op(C, I_OBJECT, 1);
    for (int i = 0; i < n->n; i++) {
        struct ast *p = n->list[i];
        if (p->kind == N_SPREAD) {
            ex(C, p->a);
            op(C, I_OSPREAD, -1);
            continue;
        }
        int computed = p->b != NULL;
        if (computed) {
            ex(C, p->b);
            op(C, I_TOKEY, 0);
        }
        ex(C, p->a);
        int code = p->kind == N_GETTER ? (computed ? I_OGETC : I_OGET)
                 : p->kind == N_SETTER ? (computed ? I_OSETC : I_OSET)
                                       : (computed ? I_OPROPC : I_OPROP);
        if (computed)
            op(C, code, -2);
        else
            op1(C, code, -1, cg_str(C, p->str));
    }
}

static void ex_class(struct cg *C, struct ast *n)
{
    if (n->a)
        ex(C, n->a);
    struct vscope *cs = n->vm, *saved = C->sc;
    C->sc = cs;
    if (cs->has_env)
        op1(C, I_PUSHENV, 0, cs->nslots);
    bind_scope_names(C,cs);
    op2(C, I_CLASS, n->a ? 1 : 2, cg_node(C, n), n->a != NULL);
    if (n->str) {                       /* the class's own name, for its methods */
        op1(C, I_PICK, 1, 1);
        store_decl(C, sc_find(cs, n->str));
        op(C, I_POP, -1);
    }
    for (int i = 0; i < n->n; i++) {
        struct ast *m = n->list[i];
        if (m->kind == N_FIELD && !m->op) {
            if (m->b) {
                ex(C, m->b);
                op(C, I_TOKEY, 0);
                store_name(C, m->c->b->str, ST_CONST);
                op(C, I_POP, -1);
            }
            continue;
        }
        if (m->kind == N_FIELD && m->op == 2) {         /* static { ... } */
            op1(C, I_CLOSURE, 1, cg_node(C, m->a));
            op(C, I_CSTATIC, -1);
            continue;
        }
        if (m->b) {
            ex(C, m->b);
            op(C, I_TOKEY, 0);
        } else {
            op1(C, I_CONST, 1, cg_str(C, m->str));
        }
        if (m->kind == N_FIELD) {                       /* static x = ... */
            if (m->d)
                op1(C, I_CLOSURE, 1, cg_node(C, m->d));
            else
                op(C, I_UNDEF, 1);
            op(C, I_CFIELD, -2);
            continue;
        }
        op1(C, I_CLOSURE, 1, cg_node(C, m->a));
        int kind = m->kind == N_GETTER ? CM_GETTER : m->kind == N_SETTER ? CM_SETTER : CM_METHOD;
        op1(C, I_CMETHOD, -2, kind | (m->op ? CM_STATIC : 0));
    }
    op(C, I_POP, -1);                   /* the prototype */
    if (cs->has_env)
        op(C, I_POPENV, 0);
    C->sc = saved;
}

static void ex_function(struct cg *C, struct ast *fn)
{
    struct vfunc *f = fn->vm;
    lay_out(f);
    op1(C, f->named && f->named->has_env ? I_CLOSUREN : I_CLOSURE, 1, cg_node(C, fn));
}

static void ex_inner(struct cg *C, struct ast *n);
static void ex(struct cg *C, struct ast *n)
{
    if (++C->depth > 512)
        bail(C->J, "a script is nested too deeply to compile");
    ex_inner(C, n);
    C->depth--;
}

static void ex_inner(struct cg *C, struct ast *n)
{
    switch (n->kind) {
    case N_NUM:
        if (n->flags & 1) {             /* a BigInt literal */
            struct jsbig *g = jb_parse(C->J, n->str->s, n->str->len, 0, 0);
            op1(C, I_CONST, 1, cg_const(C, g ? js_bigv(g) : js_num(NAN)));
            return;
        }
        op1(C, I_CONST, 1, cg_const(C, js_num(n->num)));
        return;
    case N_STR: op1(C, I_CONST, 1, cg_str(C, n->str)); return;
    case N_TRUE: op(C, I_TRUE, 1); return;
    case N_FALSE: op(C, I_FALSE, 1); return;
    case N_NULL: op(C, I_NULL, 1); return;
    case N_NEWTARGET: op(C, I_NEWTARGET, 1); return;
    case N_THIS:
        op(C, C->vf->arrow || C->vf->is_root ? I_THISDYN : I_THIS, 1);
        return;
    case N_IDENT: load_name(C, n->str, 0); return;
    case N_TEMPLATE:
        op1(C, I_CONST, 1, cg_str(C, js_mkstr(C->J, "", 0)));
        for (int i = 0; i < n->n; i++) {
            ex(C, n->list[i]);
            op(C, I_TOKEY, 0);
            op(C, I_ADD, -1);
        }
        return;
    case N_REGEX: op1(C, I_REGEX, 1, cg_node(C, n)); return;
    case N_ARRAY:
        op(C, I_ARRAY, 1);
        for (int i = 0; i < n->n; i++) {
            struct ast *x = n->list[i];
            if (x->kind == N_HOLE) {
                op(C, I_AHOLE, 0);
            } else if (x->kind == N_SPREAD) {
                ex(C, x->a);
                op(C, I_ASPREAD, -1);
            } else {
                ex(C, x);
                op(C, I_APUSH, -1);
            }
        }
        return;
    case N_OBJECT: ex_object(C, n); return;
    case N_FUNC: ex_function(C, n); return;
    case N_CLASS: ex_class(C, n); return;
    case N_AWAIT: ex(C, n->a); op(C, I_AWAIT, 0); return;
    case N_YIELD:
        if (n->a)
            ex(C, n->a);
        else
            op(C, I_UNDEF, 1);
        op(C, n->op ? I_YIELDSTAR : I_YIELD, 0);
        return;
    case N_IMPORT:
        ex(C, n->a);
        op1(C, I_IMPORT, 0, cg_str(C, n->str));
        return;
    case N_IMPORT_META: {
        struct vscope *dyn = NULL;
        cg_resolve(C, C->J->s_meta, &dyn);
        op2(C, I_TYPEOFNAME, 1, dyn ? hops_to(C, dyn) : 0, cg_str(C, C->J->s_meta));
        return;
    }
    case N_MEMBER:
    case N_INDEX: ex_member(C, n); return;
    case N_CALL: ex_call(C, n); return;
    case N_NEW: ex_new(C, n); return;
    case N_UNARY: ex_unary(C, n); return;
    case N_PREINC:
    case N_POSTINC: ex_incdec(C, n); return;
    case N_BINARY:
        ex(C, n->a);
        ex(C, n->b);
        op(C, binop(n->op), -1);
        return;
    case N_AND:
    case N_OR:
    case N_NULLISH: {
        ex(C, n->a);
        int done = jump(C, n->kind == N_AND ? I_JFALSEK : n->kind == N_OR ? I_JTRUEK : I_JNOTNULLK, -1);
        ex(C, n->b);
        land(C, done);
        return;
    }
    case N_COND: {
        ex(C, n->a);
        int other = jump(C, I_JFALSE, -1);
        ex(C, n->b);
        int done = jump(C, I_JUMP, 0);
        C->sp--;
        land(C, other);
        ex(C, n->c);
        land(C, done);
        return;
    }
    case N_SEQ:
        for (int i = 0; i < n->n; i++) {
            ex(C, n->list[i]);
            if (i < n->n - 1)
                op(C, I_POP, -1);
        }
        return;
    case N_ASSIGN: ex_assign(C, n); return;
    default:
        /* What the parser never puts in an expression: say so when it runs. */
        op1(C, I_CONST, 1, cg_str(C, js_mkstr(C->J, "SyntaxError: unexpected expression", 34)));
        op(C, I_THROW, -1);
        op(C, I_UNDEF, 1);
        return;
    }
}

/* ---- binding patterns ---- */

/* Bind the value on the stack to pattern p (consuming it). */
static void bind(struct cg *C, struct ast *p, int mode)
{
    switch (p->kind) {
    case N_IDENT:
        store_name(C, p->str, mode);
        op(C, I_POP, -1);
        return;
    case N_MEMBER:
    case N_INDEX: {
        int nbase = target_base(C, p);  /* v o (k) -> o (k) v */
        if (nbase == 1) {
            op(C, I_SWAP, 0);
        } else {
            op(C, I_ROT3, 0);
            op(C, I_ROT3, 0);
        }
        store_target(C, p, mode);
        op(C, I_POP, -1);
        return;
    }
    case N_ASSIGN: {                    /* a target with a default */
        int have = jump(C, I_JUNDEFK, -1);
        ex(C, p->b);
        if (p->a->kind == N_IDENT && is_anon_fn(p->b))
            set_fname(C, p->a->str);
        land(C, have);
        bind(C, p->a, mode);
        return;
    }
    case N_ARRAY: {
        int it = C->niters++;
        op1(C, I_ITEROPEN, -1, it);
        int thrown = jump(C, I_TRY, 0);
        if (++C->h > C->maxh)
            C->maxh = C->h;
        for (int i = 0; i < p->n; i++) {
            struct ast *x = p->list[i];
            if (x->kind == N_SPREAD) {
                op1(C, I_ITERREST, 1, it);
                bind(C, x->a, mode);
                break;
            }
            op1(C, I_ITERSTEP, 1, it);
            if (x->kind == N_HOLE)
                op(C, I_POP, -1);
            else
                bind(C, x, mode);
        }
        op(C, I_ENDTRY, 0);
        C->h--;
        op1(C, I_ITERCLOSE, 0, it);
        int end = jump(C, I_JUMP, 0);
        land(C, thrown);
        op(C, I_CATCHANY, 1);
        clear_inactive_scopes(C);
        op1(C, I_ITERCLOSE, 0, it);
        op(C, I_RETHROW, -1);
        land(C, end);
        return;
    }
    case N_OBJECT: {
        op(C, I_COERCIBLE, 0);
        int rest = -1;
        for (int i = 0; i < p->n; i++)
            if (p->list[i]->kind == N_SPREAD) {
                rest = cg_temp(C);
                op(C, I_ARRAY, 1);
                op1(C, I_SETREG, 0, rest);
                op(C, I_POP, -1);
            }
        for (int i = 0; i < p->n; i++) {
            struct ast *pr = p->list[i];
            if (pr->kind == N_SPREAD) {
                op(C, I_DUP, 1);
                op1(C, I_OBJREST, 0, rest);
                bind(C, pr->a, mode);
                continue;
            }
            op(C, I_DUP, 1);
            if (pr->b) {
                ex(C, pr->b);
                op(C, I_TOKEY, 0);
                if (rest >= 0)
                    op1(C, I_KEYPUSH, 0, rest);
                op(C, I_GETELEM, -1);
            } else {
                if (rest >= 0) {
                    op1(C, I_CONST, 1, cg_str(C, pr->str));
                    op1(C, I_KEYPUSH, 0, rest);
                    op(C, I_POP, -1);
                }
                op1(C, I_GETPROP, 0, cg_str(C, pr->str));
            }
            bind(C, pr->a, mode);
        }
        op(C, I_POP, -1);
        return;
    }
    default:
        op(C, I_POP, -1);
        op1(C, I_CONST, 1, cg_str(C, js_mkstr(C->J, "SyntaxError: invalid destructuring target", 41)));
        op(C, I_THROW, -1);
        return;
    }
}

/* ---- statements ---- */

static void patches_to(struct cg *C, struct patches *p, int target)
{
    for (int i = 0; i < p->n; i++)
        C->code[p->at[i]] = (uint32_t)target;
}

/* A loop or switch takes the labels written before it. */
static void take_labels(struct cg *C, struct ctl *c)
{
    c->labels = C->labels;
    c->nlabels = C->nlabels;
    C->labels = NULL;
    C->nlabels = 0;
}

static int has_label(struct ctl *c, struct jsstr *label)
{
    for (int i = 0; i < c->nlabels; i++)
        if (str_eq(c->labels[i], label))
            return 1;
    return 0;
}

static void emit_finally(struct cg *C, struct ctl *f)
{
    op1(C, I_FINCALL, 0, f->retreg);
    patches_add(C, &f->calls, C->n);
    cg_word(C, 0);
}

/* Leave everything from the innermost entry up to (not including) `to`. */
static void unwind_to(struct cg *C, struct ctl *to)
{
    for (struct ctl *c = C->ctl; c && c != to; c = c->up) {
        switch (c->kind) {
        case CT_SCOPE:
            clear_scope(C, c->sc);
            if (c->sc->has_env)
                op(C, I_POPENV, 0);
            break;
        case CT_ITER: op1(C, I_ITERCLOSE, 0, c->it); break;
        case CT_TRY: op(C, I_ENDTRY, 0); break;
        case CT_FINALLY:
            op(C, I_ENDTRY, 0);
            emit_finally(C, c);
            break;
        }
    }
}

static void st_jump(struct cg *C, struct ast *n)
{
    int brk = n->kind == S_BREAK;
    struct ctl *t;
    for (t = C->ctl; t; t = t->up) {
        if (n->str ? has_label(t, n->str) && (brk || t->kind == CT_LOOP)
                   : t->kind == CT_LOOP || (brk && t->kind == CT_SWITCH))
            break;
    }
    if (!t) {
        op1(C, I_CONST, 1, cg_str(C, js_mkstr(C->J, "SyntaxError: break or continue outside a loop", 45)));
        op(C, I_THROW, -1);
        return;
    }
    unwind_to(C, t);
    patches_add(C, brk ? &t->brk : &t->cont, jump(C, I_JUMP, 0));
}

static void st_return(struct cg *C, struct ast *n)
{
    if (n->a)
        ex(C, n->a);
    else
        op(C, I_UNDEF, 1);
    int cleanup = 0;
    for (struct ctl *c = C->ctl; c; c = c->up)
        cleanup |= c->kind == CT_ITER || c->kind == CT_TRY || c->kind == CT_FINALLY;
    if (cleanup) {
        int tmp = cg_temp(C);
        op1(C, I_SETREG, 0, tmp);
        op(C, I_POP, -1);
        unwind_to(C, NULL);
        op1(C, I_GETREG, 1, tmp);
    }
    op(C, I_RETURN, -1);
}

static int var_mode(int flags)
{
    return flags == V_VAR ? ST_VAR : flags == V_LET ? ST_LET : ST_CONST;
}

static void st_var(struct cg *C, struct ast *n)
{
    int mode = var_mode(n->flags);
    for (int i = 0; i < n->n; i++) {
        struct ast *d = n->list[i];
        if (d->kind == N_DESTRUCT) {
            if (!d->a) {
                op1(C, I_CONST, 1, cg_str(C, js_mkstr(C->J, "SyntaxError: a destructuring declaration needs a value", 54)));
                op(C, I_THROW, -1);
                continue;
            }
            ex(C, d->a);
            bind(C, d->b, mode);
            continue;
        }
        if (d->a) {
            ex(C, d->a);
            if (is_anon_fn(d->a))
                set_fname(C, d->str);
        } else if (mode == ST_VAR) {
            continue;
        } else {
            op(C, I_UNDEF, 1);
        }
        store_name(C, d->str, mode);
        op(C, I_POP, -1);
    }
}

/* A block's function declarations exist from its start; sloppy mode puts
 * them in the whole function too. */
static void hoist_block_functions(struct cg *C, struct ast **list, int n)
{
    for (int i = 0; i < n; i++) {
        if (list[i]->kind != S_FUNCDECL)
            continue;
        struct ast *fn = list[i]->a;
        ex_function(C, fn);
        store_name(C, fn->str, ST_LET);
        if (C->vf->is_root) {
            struct vscope *dyn = NULL;
            cg_resolve(C, fn->str, &dyn);
            if (!dyn) {
                for (dyn = C->sc; dyn && dyn->kind != SC_DYN; dyn = dyn->parent)
                    ;
            }
            op(C, I_DECLNAME, 0);
            cg_word(C, (uint32_t)(dyn ? hops_to(C, dyn) : 0));
            cg_word(C, (uint32_t)cg_str(C, fn->str));
            cg_word(C, 0);
        } else {
            struct vdecl *fd = sc_find(C->vf->scope, fn->str);
            if (fd && fd->scope != C->sc)
                store_decl(C, fd);
        }
        op(C, I_POP, -1);
    }
}

static void st_block(struct cg *C, struct ast *n)
{
    struct ctl env;
    struct vscope *s = n->vm, *saved = scope_enter(C, s, &env);
    if (s)
        hoist_block_functions(C, n->list, n->n);
    for (int i = 0; i < n->n; i++)
        st(C, n->list[i]);
    scope_leave(C, s, saved, &env);
}

/* Bind a for-in or for-of loop's variable to the value on the stack. */
static void bind_loop_target(struct cg *C, struct ast *t)
{
    if (t->kind == S_VAR) {
        struct ast *d = t->list[0];
        if (d->kind == N_DESTRUCT)
            bind(C, d->b, var_mode(t->flags));
        else {
            store_name(C, d->str, t->flags == V_VAR ? ST_ASSIGN : var_mode(t->flags));
            op(C, I_POP, -1);
        }
        return;
    }
    bind(C, t, ST_ASSIGN);
}

static void st_while(struct cg *C, struct ast *n)
{
    struct ctl loop;
    ctl_push(C, &loop, CT_LOOP);
    take_labels(C, &loop);
    int top = C->n, exit = -1;
    if (n->kind == S_WHILE) {
        ex(C, n->a);
        exit = jump(C, I_JFALSE, -1);
        st(C, n->b);
        patches_to(C, &loop.cont, top);
    } else {
        st(C, n->b);
        patches_land(C, &loop.cont);
        ex(C, n->a);
        exit = jump(C, I_JFALSE, -1);
    }
    op1(C, I_LOOP, 0, top);
    land(C, exit);
    ctl_pop(C, &loop);
    patches_land(C, &loop.brk);
}

static void st_for(struct cg *C, struct ast *n)
{
    struct ctl env, loop;
    struct jsstr **labels = C->labels;
    int nlabels = C->nlabels;
    C->labels = NULL;
    C->nlabels = 0;
    struct vscope *s = n->vm, *saved = scope_enter(C, s, &env);
    if (n->a) {
        if (n->a->kind == S_VAR) {
            st_var(C, n->a);
        } else {
            ex(C, n->a);
            op(C, I_POP, -1);
        }
    }
    if (s && s->has_env)
        op(C, I_COPYENV, 0);            /* initializer closures keep their own environment */
    ctl_push(C, &loop, CT_LOOP);
    loop.labels = labels;
    loop.nlabels = nlabels;
    int top = C->n, exit = -1;
    if (n->b) {
        ex(C, n->b);
        exit = jump(C, I_JFALSE, -1);
    }
    st(C, n->d);
    patches_land(C, &loop.cont);
    if (s && s->has_env)
        op(C, I_COPYENV, 0);            /* closures keep this turn's variables */
    if (n->c) {
        ex(C, n->c);
        op(C, I_POP, -1);
    }
    op1(C, I_LOOP, 0, top);
    if (exit >= 0)
        land(C, exit);
    ctl_pop(C, &loop);
    patches_land(C, &loop.brk);
    scope_leave(C, s, saved, &env);
}

static void st_forin(struct cg *C, struct ast *n)
{
    struct ctl loop, env;
    ex(C, n->b);
    int it = C->niters++;
    op1(C, I_FORIN, -1, it);
    ctl_push(C, &loop, CT_LOOP);
    take_labels(C, &loop);
    int top = C->n;
    op1(C, I_ITERNEXT, 1, it);          /* pushes nothing when it jumps */
    int done = C->n;
    cg_word(C, 0);
    struct vscope *s = n->vm, *saved = scope_enter(C, s, &env);
    bind_loop_target(C, n->a);
    st(C, n->c);
    scope_leave(C, s, saved, &env);
    patches_land(C, &loop.cont);
    op1(C, I_LOOP, 0, top);
    land(C, done);
    ctl_pop(C, &loop);
    patches_land(C, &loop.brk);
    op1(C, I_ITERCLOSE, 0, it);
}

static void st_forof(struct cg *C, struct ast *n)
{
    struct ctl iter, tryc, loop, env;
    ex(C, n->b);
    int it = C->niters++;
    op1(C, n->op ? I_AITEROPEN : I_ITEROPEN, -1, it);
    ctl_push(C, &iter, CT_ITER);
    iter.it = it;
    int thrown = jump(C, I_TRY, 0);
    if (++C->h > C->maxh)
        C->maxh = C->h;
    ctl_push(C, &tryc, CT_TRY);
    ctl_push(C, &loop, CT_LOOP);
    take_labels(C, &loop);
    int top = C->n;
    op1(C, n->op ? I_AITERNEXT : I_ITERNEXT, 1, it);
    int done = C->n;
    cg_word(C, 0);
    struct vscope *s = n->vm, *saved = scope_enter(C, s, &env);
    bind_loop_target(C, n->a);
    st(C, n->c);
    scope_leave(C, s, saved, &env);
    patches_land(C, &loop.cont);
    op1(C, I_LOOP, 0, top);
    ctl_pop(C, &loop);
    ctl_pop(C, &tryc);
    ctl_pop(C, &iter);
    land(C, done);                      /* the iterator is finished */
    op(C, I_ENDTRY, 0);
    int end = jump(C, I_JUMP, 0);
    patches_land(C, &loop.brk);         /* left early: close the iterator */
    op(C, I_ENDTRY, 0);
    op1(C, I_ITERCLOSE, 0, it);
    int end2 = jump(C, I_JUMP, 0);
    land(C, thrown);                    /* something threw: close it, and throw on */
    C->h--;
    op(C, I_CATCHANY, 1);
    clear_inactive_scopes(C);
    int tmp = cg_temp(C);
    op1(C, I_SETREG, 0, tmp);
    op(C, I_POP, -1);
    op1(C, I_ITERCLOSE, 0, it);
    op1(C, I_GETREG, 1, tmp);
    op(C, I_RETHROW, -1);
    land(C, end);
    land(C, end2);
}

static void st_try(struct cg *C, struct ast *n)
{
    struct ctl fin, tryc, env;
    int fin_at = -1;
    if (n->c) {
        fin_at = jump(C, I_TRY, 0);
        if (++C->h > C->maxh)
            C->maxh = C->h;
        ctl_push(C, &fin, CT_FINALLY);
        fin.fin = n->c;
        fin.sc = C->sc;
        fin.retreg = cg_temp(C);
    }
    if (n->b) {
        int catch_at = jump(C, I_TRY, 0);
        if (++C->h > C->maxh)
            C->maxh = C->h;
        ctl_push(C, &tryc, CT_TRY);
        st(C, n->a);
        ctl_pop(C, &tryc);
        C->h--;
        op(C, I_ENDTRY, 0);
        int over = jump(C, I_JUMP, 0);
        land(C, catch_at);
        op(C, I_CATCH, 1);
        clear_inactive_scopes(C);
        struct vscope *s = n->vm, *saved = scope_enter(C, s, &env);
        if (n->str) {
            store_name(C, n->str, ST_LET);
            op(C, I_POP, -1);
        } else if (n->d) {
            bind(C, n->d, ST_LET);
        } else {
            op(C, I_POP, -1);
        }
        st(C, n->b);
        scope_leave(C, s, saved, &env);
        land(C, over);
    } else {
        st(C, n->a);
    }
    if (n->c) {
        ctl_pop(C, &fin);
        C->h--;
        op(C, I_ENDTRY, 0);
        emit_finally(C, &fin);
        int end = jump(C, I_JUMP, 0);
        land(C, fin_at);
        op(C, I_CATCHANY, 1);
        clear_inactive_scopes(C);
        int tmp = cg_temp(C);
        op1(C, I_SETREG, 0, tmp);
        op(C, I_POP, -1);
        emit_finally(C, &fin);
        op1(C, I_GETREG, 1, tmp);
        op(C, I_RETHROW, -1);
        patches_land(C, &fin.calls);
        st(C, n->c);                    /* one copy, shared by every completion */
        op1(C, I_FINRET, 0, fin.retreg);
        land(C, end);
    }
}

static void st_switch(struct cg *C, struct ast *n)
{
    struct ctl env, sw;
    ex(C, n->a);
    int d = cg_temp(C);
    op1(C, I_SETREG, 0, d);
    op(C, I_POP, -1);
    struct jsstr **labels = C->labels;
    int nlabels = C->nlabels;
    C->labels = NULL;
    C->nlabels = 0;
    struct vscope *s = n->vm, *saved = scope_enter(C, s, &env);
    ctl_push(C, &sw, CT_SWITCH);
    sw.labels = labels;
    sw.nlabels = nlabels;
    for (int i = 0; i < n->n; i++)
        hoist_block_functions(C, n->list[i]->list, n->list[i]->n);
    int *at = js_alloc_atomic(C->J, sizeof(int) * (size_t)(n->n + 1));
    int deflt = -1;
    for (int i = 0; i < n->n; i++) {
        struct ast *cs = n->list[i];
        if (!cs->a) {
            deflt = i;
            continue;
        }
        op1(C, I_GETREG, 1, d);
        ex(C, cs->a);
        op(C, I_SEQ, -1);
        at[i] = jump(C, I_JTRUE, -1);
    }
    int none = jump(C, I_JUMP, 0);
    for (int i = 0; i < n->n; i++) {
        struct ast *cs = n->list[i];
        if (cs->a)
            land(C, at[i]);
        if (i == deflt)
            land(C, none);
        for (int k = 0; k < cs->n; k++)
            st(C, cs->list[k]);
    }
    if (deflt < 0)
        land(C, none);
    ctl_pop(C, &sw);
    patches_land(C, &sw.brk);
    scope_leave(C, s, saved, &env);
}

static void st_inner(struct cg *C, struct ast *n);
static void st(struct cg *C, struct ast *n)
{
    if (++C->depth > 512)
        bail(C->J, "a script is nested too deeply to compile");
    st_inner(C, n);
    C->depth--;
}

static void st_inner(struct cg *C, struct ast *n)
{
    switch (n->kind) {
    case S_EMPTY:
    case S_FUNCDECL:                    /* made on entry to its scope */
    case S_IMPORT:                      /* bound when the module was linked */
        return;
    case S_EXPORT:
        if (n->op == EX_DECL) {
            st(C, n->a);
        } else if (n->op == EX_DEFAULT) {
            ex(C, n->a);
            struct vscope *dyn = NULL;
            cg_resolve(C, C->J->s_default, &dyn);
            op1(C, I_EXPORTDEF, -1, dyn ? hops_to(C, dyn) : 0);
        }
        return;
    case S_EXPR:
        ex(C, n->a);
        op(C, C->vf->is_root ? I_COMPLETION : I_POP, -1);
        return;
    case S_VAR: st_var(C, n); return;
    case S_BLOCK: st_block(C, n); return;
    case S_IF: {
        ex(C, n->a);
        int other = jump(C, I_JFALSE, -1);
        st(C, n->b);
        if (n->c) {
            int done = jump(C, I_JUMP, 0);
            land(C, other);
            st(C, n->c);
            land(C, done);
        } else {
            land(C, other);
        }
        return;
    }
    case S_WHILE:
    case S_DOWHILE: st_while(C, n); return;
    case S_FOR: st_for(C, n); return;
    case S_FORIN: st_forin(C, n); return;
    case S_FOROF: st_forof(C, n); return;
    case S_LABEL: {
        if (C->nlabels % 8 == 0) {
            struct jsstr **nl = js_alloc(C->J, sizeof(*nl) * (size_t)(C->nlabels + 8));
            if (C->nlabels)
                memcpy(nl, C->labels, sizeof(*nl) * (size_t)C->nlabels);
            C->labels = nl;
        }
        C->labels[C->nlabels++] = n->str;
        int k = n->a->kind;
        if (k == S_LABEL || k == S_FOR || k == S_FORIN || k == S_FOROF || k == S_WHILE ||
            k == S_DOWHILE || k == S_SWITCH) {
            st(C, n->a);
            return;
        }
        struct ctl lab;
        ctl_push(C, &lab, CT_LABEL);
        take_labels(C, &lab);
        st(C, n->a);
        ctl_pop(C, &lab);
        patches_land(C, &lab.brk);
        return;
    }
    case S_RETURN: st_return(C, n); return;
    case S_BREAK:
    case S_CONTINUE: st_jump(C, n); return;
    case S_THROW:
        ex(C, n->a);
        op(C, I_THROW, -1);
        return;
    case S_TRY: st_try(C, n); return;
    case S_SWITCH: st_switch(C, n); return;
    default:
        op1(C, I_CONST, 1, cg_str(C, js_mkstr(C->J, "SyntaxError: unexpected statement", 33)));
        op(C, I_THROW, -1);
        return;
    }
}

/* ---- compiling a function ---- */

static struct vproto *cg_finish(struct cg *C)
{
    struct vproto *p = js_alloc(C->J, sizeof(*p));
    p->code = C->code;
    p->ncode = C->n;
    p->k = C->k;
    p->nk = C->nk;
    p->nregs = C->nregs;
    p->maxsp = C->maxsp;
    p->niters = C->niters;
    p->maxh = C->maxh;
    p->vf = C->vf;
    C->vf->proto = p;
    return p;
}

static struct vproto *compile_function(struct js *J, struct vfunc *vf)
{
    if (vf->proto)
        return vf->proto;
    lay_out(vf);
    struct ast *fn = vf->ast;
    struct cg C = { .J = J, .vf = vf, .sc = vf->params, .nregs = vf->nregs };
    if (vf->params->has_env)
        op1(&C, I_FUNCENV, 0, vf->params->nslots);
    bind_scope_names(&C,vf->params);
    if (vf->args) {
        op(&C, I_ARGUMENTS, 1);
        store_decl(&C, vf->args);
        op(&C, I_POP, -1);
    }
    for (int i = 0; i < fn->n; i++) {
        struct ast *p = fn->list[i];
        if ((fn->flags & F_REST) && i == fn->n - 1) {
            op1(&C, I_REST, 1, i);
        } else {
            op1(&C, I_ARG, 1, i);
            if (p->a) {
                int have = jump(&C, I_JUNDEFK, -1);
                ex(&C, p->a);
                land(&C, have);
            }
        }
        if (p->kind == N_DESTRUCT) {
            bind(&C, p->b, ST_LET);
        } else {
            store_name(&C, p->str, ST_LET);
            op(&C, I_POP, -1);
        }
    }
    if (fn->flags & F_EXPRBODY) {
        ex(&C, fn->a);
        op(&C, I_RETURN, -1);
        return cg_finish(&C);
    }
    struct fninfo *fi = fn->info;
    C.sc = vf->scope;
    if (vf->scope != vf->params) {
        if (vf->scope->has_env)
            op1(&C, I_PUSHENV, 0, vf->scope->nslots);
        bind_scope_names(&C,vf->scope);
        /* A body var that redeclares a parameter starts with its value,
         * while closures in defaults keep the parameter environment. */
        for (int i = 0; fi && i < fi->nvars; i++) {
            struct vdecl *pd = sc_find(vf->params, fi->vars[i]);
            if (pd) {
                load_decl(&C, pd);
                store_decl(&C, sc_find(vf->scope, fi->vars[i]));
                op(&C, I_POP, -1);
            }
        }
    }
    for (int i = 0; fi && i < fi->nfuncs; i++) {
        ex_function(&C, fi->funcs[i]);
        store_name(&C, fi->funcs[i]->str, ST_LET);
        op(&C, I_POP, -1);
    }
    for (int i = 0; fn->a && i < fn->a->n; i++)
        st(&C, fn->a->list[i]);
    op(&C, I_RETUNDEF, 0);
    return cg_finish(&C);
}

/* A script's (or module's, or eval'd source's) own code. */
static struct vproto *compile_root(struct js *J, struct vfunc *vf)
{
    if (vf->proto)
        return vf->proto;
    lay_out(vf);
    struct cg C = { .J = J, .vf = vf, .sc = vf->scope, .nregs = vf->nregs };
    struct ast *prog = vf->ast;
    for (int i = 0; i < prog->n; i++)
        st(&C, prog->list[i]);
    op(&C, I_RETUNDEF, 0);
    return cg_finish(&C);
}

/* ---- the machine ---- */

struct viter {
    struct iter it;
    int done;                           /* finished, or closed */
    int async;                          /* for await over an async iterator */
    jsval aobj, anext;
};

struct vhandler {
    int pc, sp;
    struct env *e;
};

static struct env *env_new(struct js *J, struct env *parent, int nslots)
{
    struct env *e = js_alloc(J, sizeof(*e) + sizeof(jsval) * (size_t)nslots);
    e->parent = parent;
    e->nslots = nslots;
    return e;
}

static struct env *env_up(struct env *e, uint32_t hops)
{
    while (hops--)
        e = e->parent;
    return e;
}

/* var x = v at the top level: wherever the hoisted x is (exec_var's way). */
static void var_init(struct js *J, struct env *e, struct jsstr *name, jsval v)
{
    struct env *s = e;
    struct binding *b = NULL;
    for (; s; s = s->parent) {
        for (b = s->vars; b; b = b->next)
            if (str_eq(b->name, name))
                break;
        if (b || s->obj || s->has_self)
            break;
    }
    if (b) {
        if(b->konst){js_throw_error(J,"TypeError: assignment to constant variable");return;}
        if(b->slot)*b->slot=v;else b->v=v;
    } else if (s && s->obj)
        js_put(J, s->obj, name, v, 0);
    else if (s)
        declare(J, s, name, v, 0);
    else
        write_var(J, e, name, v);
}

static void name_function(jsval v, struct jsstr *name)
{
    if (js_is_callable(v) && v.u.o->fn && (!v.u.o->name || !*v.u.o->name))
        v.u.o->name = name->s;
}

/* super(...) in a constructor: the parent runs on the same this. */
static jsval vm_super_call(struct js *J, struct env *e, jsval *args, int argc)
{
    struct jsobj *obj;
    struct binding *b = lookup(e, J->s_home, &obj);
    if (!b || b->v.t != JS_OBJ)
        return js_throw_error(J, "SyntaxError: 'super' is only valid in a class constructor");
    jsval ctor = js_get(J, b->v, "constructor");
    TRY();
    struct jsobj *parent = ctor.t == JS_OBJ ? ctor.u.o->proto : NULL;
    if (!parent || parent == J->function_proto || !js_is_callable(js_obj(parent)))
        return js_throw_error(J, "TypeError: super constructor is not a constructor");
    jsval self = this_value(e);
    if (self.t != JS_OBJ)
        return js_throw_error(J, "ReferenceError: 'this' is not available here");
    while (parent->bound)
        parent = parent->bound;
    if (parent->native) {
        struct jsobj *saved = J->super_this;
        J->super_this = self.u.o;
        jsval made = js_construct(J, js_obj(parent), args, argc);
        J->super_this = saved;
        TRY();
        if (made.t == JS_OBJ && made.u.o != self.u.o)
            adopt(J, self.u.o, made.u.o);
    } else {
        step(J);
        call_user(J, parent, self, args, argc);
        TRY();
    }
    return self;
}

/* super.name: looked up from the home object's prototype, with this as the receiver. */
static jsval vm_super_get(struct js *J, struct env *e, jsval key)
{
    struct jsobj *obj;
    struct binding *b = lookup(e, J->s_home, &obj);
    if (!b || b->v.t != JS_OBJ)
        return js_throw_error(J, "SyntaxError: 'super' is only valid inside a method");
    struct jsobj *proto = b->v.u.o->proto;
    if (!proto)
        return js_undefined;
    struct jsstr *k = js_tostr(J, key);
    TRY();
    jsval out;
    if (!obj_get(J, proto, k, this_value(e), &out))
        return js_undefined;
    return out;
}

static jsval vm_class(struct js *J, struct ast *n, struct env *e, jsval parent, int extends, jsval *proto_out)
{
    struct jsobj *pproto = J->object_proto;
    if (extends) {
        if (parent.t == JS_NULL) {
            pproto = NULL;
        } else if (!js_is_callable(parent) || parent.u.o->arrow) {
            return js_throw_error(J, "TypeError: class extends value is not a constructor or null");
        } else {
            jsval pp = js_get(J, parent, "prototype");
            TRY();
            pproto = pp.t == JS_OBJ ? pp.u.o : NULL;
        }
    }
    struct jsobj *proto = js_mkobj(J, C_OBJECT, pproto);
    jsval fv = make_function(J, n->b, e);
    struct jsobj *F = fv.u.o;
    F->is_class = 1;
    F->home = proto;
    F->name = n->str ? n->str->s : "";
    if (extends && parent.t == JS_OBJ)
        F->proto = parent.u.o;                  /* static members are inherited */
    js_put(J, F, J->s_prototype, js_obj(proto), 1);
    js_put(J, proto, js_mkstr(J, "constructor", 11), fv, 1);
    *proto_out = js_obj(proto);
    return fv;
}

static void define_accessor(struct js *J, struct jsobj *o, struct jsstr *key, struct jsobj *f, int setter, int hidden)
{
    struct prop *pr = js_own(o, key);
    if (!pr) {
        js_put(J, o, key, js_undefined, hidden);
        pr = js_own(o, key);
    }
    if (setter)
        pr->setter = f;
    else
        pr->getter = f;
}

/* The strings of a tagged template, with .raw. */
static jsval template_strings(struct js *J, struct ast *tl)
{
    struct jsobj *strings = js_new_array(J), *raw = js_new_array(J);
    for (int i = 0; i < tl->n; i += 2) {
        js_array_push(J, strings, strv(tl->list[i]->str));
        js_array_push(J, raw, strv(tl->list[i]->a ? tl->list[i]->a->str : tl->list[i]->str));
    }
    js_put(J, strings, js_mkstr(J, "raw", 3), js_obj(raw), 1);
    return js_obj(strings);
}

/* yield*: pass on everything the other iterable gives. */
static void yield_star(struct js *J, jsval src)
{
    struct iter it;
    if (js_iter_open(J, src, &it) < 0)
        return;
    for (;;) {
        jsval item;
        int r = js_iter_next(J, &it, &item);
        if (r <= 0)
            return;
        yield_value(J, item);
        if (J->throwing) {              /* thrown in, or the generator is being closed */
            js_iter_close(J, &it);
            return;
        }
    }
}

static void viter_done(struct viter *vi)
{
    memset(vi, 0, sizeof(*vi));
    vi->done = 1;
}

static int aiter_open(struct js *J, struct viter *vi, jsval obj)
{
    memset(vi, 0, sizeof(*vi));
    jsval m = obj.t == JS_OBJ ? js_get(J, obj, SYM_ASYNC_ITERATOR) : js_undefined;
    if (J->throwing)
        return -1;
    if (js_is_callable(m)) {
        vi->async = 1;
        vi->aobj = js_call_value(J, m, obj, NULL, 0);
        if (J->throwing)
            return -1;
        vi->anext = js_get(J, vi->aobj, "next");
        if (J->throwing)
            return -1;
        if (!js_is_callable(vi->anext)) {
            js_throw_error(J, "TypeError: the async iterator has no next() method");
            return -1;
        }
        return 0;
    }
    return js_iter_open(J, obj, &vi->it);
}

/* 1 and the next value, 0 when there is none, -1 if something threw. */
static int aiter_next(struct js *J, struct viter *vi, jsval *out)
{
    if (!vi->async) {
        int r = js_iter_next(J, &vi->it, out);
        if (r > 0) {
            *out = await_value(J, *out);
            if (J->throwing)
                return -1;
        }
        return r;
    }
    jsval res = js_call_value(J, vi->anext, vi->aobj, NULL, 0);
    if (J->throwing)
        return -1;
    res = await_value(J, res);
    if (J->throwing)
        return -1;
    if (res.t != JS_OBJ) {
        js_throw_error(J, "TypeError: iterator result is not an object");
        return -1;
    }
    jsval done = js_get(J, res, "done");
    if (J->throwing)
        return -1;
    if (js_truthy(done))
        return 0;
    *out = js_get(J, res, "value");
    return J->throwing ? -1 : 1;
}

/* A property of a plain object found without leaving it: the common case. */
static inline int own_data(jsval o, struct jsstr *key, jsval *out)
{
    if (o.t != JS_OBJ)
        return 0;
    struct jsobj *x = o.u.o;
    if (x->cls != C_OBJECT || x->phandler)
        return 0;
    struct prop *p = js_own(x, key);
    if (!p || p->getter || p->setter)
        return 0;
    *out = p->v;
    return 1;
}

#define VM_LOCAL_FRAME 48               /* registers and stack held in vm_run's own frame */
#define VM_LOCAL_ITERS 4
#define VM_LOCAL_HANDLERS 8

static jsval vm_run(struct js *J, struct vproto *P, struct jsobj *f, struct env *e, jsval self,
                    jsval *args, int argc)
{
    jsval local[VM_LOCAL_FRAME];
    struct viter local_iters[VM_LOCAL_ITERS];
    struct vhandler local_h[VM_LOCAL_HANDLERS];
    int nframe = P->nregs + P->maxsp + 1;
    jsval *frame = nframe <= VM_LOCAL_FRAME ? local : js_alloc(J, sizeof(jsval) * (size_t)nframe);
    struct viter *iters = P->niters <= VM_LOCAL_ITERS ? local_iters
                                                      : js_alloc(J, sizeof(*iters) * (size_t)P->niters);
    struct vhandler *hs = P->maxh <= VM_LOCAL_HANDLERS ? local_h
                                                       : js_alloc(J, sizeof(*hs) * (size_t)P->maxh);
    memset(frame, 0, sizeof(jsval) * (size_t)P->nregs);
    if (P->niters)
        memset(iters, 0, sizeof(*iters) * (size_t)P->niters);
    jsval *regs = frame, *stack = frame + P->nregs, *sp = stack;
    const uint32_t *code = P->code;
    const jsval *k = P->k;
    int pc = 0, nh = 0;
    jsval result = js_undefined;

    static const void *const disp[] = {
#define X(name) &&L_##name,
        OPCODES(X)
#undef X
    };
#ifdef JS_VM_VERIFY
#define NEXT do {                                                                                 \
    if (pc < 0 || pc >= P->ncode || code[pc] >= NOPS || sp < stack || sp > stack + P->maxsp ||       \
        nh < 0 || nh > P->maxh) {                                                                 \
        fprintf(stderr, "invalid VM frame: pc=%d stack=%ld/%d handlers=%d/%d\n",                   \
                pc, (long)(sp - stack), P->maxsp, nh, P->maxh);                                    \
        abort();                                                                                 \
    }                                                                                            \
    goto *disp[code[pc++]];                                                                       \
} while (0)
#else
#define NEXT goto *disp[code[pc++]]
#endif
#define A (code[pc])
#define B (code[pc + 1])
#define CHECK() do { if (J->throwing) goto unwind; } while (0)
#define KSTR(i) (k[i].u.s)
#define NUM2 (sp[-2].t == JS_NUM && sp[-1].t == JS_NUM)

    NEXT;

L_NOP: NEXT;
L_UNDEF: *sp++ = js_undefined; NEXT;
L_NULL: *sp++ = js_null; NEXT;
L_TRUE: *sp++ = (jsval){ JS_BOOL, { .b = 1 } }; NEXT;
L_FALSE: *sp++ = (jsval){ JS_BOOL, { .b = 0 } }; NEXT;
L_CONST: *sp++ = k[A]; pc++; NEXT;
L_POP: *--sp = js_undefined; NEXT;
L_DUP: sp[0] = sp[-1]; sp++; NEXT;
L_DUP2: sp[0] = sp[-2]; sp[1] = sp[-1]; sp += 2; NEXT;
L_SWAP: { jsval t = sp[-1]; sp[-1] = sp[-2]; sp[-2] = t; NEXT; }
L_ROT3: { jsval t = sp[-1]; sp[-1] = sp[-2]; sp[-2] = sp[-3]; sp[-3] = t; NEXT; }
L_ROT4: { jsval t = sp[-1]; sp[-1] = sp[-2]; sp[-2] = sp[-3]; sp[-3] = sp[-4]; sp[-4] = t; NEXT; }
L_PICK: sp[0] = sp[-1 - (int)A]; sp++; pc++; NEXT;
L_GETREG: *sp++ = regs[A]; pc++; NEXT;
L_SETREG: regs[A] = sp[-1]; pc++; NEXT;
L_GETENV: *sp++ = env_up(e, A)->slots[B]; pc += 2; NEXT;
L_SETENV: env_up(e, A)->slots[B] = sp[-1]; pc += 2; NEXT;
L_GETNAME: {
    jsval v = read_var(J, env_up(e, A), KSTR(B), 0);
    pc += 2;
    CHECK();
    *sp++ = v;
    NEXT;
}
L_TYPEOFNAME: {
    jsval v = read_var(J, env_up(e, A), KSTR(B), 1);
    pc += 2;
    CHECK();
    *sp++ = v;
    NEXT;
}
L_SETNAME:
    write_var(J, env_up(e, A), KSTR(B), sp[-1]);
    pc += 2;
    CHECK();
    NEXT;
L_DECLNAME: {
    struct env *x = env_up(e, A);
    if (x->obj)
        js_put(J, x->obj, KSTR(B), sp[-1], 0);
    else
        declare(J, x, KSTR(B), sp[-1], (int)code[pc + 2]);
    pc += 3;
    NEXT;
}
L_VARNAME:
    var_init(J, env_up(e, A), KSTR(B), sp[-1]);
    pc += 2;
    CHECK();
    NEXT;
L_CONSTERR:
    js_throw_error(J, "TypeError: assignment to constant variable '%s'", KSTR(A)->s);
    pc++;
    goto unwind;
L_NEWTARGET: {
    struct env *nt = e;
    while (nt && !nt->has_self) nt = nt->parent;
    *sp++ = nt ? nt->new_target : js_undefined;
    NEXT;
}
L_THIS: *sp++ = self; NEXT;
L_THISDYN: *sp++ = this_value(e); NEXT;
L_PUSHENV: e = env_new(J, e, (int)A); pc++; NEXT;
L_POPENV: e = e->parent; NEXT;
L_COPYENV: {
    struct env *x = env_new(J, e->parent, e->nslots);
    memcpy(x->slots, e->slots, sizeof(jsval) * (size_t)e->nslots);
    for(struct binding *b=e->vars;b;b=b->next) {
        struct binding *copy=declare(J,x,b->name,b->v,b->konst);
        if(b->slot)copy->slot=&x->slots[b->slot-e->slots];
    }
    e = x;
    NEXT;
}
L_BINDENV: {
    struct binding *binding=declare(J,e,KSTR(B),js_undefined,(int)A&1);
    binding->slot=&e->slots[(int)A/2];pc+=2;NEXT;
}
L_FUNCENV:
    e = env_new(J, e, (int)A);
    pc++;
    if (f && !f->arrow) {
        e->has_self = 1;
        e->self = self;
        e->new_target = J->new_target ? js_obj(J->new_target) : js_undefined;
    }
    if (f && f->home)
        declare(J, e, J->s_home, js_obj(f->home), 0);
    NEXT;
L_CLOSURE:
    *sp++ = make_function(J, (struct ast *)KSTR(A), e);
    pc++;
    NEXT;
L_CLOSUREN: {
    struct env *x = env_new(J, e, 1);
    jsval fv = make_function(J, (struct ast *)KSTR(A), x);
    x->slots[0] = fv;
    *sp++ = fv;
    pc++;
    NEXT;
}
L_ARGUMENTS: {
    struct jsobj *a = js_mkobj(J, C_ARGUMENTS, J->object_proto);
    for (int i = 0; i < argc; i++)
        js_array_push(J, a, args[i]);
    if (f)
        js_put(J, a, js_mkstr(J, "callee", 6), js_obj(f), 1);
    *sp++ = js_obj(a);
    NEXT;
}
L_REST: {
    struct jsobj *rest = js_new_array(J);
    for (int i = (int)A; i < argc; i++)
        js_array_push(J, rest, args[i]);
    *sp++ = js_obj(rest);
    pc++;
    NEXT;
}
L_ARG: *sp++ = (int)A < argc ? args[A] : js_undefined; pc++; NEXT;
L_ARRAY: *sp++ = js_obj(js_new_array(J)); NEXT;
L_APUSH: sp--; js_array_push(J, sp[-1].u.o, sp[0]); NEXT;
L_AHOLE: js_array_push(J, sp[-1].u.o, js_undefined); NEXT;
L_ASPREAD: {
    struct iter it;
    jsval item;
    int r;
    sp--;
    if (js_iter_open(J, sp[0], &it) < 0)
        goto unwind;
    while ((r = js_iter_next(J, &it, &item)) > 0)
        js_array_push(J, sp[-1].u.o, item);
    CHECK();
    NEXT;
}
L_OBJECT: *sp++ = js_obj(js_new_object(J)); NEXT;
L_OPROP: {
    sp--;
    name_function(sp[0], KSTR(A));
    js_put(J, sp[-1].u.o, KSTR(A), sp[0], 0);
    pc++;
    NEXT;
}
L_OPROPC: {
    sp -= 2;
    name_function(sp[1], sp[0].u.s);
    js_put(J, sp[-1].u.o, sp[0].u.s, sp[1], 0);
    NEXT;
}
L_OGET: sp--; define_accessor(J, sp[-1].u.o, KSTR(A), sp[0].u.o, 0, 0); pc++; NEXT;
L_OSET: sp--; define_accessor(J, sp[-1].u.o, KSTR(A), sp[0].u.o, 1, 0); pc++; NEXT;
L_OGETC: sp -= 2; define_accessor(J, sp[-1].u.o, sp[0].u.s, sp[1].u.o, 0, 0); NEXT;
L_OSETC: sp -= 2; define_accessor(J, sp[-1].u.o, sp[0].u.s, sp[1].u.o, 1, 0); NEXT;
L_OSPREAD: {
    sp--;
    struct jsstr **keys;
    int nk = js_keys(J, sp[0], 0, &keys);
    for (int i = 0; i < nk && !J->throwing; i++)
        js_put(J, sp[-1].u.o, keys[i], js_get_str(J, sp[0], keys[i]), 0);
    nk = js_own_symbols(J, sp[0], &keys);
    for (int i = 0; i < nk && !J->throwing; i++)
        js_put(J, sp[-1].u.o, keys[i], js_get_str(J, sp[0], keys[i]), 0);
    CHECK();
    NEXT;
}
L_GETPROP: {
    struct jsstr *key = KSTR(A);
    pc++;
    if (!own_data(sp[-1], key, &sp[-1])) {
        sp[-1] = js_get_str(J, sp[-1], key);
        CHECK();
    }
    NEXT;
}
L_GETELEM: {
    sp--;
    jsval o = sp[-1], key = sp[0];
    uint32_t i;
    if (o.t == JS_OBJ && o.u.o->cls == C_ARRAY && key.t == JS_NUM && js_index(key, &i)) {
        sp[-1] = i < (uint32_t)o.u.o->len ? o.u.o->items[i] : js_undefined;
        NEXT;
    }
    if (o.t == JS_OBJ && o.u.o->cls == C_HOST && key.t == JS_NUM && js_index(key, &i) &&
        js_typed_get_index(J, o.u.o, i, &sp[-1])) {
        CHECK();
        NEXT;
    }
    if (key.t == JS_STR && own_data(o, key.u.s, &sp[-1]))
        NEXT;
    sp[-1] = js_getv(J, o, key);
    CHECK();
    NEXT;
}
L_SETPROP: {
    sp--;
    jsval o = sp[-1], v = sp[0];
    pc++;
    if (o.t == JS_OBJ)
        obj_set(J, o.u.o, KSTR(code[pc - 1]), v, o);
    else
        js_setv(J, o, strv(KSTR(code[pc - 1])), v);
    CHECK();
    sp[-1] = v;
    NEXT;
}
L_SETELEM: {
    sp -= 2;
    jsval o = sp[-1], key = sp[0], v = sp[1];
    uint32_t i;
    if (o.t == JS_OBJ && o.u.o->cls == C_ARRAY && key.t == JS_NUM && js_index(key, &i) &&
        i < (uint32_t)o.u.o->len) {
        o.u.o->items[i] = v;
        sp[-1] = v;
        NEXT;
    }
    if (o.t == JS_OBJ && o.u.o->cls == C_HOST && key.t == JS_NUM && js_index(key, &i) &&
        js_typed_set_index(J, o.u.o, i, v)) {
        CHECK();
        sp[-1] = v;
        NEXT;
    }
    js_setv(J, o, key, v);
    CHECK();
    sp[-1] = v;
    NEXT;
}
L_DELPROP: sp[-1] = js_bool(js_delete(J, sp[-1], KSTR(A))); pc++; CHECK(); NEXT;
L_DELELEM: {
    sp--;
    struct jsstr *key = js_tostr(J, sp[0]);
    CHECK();
    sp[-1] = js_bool(js_delete(J, sp[-1], key));
    CHECK();
    NEXT;
}
L_SUPERGET: sp[-1] = vm_super_get(J, e, sp[-1]); CHECK(); NEXT;
L_SUPERCALL: {
    int n = (int)A;
    pc++;
    jsval r = vm_super_call(J, e, sp - n, n);
    sp -= n;
    CHECK();
    *sp++ = r;
    NEXT;
}
L_SUPERCALLS: {
    struct jsobj *arr = sp[-1].u.o;
    sp[-1] = vm_super_call(J, e, arr->items, arr->len);
    CHECK();
    NEXT;
}
L_CALL: {
    int n = (int)A, name = (int)B;
    pc += 2;
    jsval fn = sp[-n - 2];
    if (!js_is_callable(fn)) {
        if (name >= 0)
            js_throw_error(J, "TypeError: %s is not a function", KSTR(name)->s);
        else
            js_throw_error(J, "TypeError: %s is not a function",
                           fn.t == JS_UNDEF ? "undefined" : fn.t == JS_NULL ? "null" : "value");
        goto unwind;
    }
    struct env *saved_eval=J->eval_env;
    J->eval_env=name==-2 && fn.u.o->native==global_eval?e:NULL;
    jsval r = js_call_value(J, fn, sp[-n - 1], sp - n, n);
    J->eval_env=saved_eval;
    sp -= n + 2;
    CHECK();
    *sp++ = r;
    NEXT;
}
L_CALLS: {
    int name = (int)A;
    pc++;
    struct jsobj *arr = sp[-1].u.o;
    jsval fn = sp[-3];
    if (!js_is_callable(fn)) {
        js_throw_error(J, "TypeError: %s is not a function", name >= 0 ? KSTR(name)->s : "value");
        goto unwind;
    }
    struct env *saved_eval=J->eval_env;
    J->eval_env=name==-2 && fn.u.o->native==global_eval?e:NULL;
    jsval r = js_call_value(J, fn, sp[-2], arr->items, arr->len);
    J->eval_env=saved_eval;
    sp -= 3;
    CHECK();
    *sp++ = r;
    NEXT;
}
L_NEW: {
    int n = (int)A;
    pc++;
    jsval fn = sp[-n - 1];
    if (!js_is_callable(fn)) {
        js_throw_error(J, "TypeError: value is not a constructor");
        goto unwind;
    }
    jsval r = js_construct(J, fn, sp - n, n);
    sp -= n + 1;
    CHECK();
    *sp++ = r;
    NEXT;
}
L_NEWS: {
    struct jsobj *arr = sp[-1].u.o;
    jsval fn = sp[-2];
    if (!js_is_callable(fn)) {
        js_throw_error(J, "TypeError: value is not a constructor");
        goto unwind;
    }
    jsval r = js_construct(J, fn, arr->items, arr->len);
    sp -= 2;
    CHECK();
    *sp++ = r;
    NEXT;
}
L_OPTCALL:
    if (sp[-2].t == JS_UNDEF || sp[-2].t == JS_NULL) {
        sp -= 2;
        *sp++ = js_undefined;
        pc = (int)A;
        NEXT;
    }
    pc++;
    NEXT;
L_TEMPLATE: *sp++ = template_strings(J, (struct ast *)KSTR(A)); pc++; NEXT;
L_REGEX: {
    struct ast *n = (struct ast *)KSTR(A);
    pc++;
    if (!n->re) {
        js_throw_error(J, "SyntaxError: invalid regular expression /%s/: %s", n->str->s,
                       n->a ? n->a->str->s : "?");
        goto unwind;
    }
    struct jsobj *o = js_mkobj(J, C_REGEXP, J->regexp_proto);
    o->re = n->re;
    js_put(J, o, js_mkstr(J, "lastIndex", 9), js_num(0), 1);
    *sp++ = js_obj(o);
    NEXT;
}
L_ADD:
    sp--;
    if (sp[-1].t == JS_NUM && sp[0].t == JS_NUM) {
        sp[-1].u.n += sp[0].u.n;
        NEXT;
    }
    sp[-1] = add(J, sp[-1], sp[0]);
    CHECK();
    NEXT;
#define ARITH(name, expr, opcode)                       \
L_##name:                                               \
    if (NUM2) {                                         \
        double x = sp[-2].u.n, y = sp[-1].u.n;          \
        sp--;                                           \
        sp[-1].u.n = (expr);                            \
        NEXT;                                           \
    }                                                   \
    sp--;                                               \
    sp[-1] = binary_op(J, opcode, sp[-1], sp[0]);       \
    CHECK();                                            \
    NEXT;
    ARITH(SUB, x - y, OP_SUB)
    ARITH(MUL, x * y, OP_MUL)
    ARITH(DIV, x / y, OP_DIV)
    /* x % y for the common case, as binary_op has it (fmod, except by an infinity) */
    ARITH(MOD, isinf(y) && !isinf(x) ? x : fmod(x, y), OP_MOD)
#undef ARITH
#define BITS(name, expr, opcode)                        \
L_##name:                                               \
    if (NUM2) {                                         \
        int32_t x = (int32_t)to_int32(J, sp[-2]);       \
        uint32_t y = (uint32_t)to_int32(J, sp[-1]);     \
        sp--;                                           \
        sp[-1].u.n = (expr);                            \
        NEXT;                                           \
    }                                                   \
    sp--;                                               \
    sp[-1] = binary_op(J, opcode, sp[-1], sp[0]);       \
    CHECK();                                            \
    NEXT;
    BITS(SHL, (double)(int32_t)((uint32_t)x << (y & 31)), OP_SHL)
    BITS(SHR, (double)(x >> (y & 31)), OP_SHR)
    BITS(USHR, (double)((uint32_t)x >> (y & 31)), OP_USHR)
    BITS(BAND, (double)(x & (int32_t)y), OP_BAND)
    BITS(BOR, (double)(x | (int32_t)y), OP_BOR)
    BITS(BXOR, (double)(x ^ (int32_t)y), OP_BXOR)
#undef BITS
#define GENERIC(name, opcode)                           \
L_##name:                                               \
    sp--;                                               \
    sp[-1] = binary_op(J, opcode, sp[-1], sp[0]);       \
    CHECK();                                            \
    NEXT;
    GENERIC(EXP, OP_EXP)
    GENERIC(INSTANCEOF, OP_INSTANCEOF)
    GENERIC(IN, OP_IN)
#undef GENERIC
L_EQ: sp--; sp[-1] = js_bool(js_loose_equal(J, sp[-1], sp[0])); CHECK(); NEXT;
L_NE: sp--; sp[-1] = js_bool(!js_loose_equal(J, sp[-1], sp[0])); CHECK(); NEXT;
L_SEQ: sp--; sp[-1] = js_bool(js_strict_equal(sp[-1], sp[0])); NEXT;
L_SNE: sp--; sp[-1] = js_bool(!js_strict_equal(sp[-1], sp[0])); NEXT;
#define RELATION(name, test, opcode)                    \
L_##name:                                               \
    if (NUM2) {                                         \
        double x = sp[-2].u.n, y = sp[-1].u.n;          \
        sp--;                                           \
        sp[-1] = (jsval){ JS_BOOL, { .b = (test) } };   \
        NEXT;                                           \
    }                                                   \
    sp--;                                               \
    sp[-1] = binary_op(J, opcode, sp[-1], sp[0]);       \
    CHECK();                                            \
    NEXT;
    RELATION(LT, x < y, OP_LT)
    RELATION(GT, x > y, OP_GT)
    RELATION(LE, x <= y, OP_LE)
    RELATION(GE, x >= y, OP_GE)
#undef RELATION
L_NOT: sp[-1] = js_bool(!js_truthy(sp[-1])); NEXT;
L_NEG:
    sp[-1] = to_numeric(J, sp[-1]);
    CHECK();
    sp[-1] = sp[-1].t == JS_BIG ? js_bigv(jb_neg(J, sp[-1].u.g)) : js_num(-sp[-1].u.n);
    NEXT;
L_PLUS:
    if (sp[-1].t == JS_BIG) {
        js_throw_error(J, "TypeError: Cannot convert a BigInt value to a number");
        goto unwind;
    }
    sp[-1] = js_num(js_to_number(J, sp[-1]));
    CHECK();
    NEXT;
L_BNOT:
    sp[-1] = to_numeric(J, sp[-1]);
    CHECK();
    sp[-1] = sp[-1].t == JS_BIG ? js_bigv(jb_not(J, sp[-1].u.g)) : js_num((double)~to_int32(J, sp[-1]));
    NEXT;
L_TYPEOF: sp[-1] = js_str(J, type_of(sp[-1])); NEXT;
L_VOID: sp[-1] = js_undefined; NEXT;
L_TONUM:                                /* ToNumeric: BigInts stay */
    if (sp[-1].t != JS_NUM) {
        sp[-1] = to_numeric(J, sp[-1]);
        CHECK();
    }
    NEXT;
L_INC:
    if (sp[-1].t == JS_BIG)
        sp[-1] = js_bigv(jb_add(J, sp[-1].u.g, jb_from_u64(J, 1, 0)));
    else
        sp[-1].u.n += 1;
    NEXT;
L_DEC:
    if (sp[-1].t == JS_BIG)
        sp[-1] = js_bigv(jb_sub(J, sp[-1].u.g, jb_from_u64(J, 1, 0)));
    else
        sp[-1].u.n -= 1;
    NEXT;
L_TOKEY:                                /* a symbol stays one: its u.s is its key */
    if (sp[-1].t != JS_STR && sp[-1].t != JS_SYM) {
        struct jsstr *s = js_tostr(J, sp[-1]);
        CHECK();
        sp[-1] = strv(s);
    }
    NEXT;
L_JUMP: pc = (int)A; NEXT;
L_LOOP:
    J->steps += 8;
    if (J->steps > J->step_limit)
        bail(J, "a script ran too long and was stopped");
    pc = (int)A;
    NEXT;
L_JFALSE: sp--; pc = js_truthy(sp[0]) ? pc + 1 : (int)A; NEXT;
L_JTRUE: sp--; pc = js_truthy(sp[0]) ? (int)A : pc + 1; NEXT;
L_JFALSEK:
    if (!js_truthy(sp[-1])) {
        pc = (int)A;
        NEXT;
    }
    sp--;
    pc++;
    NEXT;
L_JTRUEK:
    if (js_truthy(sp[-1])) {
        pc = (int)A;
        NEXT;
    }
    sp--;
    pc++;
    NEXT;
L_JNOTNULLK:
    if (sp[-1].t != JS_UNDEF && sp[-1].t != JS_NULL) {
        pc = (int)A;
        NEXT;
    }
    sp--;
    pc++;
    NEXT;
L_JUNDEFK:
    if (sp[-1].t != JS_UNDEF) {
        pc = (int)A;
        NEXT;
    }
    sp--;
    pc++;
    NEXT;
L_JNULLISH:
    if (sp[-1].t == JS_UNDEF || sp[-1].t == JS_NULL) {
        sp[-1] = js_undefined;
        pc = (int)A;
        NEXT;
    }
    pc++;
    NEXT;
L_SETFNAME: name_function(sp[-1], KSTR(A)); pc++; NEXT;
L_RETURN: result = *--sp; goto out;
L_RETUNDEF: goto out;
L_THROW: sp--; js_throw_value(J, sp[0]); goto unwind;
L_TRY:
    hs[nh].pc = (int)A;
    hs[nh].sp = (int)(sp - stack);
    hs[nh].e = e;
    nh++;
    pc++;
    NEXT;
L_ENDTRY: nh--; NEXT;
L_FINCALL:
    regs[A] = js_num((double)(pc + 2));
    pc = (int)B;
    NEXT;
L_FINRET: pc = (int)regs[A].u.n; NEXT;
L_CLEARREGS:
    memset(regs + A, 0, sizeof(jsval) * (size_t)B);
    pc += 2;
    /* Values above sp are dead too, including in conservatively scanned
     * heap frames. Do this at statement/scope boundaries. */
    memset(sp, 0, sizeof(jsval) * (size_t)(stack + P->maxsp - sp));
    NEXT;
L_CATCH:
    if (J->exc.t == JS_OBJ && J->exc.u.o == J->return_marker)
        goto unwind;                    /* a generator being closed: only finally runs */
    /* fall through */
L_CATCHANY:
    *sp++ = J->exc;
    J->throwing = 0;
    J->exc = js_undefined;
    NEXT;
L_RETHROW: sp--; js_throw_value(J, sp[0]); goto unwind;
L_ITEROPEN: {
    struct viter *vi = &iters[A];
    pc++;
    memset(vi, 0, sizeof(*vi));
    sp--;
    if (js_iter_open(J, sp[0], &vi->it) < 0) {
        viter_done(vi);
        goto unwind;
    }
    NEXT;
}
L_ITERNEXT: {
    struct viter *vi = &iters[A];
    jsval out;
    int r = js_iter_next(J, &vi->it, &out);
    if (r < 0) {
        viter_done(vi);
        pc += 2;
        goto unwind;
    }
    if (!r) {
        viter_done(vi);
        pc = (int)B;
        NEXT;
    }
    *sp++ = out;
    pc += 2;
    NEXT;
}
L_ITERSTEP: {
    struct viter *vi = &iters[A];
    pc++;
    jsval out = js_undefined;
    if (!vi->done) {
        int r = js_iter_next(J, &vi->it, &out);
        if (r < 0) {
            viter_done(vi);
            goto unwind;
        }
        if (!r) {
            viter_done(vi);
            out = js_undefined;
        }
    }
    *sp++ = out;
    NEXT;
}
L_ITERREST: {
    struct viter *vi = &iters[A];
    pc++;
    struct jsobj *rest = js_new_array(J);
    jsval item;
    while (!vi->done) {
        int r = js_iter_next(J, &vi->it, &item);
        if (r < 0) {
            viter_done(vi);
            goto unwind;
        }
        if (!r)
            viter_done(vi);
        else
            js_array_push(J, rest, item);
    }
    *sp++ = js_obj(rest);
    NEXT;
}
L_ITERCLOSE: {
    struct viter *vi = &iters[A];
    pc++;
    if (!vi->done) {
        vi->done = 1;
        if (!vi->async)
            js_iter_close(J, &vi->it);
        viter_done(vi);
    }
    NEXT;
}
L_FORIN: {
    struct viter *vi = &iters[A];
    pc++;
    memset(vi, 0, sizeof(*vi));
    sp--;
    struct jsobj *keys = js_new_array(J);
    if (sp[0].t != JS_UNDEF && sp[0].t != JS_NULL) {
        struct jsstr **list;
        int nk = js_keys(J, sp[0], 1, &list);
        CHECK();
        for (int i = 0; i < nk; i++)
            js_array_push(J, keys, strv(list[i]));
    }
    vi->it.kind = IT_ARRAY;
    vi->it.arr = keys;
    NEXT;
}
L_AITEROPEN: {
    struct viter *vi = &iters[A];
    pc++;
    sp--;
    if (aiter_open(J, vi, sp[0]) < 0) {
        viter_done(vi);
        goto unwind;
    }
    NEXT;
}
L_AITERNEXT: {
    struct viter *vi = &iters[A];
    jsval out;
    int r = aiter_next(J, vi, &out);
    if (r < 0) {
        viter_done(vi);
        pc += 2;
        goto unwind;
    }
    if (!r) {
        viter_done(vi);
        pc = (int)B;
        NEXT;
    }
    *sp++ = out;
    pc += 2;
    NEXT;
}
L_COERCIBLE:
    if (sp[-1].t == JS_UNDEF || sp[-1].t == JS_NULL) {
        js_throw_error(J, "TypeError: cannot destructure '%s'", sp[-1].t == JS_NULL ? "null" : "undefined");
        goto unwind;
    }
    NEXT;
L_OBJREST: {
    struct jsobj *used = regs[A].u.o, *rest = js_new_object(J);
    pc++;
    jsval src = sp[-1];
    struct jsstr **keys;
    int nk = js_keys(J, src, 0, &keys);
    for (int i = 0; i < nk && !J->throwing; i++) {
        int skip = 0;
        for (int u = 0; u < used->len && !skip; u++)
            skip = (used->items[u].t == JS_STR || used->items[u].t == JS_SYM) && str_eq(used->items[u].u.s, keys[i]);
        if (!skip)
            js_put(J, rest, keys[i], js_get_str(J, src, keys[i]), 0);
    }
    CHECK();
    sp[-1] = js_obj(rest);
    NEXT;
}
L_KEYPUSH: js_array_push(J, regs[A].u.o, sp[-1]); pc++; NEXT;
L_AWAIT: sp[-1] = await_value(J, sp[-1]); CHECK(); NEXT;
L_YIELD: {
    jsval v = sp[-1];
    if (in_async_gen(J) && js_is_promise(v)) {     /* async: yield await v */
        v = await_value(J, v);
        CHECK();
    }
    sp[-1] = yield_value(J, v);
    CHECK();
    NEXT;
}
L_YIELDSTAR: yield_star(J, sp[-1]); sp[-1] = js_undefined; CHECK(); NEXT;
L_IMPORT: {
    struct jsstr *s = js_tostr(J, sp[-1]);
    CHECK();
    sp[-1] = mod_import(J, s->s, KSTR(A)->s);
    pc++;
    CHECK();
    NEXT;
}
L_COMPLETION: J->completion = *--sp; NEXT;
L_EXPORTDEF:
    sp--;
    if (js_is_callable(sp[0]) && sp[0].u.o->fn && (!sp[0].u.o->name || !*sp[0].u.o->name))
        sp[0].u.o->name = "default";
    declare(J, env_up(e, A), J->s_default, sp[0], 0);
    pc++;
    NEXT;
L_CLASS: {
    struct ast *n = (struct ast *)KSTR(A);
    int extends = (int)B;
    pc += 2;
    jsval parent = js_undefined, proto;
    if (extends)
        parent = *--sp;
    jsval F = vm_class(J, n, e, parent, extends, &proto);
    CHECK();
    *sp++ = F;
    *sp++ = proto;
    NEXT;
}
L_CMETHOD: {
    int kind = (int)A;
    pc++;
    sp -= 2;
    struct jsstr *key = sp[0].u.s;
    struct jsobj *fo = sp[1].u.o, *target = (kind & CM_STATIC) ? sp[-2].u.o : sp[-1].u.o;
    fo->home = target;
    fo->name = key->s;
    if ((kind & 3) == CM_METHOD)
        js_put(J, target, key, sp[1], 1);
    else
        define_accessor(J, target, key, fo, (kind & 3) == CM_SETTER, 1);
    NEXT;
}
L_CFIELD: {
    sp -= 2;
    struct jsstr *key = sp[0].u.s;
    jsval F = sp[-2], v = js_undefined;
    if (sp[1].t == JS_OBJ) {
        sp[1].u.o->home = F.u.o;
        v = js_call_value(J, sp[1], F, NULL, 0);
        CHECK();
        name_function(v, key);
    }
    js_put(J, F.u.o, key, v, 0);
    NEXT;
}
L_CSTATIC: {
    sp--;
    jsval F = sp[-2];
    sp[0].u.o->home = F.u.o;
    js_call_value(J, sp[0], F, NULL, 0);
    CHECK();
    NEXT;
}

unwind:
    if (nh > 0) {
        struct vhandler *h = &hs[--nh];
        pc = h->pc;
        sp = stack + h->sp;
        e = h->e;
        NEXT;
    }
    result = js_undefined;
out:
    return result;
#undef NEXT
#undef A
#undef B
#undef CHECK
#undef KSTR
#undef NUM2
}
