/* Guest COM identities and vtables. Only validated guest addresses cross here. */
#ifndef WIN_COMPAT_COM_H
#define WIN_COMPAT_COM_H
struct com_spec {int kind,first,count;uint8_t iid[16];};
struct com_object {struct com_object *next;int refs,category;gp_t interfaces[24];void (*destroy)(struct com_object *);};
static struct com_object *com_objects;
static const uint8_t unknown_iid[16]={0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
static gp_t com_tables[24];
static struct com_object *com_find(gp_t p,int kind)
{for(struct com_object *o=com_objects;o;o=o->next)if(o->interfaces[kind]==p)return o;return NULL;}
static void com_init(struct com_object *o,int category,void (*destroy)(struct com_object *))
{o->refs=1;o->category=category;o->destroy=destroy;o->next=com_objects;com_objects=o;}
static uint32_t com_release(struct com_object *o)
{
    if(--o->refs)return o->refs;
    for(struct com_object **p=&com_objects;*p;p=&(*p)->next)if(*p==o){*p=o->next;break;}
    for(int i=0;i<24;i++)if(o->interfaces[i])gfree(o->interfaces[i]);
    o->destroy(o);return 0;
}
static gp_t com_interface(struct com_object *o,int kind,const struct com_spec *specs,int count,const struct api_entry *methods,struct api_dll *dll)
{
    if(o->interfaces[kind])return o->interfaces[kind];
    const struct com_spec *spec=NULL;for(int i=0;i<count;i++)if(specs[i].kind==kind)spec=&specs[i];if(!spec)return 0;
    if(!com_tables[kind]) {
        com_tables[kind]=vm_alloc_loader(spec->count*4);if(!com_tables[kind])return 0;
        for(int i=0;i<spec->count;i++)((gp_t *)GP(com_tables[kind]))[i]=api_thunk(dll,methods+spec->first+i);
    }
    gp_t p=galloc(4);if(!p)return 0;*(gp_t *)GP(p)=com_tables[kind];o->interfaces[kind]=p;return p;
}
#endif
