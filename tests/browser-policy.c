#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "webpolicy.h"
#include "web.h"
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x)) {                                                                                          \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                          \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)
unsigned int uptime_ms(void)
{
    return 10000;
}
int main(void)
{
    char out[4096];
    CHECK(web_same_origin("HTTPS://EXAMPLE.COM:443/a", "https://example.com/b"));
    CHECK(!web_same_origin("https://example.com", "http://example.com"));
    CHECK(!web_same_origin("https://example.com:444", "https://example.com"));
    CHECK(!web_origin("https://host:99999999999999/", out, sizeof(out)));
    CHECK(web_same_site("https://a.example.co.uk", "https://b.example.co.uk"));
    CHECK(!web_same_site("https://a.github.io", "https://b.github.io"));
    CHECK(!web_same_site("http://example.com", "https://example.com"));
    CHECK(web_public_suffix("co.uk") && web_public_suffix("github.io") &&
          !web_public_suffix("example.co.uk"));
    CHECK(web_public_suffix("a.ck") && !web_public_suffix("www.ck"));
    CHECK(web_public_suffix("co.uk."));
    CHECK(!web_same_site("https://a.co.uk./","https://b.co.uk./"));
    CHECK(!web_same_site("https://127.1.2.3/","https://128.1.2.3/"));
    CHECK(!web_same_site("https://[::1]/","https://[::2]/"));
    CHECK(web_cors_allow("https://app.example", "Access-Control-Allow-Origin: *\r\n", 0));
    CHECK(!web_cors_allow("https://app.example", "Access-Control-Allow-Origin: *\r\n", 1));
    CHECK(web_cors_allow(
        "https://app.example",
        "Access-Control-Allow-Origin: https://app.example\r\nAccess-Control-Allow-Credentials: true\r\n", 1));
    CHECK(!web_cors_allow(
        "https://app.example",
        "Access-Control-Allow-Origin: https://app.example\r\nAccess-Control-Allow-Origin: *\r\n", 0));
    CHECK(web_simple_request("POST", "Content-Type: text/plain;charset=UTF-8\n"));
    CHECK(!web_simple_request("POST", "Content-Type: application/json\n"));
    CHECK(!web_simple_request("PUT", ""));
    CHECK(web_cors_preflight("https://app.example",
                             "Access-Control-Allow-Origin: *\nAccess-Control-Allow-Methods: "
                             "PUT\nAccess-Control-Allow-Headers: x-test\n",
                             0, "PUT", "X-Test: yes\n"));
    CHECK(!web_cors_preflight("https://app.example",
                              "Access-Control-Allow-Origin: *\nAccess-Control-Allow-Methods: "
                              "PUT\nAccess-Control-Allow-Headers: *\n",
                              0, "PUT", "Authorization: token\n"));
    CHECK(!web_cors_allow("https://app.example", "HTTP/1.1 200 OK\r\n\r\nAccess-Control-Allow-Origin: *\r\n",
                          0));
    CHECK(web_response_headers("Content-Type: text/plain\r\n\r\nX-Body: not-a-header\n", 0, 0, out,
                               sizeof(out)) > 0);
    CHECK(!strstr(out, "X-Body"));
    CHECK(web_forbidden_header("Cookie") && web_forbidden_header("Sec-Fetch-Site") &&
          !web_forbidden_header("Authorization"));
    cookie_set("https://api.example.com/", "lax=1; Path=/", 0);
    cookie_set("https://api.example.com/", "strict=1; SameSite=Strict; Path=/", 0);
    cookie_set("https://api.example.com/", "none=1; SameSite=None; Secure; Path=/", 0);
    cookie_set("https://api.example.com/", "bad=1; SameSite=None; Path=/", 0);
    cookie_header_context("https://api.example.com/", "https://foreign.test/", 0, 1, out, sizeof(out));
    CHECK(!strcmp(out, "none=1"));
    cookie_header_context("https://api.example.com/", "https://foreign.test/", 1, 1, out, sizeof(out));
    CHECK(strstr(out, "lax=1") && !strstr(out, "strict=1") && !strstr(out, "bad=1"));
    cookie_header_context("https://api.example.com/", "https://www.example.com/", 0, 0, out, sizeof(out));
    CHECK(strstr(out, "strict=1"));
    cookie_set("https://a.github.io/", "psl=1; Domain=github.io; Path=/", 0);
    cookie_header("https://b.github.io/", 0, out, sizeof(out));
    CHECK(!out[0]);
    cookie_set("https://api.example.com/", "__Host-bad=1; Secure; Domain=example.com; Path=/", 0);
    cookie_set("https://api.example.com/", "__Host-good=1; Secure; Path=/", 0);
    cookie_header("https://api.example.com/", 0, out, sizeof(out));
    CHECK(!strstr(out, "__Host-bad") && strstr(out, "__Host-good"));
    cookie_set("https://api.example.com/", "hidden=1; HttpOnly; Path=/", 0);
    cookie_set("https://api.example.com/", "hidden=2; Path=/", 1);
    cookie_header("https://api.example.com/", 1, out, sizeof(out));
    CHECK(!strstr(out, "hidden="));
    cookie_header("https://api.example.com/", 0, out, sizeof(out));
    CHECK(strstr(out, "hidden=1"));
    /* Referrer Policy: strict-origin-when-cross-origin unless the page says otherwise */
    char ref[512];
    const char *page = "https://user:pw@shop.example.com/cart?id=7#total";
    CHECK(web_referrer(page, "https://shop.example.com/pay", RP_DEFAULT, ref, sizeof(ref)) &&
          !strcmp(ref, "https://shop.example.com/cart?id=7"));
    CHECK(web_referrer(page, "https://cdn.other.net/a.js", RP_DEFAULT, ref, sizeof(ref)) &&
          !strcmp(ref, "https://shop.example.com/"));
    CHECK(!web_referrer(page, "http://shop.example.com/", RP_DEFAULT, ref, sizeof(ref)) && !ref[0]);
    CHECK(web_referrer("https://a.example?q=1", "https://a.example/x", RP_DEFAULT, ref, sizeof(ref)) &&
          !strcmp(ref, "https://a.example/?q=1"));
    CHECK(!web_referrer(page, "https://shop.example.com/", RP_NO_REFERRER, ref, sizeof(ref)));
    CHECK(!web_referrer(page, "https://x.org/", RP_SAME_ORIGIN, ref, sizeof(ref)));
    CHECK(web_referrer(page, "http://x.org/", RP_ORIGIN, ref, sizeof(ref)) && !strcmp(ref, "https://shop.example.com/"));
    CHECK(!web_referrer(page, "http://x.org/", RP_STRICT_ORIGIN, ref, sizeof(ref)));
    CHECK(web_referrer(page, "http://x.org/", RP_UNSAFE_URL, ref, sizeof(ref)) &&
          !strcmp(ref, "https://shop.example.com/cart?id=7"));
    CHECK(web_referrer(page, "https://x.org/", RP_NO_REFERRER_WHEN_DOWNGRADE, ref, sizeof(ref)) &&
          !strcmp(ref, "https://shop.example.com/cart?id=7"));
    CHECK(!web_referrer("file:///home/flux/a.html", "https://x.org/", RP_UNSAFE_URL, ref, sizeof(ref)));
    CHECK(!web_referrer("", "https://x.org/", RP_DEFAULT, ref, sizeof(ref)));
    CHECK(web_referrer_policy("no-referrer") == RP_NO_REFERRER && web_referrer_policy("bogus") == RP_DEFAULT);
    CHECK(web_referrer_policy("no-referrer, strict-origin-when-cross-origin") == RP_STRICT_ORIGIN_WHEN_CROSS_ORIGIN);
    CHECK(web_referrer_policy(" Origin , bogus") == RP_ORIGIN && web_referrer_policy("never") == RP_NO_REFERRER);
    /* Sec-Fetch-Site */
    CHECK(!strcmp(web_fetch_site("", "https://a.example.com/"), "none"));
    CHECK(!strcmp(web_fetch_site("https://a.example.com/x", "https://a.example.com/y"), "same-origin"));
    CHECK(!strcmp(web_fetch_site("https://a.example.com/x", "https://img.example.com/y"), "same-site"));
    CHECK(!strcmp(web_fetch_site("https://a.example.com/x", "https://example.org/y"), "cross-site"));
    CHECK(!strcmp(web_fetch_site("https://a.github.io/", "https://b.github.io/"), "cross-site"));
    puts("browser policy: origins, public suffixes, CORS, preflights, SameSite cookies, referrers and fetch metadata passed");
    return 0;
}
