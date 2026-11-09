/* Drag and drop as the browser drives it (jsdom_drag_*), and what the page's scripts see of it. */
#define main browser_dom_main
#include "browser-dom.c"
#undef main

static struct node *by_id(struct node *root, const char *id)
{
    for (struct node *n = root; n; n = node_next(n, root))
        if (n->type == N_ELEMENT && node_attr(n, "id") && !strcmp(node_attr(n, "id"), id))
            return n;
    return NULL;
}

int main(void)
{
    mallopt(M_MMAP_MAX, 0);
    struct arena a = { 0 };
    const char *html = "<!doctype html><html><body><a id=link href='/page'>L</a><div id=src draggable=true>S</div>"
                       "<div id=zone>Z</div><div id=other>O</div></body></html>";
    struct node *root = html_parse(&a, html, (int)strlen(html));
    struct jshost h = { .arena = &a, .root = root, .url = "http://x/", .base = "http://x/", .status = status,
                        .changed = changed };
    struct jsdom *d = jsdom_new(&h);
    const char *setup =
        "function check(x, what) { if (!x) throw Error('drag: ' + what); }"
        "var log = [], src = document.getElementById('src'), zone = document.getElementById('zone');"
        "src.addEventListener('dragstart', function (e) {"
        "  check(e instanceof DragEvent && e.dataTransfer instanceof DataTransfer, 'a DragEvent');"
        "  e.dataTransfer.setData('text/plain', 'hello'); e.dataTransfer.setData('application/x-test', '42');"
        "  e.dataTransfer.effectAllowed = 'copyMove';"
        "  check(e.dataTransfer.types.join() === 'text/plain,application/x-test', 'types ' + e.dataTransfer.types);"
        "  check(e.dataTransfer.items.length === 2 && e.dataTransfer.items[0].kind === 'string', 'items');"
        "  log.push('start'); });"
        "src.addEventListener('drag', function (e) { log.push('drag'); });"
        "zone.addEventListener('dragenter', function (e) { e.preventDefault(); log.push('enter:' + e.dataTransfer.dropEffect); });"
        "zone.addEventListener('dragover', function (e) {"
        "  check(e.dataTransfer.getData('text') === '', 'data is protected while dragging over');"
        "  check(e.dataTransfer.types.indexOf('application/x-test') >= 0, 'types are not');"
        "  e.dataTransfer.effectAllowed = 'none';"   /* too late: only dragstart may */
        "  e.preventDefault(); e.dataTransfer.dropEffect = 'move'; log.push('over'); });"
        "zone.addEventListener('drop', function (e) {"
        "  if (!e.isTrusted) return;"   /* (the page's own drop, at the end) */
        "  e.preventDefault();"
        "  log.push('drop:' + e.dataTransfer.getData('text') + ':' + e.dataTransfer.getData('application/x-test') + ':' +"
        "           e.dataTransfer.dropEffect + ':' + e.buttons);"
        "  try { e.dataTransfer.setData('text/plain', 'changed'); } catch (x) {}"
        "  check(e.dataTransfer.getData('text/plain') === 'hello', 'data is read-only in drop'); });"
        "zone.addEventListener('dragleave', function (e) { log.push('leave'); });"
        "src.addEventListener('dragend', function (e) { log.push('end:' + e.dataTransfer.dropEffect); });";
    jsdom_run_script(d, NULL, setup, (int)strlen(setup), "drag setup");
    struct node *src = by_id(root, "src"), *zone = by_id(root, "zone"), *other = by_id(root, "other"),
                *link = by_id(root, "link");
    char effect[8];
    int bad = 0;
    jsdom_drag_begin(d, "", "", 0);
    bad |= jsdom_drag_event(d, src, "dragstart", 5, 5, NULL, NULL, NULL, 0) != 0;
    jsdom_drag_event(d, src, "drag", 10, 10, NULL, NULL, NULL, 0);
    bad |= jsdom_drag_event(d, zone, "dragenter", 10, 10, NULL, NULL, effect, sizeof(effect)) != 1;
    bad |= strcmp(effect, "copy") != 0;                 /* copyMove starts as copy */
    bad |= jsdom_drag_event(d, zone, "dragover", 10, 10, NULL, NULL, effect, sizeof(effect)) != 1;
    bad |= strcmp(effect, "move") != 0;
    bad |= jsdom_drag_event(d, zone, "drop", 10, 10, NULL, effect, NULL, 0) != 1;
    jsdom_drag_event(d, src, "dragend", 10, 10, NULL, effect, NULL, 0);
    jsdom_drag_end(d);
    if (bad)
        status(NULL, "drag: the browser's view of the first drag is wrong");
    const char *first = "check(log.join() === 'start,drag,enter:copy,over,drop:hello:42:move:0,end:move', 'log ' + log.join());"
                        "log = [];";
    jsdom_run_script(d, NULL, first, (int)strlen(first), "first drag");

    /* A link: it carries its address, a drop is a link at first, and where nobody takes it nothing happens. */
    const char *setup2 =
        "var a = document.getElementById('link'), seen = null;"
        "a.addEventListener('dragstart', function (e) { seen = [e.dataTransfer.getData('URL'), e.dataTransfer.getData('text'),"
        "  e.dataTransfer.effectAllowed]; });"
        "document.getElementById('other').addEventListener('dragover', function (e) { log.push('over2:' + e.dataTransfer.dropEffect); });";
    jsdom_run_script(d, NULL, setup2, (int)strlen(setup2), "link setup");
    jsdom_drag_begin(d, "http://x/page", "http://x/page", 1);
    bad = jsdom_drag_event(d, link, "dragstart", 1, 1, NULL, NULL, NULL, 0) != 0;
    bad |= jsdom_drag_event(d, other, "dragover", 1, 30, NULL, NULL, effect, sizeof(effect)) != 0 || strcmp(effect, "none");
    jsdom_drag_end(d);
    if (bad)
        status(NULL, "drag: the link drag went wrong");
    const char *second = "check(JSON.stringify(seen) === '[\"http://x/page\",\"http://x/page\",\"uninitialized\"]', 'link ' + JSON.stringify(seen));"
                         "check(log.join() === 'over2:link', 'link effect ' + log.join());";
    jsdom_run_script(d, NULL, second, (int)strlen(second), "link drag");

    /* dragstart prevented: no drag. */
    const char *setup3 = "src.addEventListener('dragstart', function (e) { e.preventDefault(); });";
    jsdom_run_script(d, NULL, setup3, (int)strlen(setup3), "refusing setup");
    jsdom_drag_begin(d, "", "", 0);
    if (jsdom_drag_event(d, src, "dragstart", 5, 5, NULL, NULL, NULL, 0) != 1)
        status(NULL, "drag: a prevented dragstart was not seen");
    jsdom_drag_end(d);

    /* buttons: none while the pointer only moves, the left one between pointerdown and pointerup. */
    const char *setup4 = "var buttons = []; zone.addEventListener('mousemove', function (e) { buttons.push(e.buttons); });";
    jsdom_run_script(d, NULL, setup4, (int)strlen(setup4), "buttons setup");
    jsdom_mouse_event(d, zone, "mousemove", 1, 1, -1, NULL);
    jsdom_mouse_event(d, zone, "pointerdown", 1, 1, 0, NULL);
    jsdom_mouse_event(d, zone, "mousemove", 2, 2, -1, NULL);
    jsdom_mouse_event(d, zone, "pointerup", 2, 2, 0, NULL);
    jsdom_mouse_event(d, zone, "mousemove", 3, 3, -1, NULL);
    const char *rest =
        "check(buttons.join() === '0,1,0', 'buttons ' + buttons.join());"
        "check(a.draggable === true && zone.draggable === false && src.draggable === true, 'draggable defaults');"
        "zone.draggable = true; check(zone.getAttribute('draggable') === 'true' && zone.draggable, 'draggable set');"
        "var dt = new DataTransfer(); dt.items.add('x', 'Text/Plain'); dt.setData('text/html', '<b>');"
        "check(dt.types.join() === 'text/plain,text/html' && dt.items[1].type === 'text/html', 'a DataTransfer of its own');"
        "dt.clearData('text/plain'); check(dt.items.length === 1 && dt.items[0].type === 'text/html' && !dt.items[1], 'clearData');"
        "var ev = new DragEvent('drop', { dataTransfer: dt, bubbles: true });"
        "check(ev instanceof DragEvent && ev instanceof MouseEvent && ev.dataTransfer === dt && ev.bubbles, 'DragEvent');"
        "var got = null; zone.ondrop = function (e) { got = e.dataTransfer.getData('text/html'); }; zone.dispatchEvent(ev);"
        "check(got === '<b>', 'ondrop');";
    jsdom_run_script(d, NULL, rest, (int)strlen(rest), "drag extras");
    jsdom_free(d);
    arena_free(&a);
    if (!errors)
        puts("browser drag and drop: DataTransfer modes, effects, link drags, refused drags, buttons and DragEvent passed");
    return errors ? 1 : 0;
}
