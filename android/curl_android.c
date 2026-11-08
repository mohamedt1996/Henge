/* Every curl handle in the process (ours, libtatsu's, idevicerestore's) is made
   through here, via the linker's --wrap=curl_easy_init. The engine is a static
   glibc binary, and on Android glibc's resolver has no /etc/resolv.conf to read
   and its CA default is not Android's, so each handle gets:
     - CAs: CURL_CA_BUNDLE, Termux's bundle, a desktop bundle, or Android's
       /system/etc/security/cacerts directory;
     - DNS: over HTTPS to an IP-literal resolver (no name lookup to bootstrap)
       when the system resolver can't work, or HV_DOH names one ("off" disables). */
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

CURL *__real_curl_easy_init(void);

static const char *ca_bundle(void) {
    const char *b = getenv("CURL_CA_BUNDLE");
    if (b && *b) return b;
    if (access("/data/data/com.termux/files/usr/etc/tls/cert.pem", R_OK) == 0) return "/data/data/com.termux/files/usr/etc/tls/cert.pem";
    if (access("/etc/ssl/certs/ca-certificates.crt", R_OK) == 0) return "/etc/ssl/certs/ca-certificates.crt";
    return NULL;
}

static const char *doh_url(void) {
    const char *d = getenv("HV_DOH");
    if (d && !strcmp(d, "off")) return NULL;
    if (d && *d) return d;
    if (access("/etc/resolv.conf", R_OK) == 0) return NULL; /* desktop Linux: the system resolver works */
    return "https://1.1.1.1/dns-query";
}

CURL *__wrap_curl_easy_init(void) {
    CURL *c = __real_curl_easy_init();
    if (!c) return c;
    const char *b = ca_bundle();
    if (b) {
        curl_easy_setopt(c, CURLOPT_CAINFO, b);
        curl_easy_setopt(c, CURLOPT_CAPATH, NULL);
    } else {
        curl_easy_setopt(c, CURLOPT_CAPATH, "/system/etc/security/cacerts");
    }
    const char *doh = doh_url();
    if (doh) curl_easy_setopt(c, CURLOPT_DOH_URL, doh);
    return c;
}
