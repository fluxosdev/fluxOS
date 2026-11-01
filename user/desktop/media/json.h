/*
 * A small JSON reader (json.c), enough for the answers of web APIs: the whole text becomes a tree
 * of values, which the functions below look things up in. Strings are unescaped to UTF-8.
 */
#ifndef MEDIA_JSON_H
#define MEDIA_JSON_H

#include <stddef.h>

enum { JSON_NULL, JSON_FALSE, JSON_TRUE, JSON_NUM, JSON_STR, JSON_ARR, JSON_OBJ };

struct json {
    int type;
    double num;
    char *str;                  /* JSON_STR: the text; in an object, every member has key */
    char *key;
    struct json *child;         /* JSON_ARR and JSON_OBJ: the first element or member */
    struct json *next;          /* the next one after this, in its array or object */
};

/* The value text holds, or NULL if it is not JSON. */
struct json *json_parse(const char *text, size_t len);
void json_free(struct json *v);
/* An object's member, or NULL (also when v is not an object). */
const struct json *json_get(const struct json *v, const char *key);
/* Members down a path of keys separated by dots: "streamingData.formats". */
const struct json *json_path(const struct json *v, const char *path);
/* A member's string, or def; its number, or def. */
const char *json_str(const struct json *v, const char *key, const char *def);
double json_num(const struct json *v, const char *key, double def);

#endif
