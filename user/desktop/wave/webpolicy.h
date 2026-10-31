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
/* Referrer Policy: a policy from a <meta name=referrer> content or a Referrer-Policy header (the last
 * one known in its list; RP_DEFAULT if none is), and the Referer a request from document from to to
 * carries under it (0: none). */
enum { RP_DEFAULT, RP_NO_REFERRER, RP_NO_REFERRER_WHEN_DOWNGRADE, RP_SAME_ORIGIN, RP_ORIGIN, RP_STRICT_ORIGIN,
       RP_ORIGIN_WHEN_CROSS_ORIGIN, RP_STRICT_ORIGIN_WHEN_CROSS_ORIGIN, RP_UNSAFE_URL };
int web_referrer_policy(const char *value);
int web_referrer(const char *from, const char *to, int policy, char *out, size_t size);
/* Sec-Fetch-Site: how the initiator's origin stands to to's ("none": the user asked, there is no initiator). */
const char *web_fetch_site(const char *initiator, const char *to);
/* Enforced source-list subset: policies are intersected across newline-separated headers/metas.
 * target NULL denotes inline code; the special directive "eval" checks unsafe-eval. */
int web_csp_allows(const char *policies,const char *document,const char *directive,const char *target,const char *nonce);
/* The Content-Security-Policy headers, newline-separated, in a malloc'd string (NULL: none). */
char *web_csp_headers(const char *headers);
enum { WEB_SB_SCRIPTS=1,WEB_SB_FORMS=2,WEB_SB_ORIGIN=4,WEB_SB_POPUPS=8,WEB_SB_TOP=16,WEB_SB_ESCAPE=32 };
unsigned web_sandbox_flags(const char *attribute);

#endif
