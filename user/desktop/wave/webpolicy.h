#ifndef WEBPOLICY_H
#define WEBPOLICY_H
#include <stddef.h>
int web_origin(const char *url, char *out, size_t size);
int web_same_origin(const char *a, const char *b);
int web_same_site(const char *a, const char *b);
int web_public_suffix(const char *domain);
int web_header_value(const char *headers, const char *name, char *out, size_t size);
int web_forbidden_header(const char *name);
int web_simple_request(const char *method, const char *headers);
int web_cors_allow(const char *origin, const char *headers, int credentials);
int web_cors_preflight(const char *origin, const char *response, int credentials, const char *method,
                       const char *headers);
int web_response_headers(const char *headers, int cross, int credentials, char *out, size_t size);
#endif
