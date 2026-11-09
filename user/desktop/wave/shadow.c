/* Temporarily compose the flat rendering tree, preserving the DOM's separate trees.
 * Layout does not call scripts. Every pointer changed here is restored before it returns. */
#include <string.h>
#include "web.h"
struct shadow_swap {
    struct node **where, *before;
    struct shadow_swap *next;
};
struct compose_context {
    struct arena *arena;
    struct shadow_swap *patches;
    int failed;
};
static void set(struct compose_context *ctx, struct node **where, struct node *value)
{
    struct shadow_swap *p = arena_alloc(ctx->arena, sizeof(*p));
    if (!p) {
        ctx->failed = 1;
        return;
    }
    *p = (struct shadow_swap){where, *where, ctx->patches};
    ctx->patches = p;
    *where = value;
}
struct node *shadow_slot(struct node *host, const char *name)
{
    if (!host || !host->shadow_root)
        return NULL;
    struct node *root = host->shadow_root;
    for (struct node *n = root; n; n = node_next(n, root))
        if (n->type == N_ELEMENT && !strcmp(n->name, "slot")) {
            const char *s = node_attr(n, "name");
            if (!strcmp(s ? s : "", name))
                return n;
        }
    return NULL;
}
static void compose(struct compose_context *ctx, struct node *root)
{
    struct arena *a = ctx->arena;
    for (struct node *n = root; n; n = node_next(n, root))
        if (n->shadow_root) {
            struct node *shadow = n->shadow_root;
            int count = 0;
            for (struct node *k = n->first; k; k = k->next)
                count++;
            struct node **light = arena_alloc(a, (size_t)(count ? count : 1) * sizeof(*light));
            if (!light)
                continue;
            int i = 0;
            for (struct node *k = n->first; k; k = k->next)
                light[i++] = k;
            int ns = 0;
            for (struct node *k = shadow; k; k = node_next(k, shadow))
                if (k->type == N_ELEMENT && !strcmp(k->name, "slot"))
                    ns++;
            struct node **slots = arena_alloc(a, (size_t)(ns ? ns : 1) * sizeof(*slots));
            if (!slots)
                continue;
            ns = 0;
            for (struct node *k = shadow; k; k = node_next(k, shadow))
                if (k->type == N_ELEMENT && !strcmp(k->name, "slot")) {
                    const char *name = node_attr(k, "name");
                    if (shadow_slot(n, name ? name : "") == k)
                        slots[ns++] = k;
                }
            for (int si = 0; si < ns; si++) {
                struct node *slot = slots[si];
                const char *name = node_attr(slot, "name");
                if (!name)
                    name = "";
                struct node *first = NULL, *last = NULL;
                for (i = 0; i < count; i++) {
                    struct node *k = light[i];
                    const char *s = node_attr(k, "slot");
                    if (strcmp(s ? s : "", name))
                        continue;
                    if (last)
                        set(ctx, &last->next, k);
                    else
                        first = k;
                    set(ctx, &k->prev, last);
                    set(ctx, &k->next, NULL);
                    set(ctx, &k->parent, slot);
                    last = k;
                }
                if (first) {
                    set(ctx, &slot->first, first);
                    set(ctx, &slot->last, last);
                }
            }
            set(ctx, &n->first, shadow->first);
            set(ctx, &n->last, shadow->last);
            for (struct node *k = n->first; k; k = k->next)
                set(ctx, &k->parent, n);
            if (ctx->failed)
                return;
            /* The outer walk will visit nested hosts through the composed children. */
        }
}
struct shadow_swap *shadow_compose(struct arena *a, struct node *root)
{
    struct compose_context ctx = {.arena = a};
    compose(&ctx, root);
    if (ctx.failed) {
        shadow_restore(ctx.patches);
        return NULL;
    }
    return ctx.patches;
}
void shadow_restore(struct shadow_swap *p)
{
    for (; p; p = p->next)
        *p->where = p->before;
}
