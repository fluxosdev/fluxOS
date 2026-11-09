#include <stdlib.h>
#include <string.h>
#include "web.h"

/* Interaction follows composed ancestry, including shadow hosts. */
static const struct node *interaction_parent(const struct node *n)
{
    return n->parent ? n->parent : n->shadow_host;
}
/* inert, or outside the document's topmost modal dialog (which its root names). */
int web_inert(const struct node *n)
{
    const struct node *root = n;
    for (const struct node *p = n; p; p = interaction_parent(p)) {
        if (node_attr(p, "inert")) return 1;
        root = p;
    }
    const struct node *modal = root ? root->top_modal : NULL;
    if (!modal) return 0;
    for (; n; n = interaction_parent(n))
        if (n == modal) return 0;
    return 1;
}
int web_disabled(const struct node *n)
{
    if (!n) return 1;
    if (node_attr(n, "disabled")) return 1;
    const struct node *child = n;
    for (const struct node *p = interaction_parent(n); p; child=p,p=interaction_parent(p)) {
        if (p->tag != T_FIELDSET || !node_attr(p,"disabled")) continue;
        const struct node *legend=p->first;
        while(legend && legend->tag != T_LEGEND) legend=legend->next;
        if(child != legend) return 1;
    }
    return 0;
}
int web_tabindex(const struct node *n)
{
    const char *v=node_attr(n,"tabindex");
    if(v){char *end;long index=strtol(v,&end,10);if(end!=v){if(index>32767)index=32767;if(index< -32768)index=-32768;return (int)index;}}
    if ((n->tag==T_A || n->tag==T_AREA) && node_attr(n,"href")) return 0;
    if (n->tag==T_INPUT && !strcasecmp(node_attr(n,"type")?node_attr(n,"type"):"text","hidden")) return -1;
    if(n->tag==T_INPUT||n->tag==T_TEXTAREA||n->tag==T_SELECT||n->tag==T_BUTTON||n->tag==T_SUMMARY||n->tag==T_IFRAME) return 0;
    v=node_attr(n,"contenteditable");
    return v && strcasecmp(v,"false") ? 0 : -1;
}
int web_focusable(const struct node *n)
{
    if(!n||n->type!=N_ELEMENT||web_inert(n)||web_disabled(n))return 0;
    for(const struct node *p=n;p;p=interaction_parent(p)) {
        if(node_attr(p,"hidden")||(p->style&&(p->style->display==D_NONE||p->style->hidden)))return 0;
        const struct node *parent=interaction_parent(p);
        if(parent&&parent->tag==T_DETAILS&&!node_attr(parent,"open")) {
            const struct node *first=parent->first;
            while(first&&first->tag!=T_SUMMARY)first=first->next;
            if(p!=first)return 0;
        }
    }
    return node_attr(n,"tabindex") || web_tabindex(n)>=0;
}
/* Sort positive indices first, then zero in composed document order. */
struct node *web_focus_next(struct node *root,struct node *current,int dir)
{
    struct node *first=NULL,*last=NULL,*next=NULL;int current_order=-1,order=0;
    for(struct node *n=root;n;n=node_next_shadow(n,root),order++)if(n==current)current_order=order;
    int ct=current?web_tabindex(current):-1;
    int cr=ct>0?ct:32768;order=0;
    int first_rank=32769,last_rank=-1,next_rank=dir>0?32769:-1;
    for(struct node *n=root;n;n=node_next_shadow(n,root),order++) {
        int t=web_tabindex(n);if(t<0||!web_focusable(n))continue;
        int rank=t>0?t:32768;
        if(!first||rank<first_rank){first=n;first_rank=rank;}
        if(!last||rank>=last_rank){last=n;last_rank=rank;}
        int after=rank>cr||(rank==cr&&order>current_order);
        int before=rank<cr||(rank==cr&&order<current_order);
        if(current_order<0)continue;
        if(dir>0&&after&&(!next||rank<next_rank)){next=n;next_rank=rank;}
        if(dir<0&&before&&(!next||rank>=next_rank)){next=n;next_rank=rank;}
    }
    return next?next:dir>0?first:last;
}
