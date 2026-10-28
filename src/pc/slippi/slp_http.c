/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Small HTTP GET/POST for the Slippi account refresh and login flow
 * (slp_user.h), the XeleeNet lobby client (xeleenet.c), and pc_http_post for
 * the launcher's log upload. Built on libcurl where the platform has it
 * (desktop, and the same dependency the updater already uses), java.net on
 * Android and WinHTTP on Windows; on a platform with none of them the calls
 * report failure, which the account refresh treats as "keep the stored file".
 * This is not on the per-frame path. */
#include "pc/slippi/slp_user.h"

#include <stdlib.h>
#include <string.h>

#if defined(MELEE_USE_CURL) && defined(_WIN32)
#include <windows.h>
#endif

#if defined(MELEE_USE_CURL)
#include <curl/curl.h>

typedef struct Sink {
    char* out;
    size_t cap;
    size_t len;
    bool overflow;
} Sink;

static size_t write_cb(char* data, size_t size, size_t nmemb, void* user) {
    Sink* s = (Sink*)user;
    size_t n = size * nmemb;
    if (s->len + n >= s->cap) {
        /* Copy what fits (leaving room for the NUL) and flag the overflow; the
         * caller sees a truncated body rather than a crash. */
        size_t room = s->cap > s->len + 1 ? s->cap - s->len - 1 : 0;
        if (room > 0) {
            memcpy(s->out + s->len, data, room);
            s->len += room;
        }
        s->out[s->len] = '\0';
        s->overflow = true;
        return n; /* claim it was consumed so curl does not abort */
    }
    memcpy(s->out + s->len, data, n);
    s->len += n;
    s->out[s->len] = '\0';
    return n;
}

static bool ensure_global(void) {
    static bool done;
    static bool ok;
    if (!done) {
        done = true;
        ok = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    }
    return ok;
}

static bool http_request(const char* url, const char* body, size_t body_len, const char* content_type,
                         const char* bearer, bool post, long timeout_s, char* out, size_t cap, int* status) {
    if (out != NULL && cap > 0) {
        out[0] = '\0';
    }
    if (status != NULL) {
        *status = 0;
    }
    if (url == NULL || out == NULL || cap == 0 || !ensure_global()) {
        return false;
    }
    CURL* curl = curl_easy_init();
    if (curl == NULL) {
        return false;
    }
    Sink sink = {out, cap, 0, false};
    struct curl_slist* headers = NULL;
    /* A Slippi ID token is a JWT, commonly over 1 KB, so this must not be a
     * small fixed buffer that silently drops the header and fails the login. */
    char* auth = NULL;
    if (bearer != NULL && bearer[0] != '\0') {
        static const char kPrefix[] = "Authorization: Bearer ";
        size_t need = strlen(bearer) + sizeof kPrefix;
        auth = (char*)malloc(need);
        if (auth == NULL) {
            curl_easy_cleanup(curl);
            return false;
        }
        snprintf(auth, need, "%s%s", kPrefix, bearer);
        headers = curl_slist_append(headers, auth);
    }
    char type_header[128];
    if (post) {
        snprintf(type_header, sizeof type_header, "Content-Type: %s", content_type);
        headers = curl_slist_append(headers, type_header);
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "melee-pc Slippi");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (headers != NULL) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }
    if (post && body != NULL) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
    }
    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (status != NULL) {
        *status = (int)code;
    }
    if (headers != NULL) {
        curl_slist_free_all(headers);
    }
    free(auth);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK;
}

bool pc_slippi_http_get(const char* url, const char* bearer, char* out, size_t cap, int* status) {
    return http_request(url, NULL, 0, NULL, bearer, false, 15L, out, cap, status);
}

bool pc_slippi_http_post_json(const char* url, const char* body, const char* bearer, char* out,
                              size_t cap, int* status) {
    return http_request(url, body, body ? strlen(body) : 0, "application/json", bearer, true, 15L, out,
                        cap, status);
}

bool pc_http_post(const char* url, const char* content_type, const char* body, size_t len, char* out,
                  size_t cap, int* status) {
    /* A few MB of logs over a slow uplink takes longer than an account call. */
    return http_request(url, body, len, content_type, NULL, true, 120L, out, cap, status);
}

#elif defined(__ANDROID__) /* no libcurl: java.net over JNI (slp_http_android.cpp) */

#include <stdbool.h>
bool pc_slippi_http_android(const char* url, const char* body, const char* bearer, bool post, char* out,
                            size_t cap, int* status);
bool pc_http_android(const char* url, const char* body, size_t body_len, const char* content_type,
                     const char* bearer, bool post, int read_timeout_ms, char* out, size_t cap, int* status);

bool pc_slippi_http_get(const char* url, const char* bearer, char* out, size_t cap, int* status) {
    if (status != NULL) {
        *status = 0;
    }
    return pc_slippi_http_android(url, NULL, bearer, false, out, cap, status);
}

bool pc_slippi_http_post_json(const char* url, const char* body, const char* bearer, char* out,
                              size_t cap, int* status) {
    if (status != NULL) {
        *status = 0;
    }
    return pc_slippi_http_android(url, body, bearer, true, out, cap, status);
}

bool pc_http_post(const char* url, const char* content_type, const char* body, size_t len, char* out,
                  size_t cap, int* status) {
    if (status != NULL) {
        *status = 0;
    }
    return pc_http_android(url, body, len, content_type, NULL, true, 120000, out, cap, status);
}

#elif defined(_WIN32)
/* Windows builds have no libcurl, only WinHTTP (as the updater uses), so the
 * account calls, XeleeNet and the log upload share one request here. */
#include <stdio.h>
#include <wchar.h>
#include <windows.h>
#include <winhttp.h>

static bool winhttp_request(const wchar_t* method, const char* url, const char* body, size_t body_len,
                            const char* content_type, const char* bearer, DWORD timeout_ms, char* out, size_t cap,
                            int* status) {
    if (out != NULL && cap > 0) {
        out[0] = '\0';
    }
    if (status != NULL) {
        *status = 0;
    }
    if (url == NULL || out == NULL || cap == 0) {
        return false;
    }
    wchar_t wurl[1024];
    if (MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 1024) == 0) {
        return false;
    }
    URL_COMPONENTS parts;
    wchar_t host[256], path[1024], extra[1024];
    ZeroMemory(&parts, sizeof parts);
    parts.dwStructSize = sizeof parts;
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 1024;
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = 1024;
    if (!WinHttpCrackUrl(wurl, 0, 0, &parts)) {
        return false;
    }
    /* The request object is path plus query (GET /v1/lobbies?proto=...); both
     * came out of wurl, so together they fit its size. */
    wchar_t object[1024];
    wcscpy(object, path[0] != L'\0' ? path : L"/");
    if (wcslen(object) + wcslen(extra) >= 1024) {
        return false;
    }
    wcscat(object, extra);
    /* A Slippi ID token is a JWT, commonly over 1 KB, so the headers are sized
     * to it rather than a fixed buffer. ASCII in, so one wchar per byte. */
    size_t need = 64 + (content_type ? strlen(content_type) : 0) + (bearer ? strlen(bearer) : 0);
    char* header = (char*)malloc(need);
    wchar_t* wheader = (wchar_t*)malloc(need * sizeof(wchar_t));
    if (header == NULL || wheader == NULL) {
        free(header);
        free(wheader);
        return false;
    }
    size_t at = 0;
    header[0] = '\0';
    if (content_type != NULL) {
        at += (size_t)snprintf(header + at, need - at, "Content-Type: %s\r\n", content_type);
    }
    if (bearer != NULL && bearer[0] != '\0') {
        snprintf(header + at, need - at, "Authorization: Bearer %s\r\n", bearer);
    }
    if (MultiByteToWideChar(CP_UTF8, 0, header, -1, wheader, (int)need) == 0) {
        free(header);
        free(wheader);
        return false;
    }
    bool ok = false;
    HINTERNET session = WinHttpOpen(L"Xelee", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET conn = session ? WinHttpConnect(session, host, parts.nPort, 0) : NULL;
    HINTERNET req = conn ? WinHttpOpenRequest(conn, method, object, NULL, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                         : NULL;
    if (req != NULL) {
        WinHttpSetTimeouts(req, 10000, 10000, (int)timeout_ms, (int)timeout_ms);
    }
    if (req != NULL &&
        WinHttpSendRequest(req, wheader[0] != L'\0' ? wheader : WINHTTP_NO_ADDITIONAL_HEADERS,
                           wheader[0] != L'\0' ? (DWORD)-1L : 0, (LPVOID)body, (DWORD)body_len, (DWORD)body_len,
                           0) &&
        WinHttpReceiveResponse(req, NULL)) {
        /* Any status counts as an answer: callers read 4xx refusal bodies. */
        DWORD code = 0, size = sizeof code;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &code, &size, WINHTTP_NO_HEADER_INDEX);
        if (status != NULL) {
            *status = (int)code;
        }
        size_t got = 0;
        DWORD n = 0;
        while (got + 1 < cap && WinHttpReadData(req, out + got, (DWORD)(cap - 1 - got), &n) && n > 0) {
            got += n;
        }
        out[got] = '\0';
        ok = true;
    }
    if (req != NULL) {
        WinHttpCloseHandle(req);
    }
    if (conn != NULL) {
        WinHttpCloseHandle(conn);
    }
    if (session != NULL) {
        WinHttpCloseHandle(session);
    }
    free(header);
    free(wheader);
    return ok;
}

bool pc_slippi_http_get(const char* url, const char* bearer, char* out, size_t cap, int* status) {
    return winhttp_request(L"GET", url, NULL, 0, NULL, bearer, 15000, out, cap, status);
}

bool pc_slippi_http_post_json(const char* url, const char* body, const char* bearer, char* out,
                              size_t cap, int* status) {
    return winhttp_request(L"POST", url, body, body ? strlen(body) : 0, "application/json", bearer, 15000, out,
                           cap, status);
}

bool pc_http_post(const char* url, const char* content_type, const char* body, size_t len, char* out,
                  size_t cap, int* status) {
    /* A few MB of logs over a slow uplink takes longer than an account call. */
    return winhttp_request(L"POST", url, body, len, content_type, NULL, 120000, out, cap, status);
}

#else /* no HTTP stack: the refresh is unavailable, everything else still works */

bool pc_slippi_http_get(const char* url, const char* bearer, char* out, size_t cap, int* status) {
    (void)url;
    (void)bearer;
    if (out != NULL && cap > 0) {
        out[0] = '\0';
    }
    if (status != NULL) {
        *status = 0;
    }
    return false;
}

bool pc_slippi_http_post_json(const char* url, const char* body, const char* bearer, char* out,
                              size_t cap, int* status) {
    (void)url;
    (void)body;
    (void)bearer;
    if (out != NULL && cap > 0) {
        out[0] = '\0';
    }
    if (status != NULL) {
        *status = 0;
    }
    return false;
}

bool pc_http_post(const char* url, const char* content_type, const char* body, size_t len, char* out,
                  size_t cap, int* status) {
    (void)url;
    (void)content_type;
    (void)body;
    (void)len;
    if (out != NULL && cap > 0) {
        out[0] = '\0';
    }
    if (status != NULL) {
        *status = 0;
    }
    return false;
}

#endif
