// Speech-to-text and cleanup through an OpenAI-compatible service: {base}/audio/transcriptions,
// {base}/chat/completions and {base}/models. Ported from main/services/. Failure messages never
// repeat what the service answered: providers echo keys and dictated text there, and users attach
// the log to bug reports. Each call waits for the network; call it without the loop's lock.
#include "remote.h"
#include <SDL3/SDL.h>
#include <string.h>
#include "cleanup.h"
#include "gs_http.h"
#include "settings.h"
#include "text.h"

#define HTTP_TIMEOUT_MS 15000  // for the model list; the engines have their own, far longer

// The route's URL under the base, or false with `error` set for a base that is no http(s) URL.
static bool service_url(const char *base_url, const char *route, char *url, size_t url_size, char *error, size_t error_size) {
    char *base = eh_trim(base_url ? base_url : "");
    bool ok = false;
    const char *scheme_end = SDL_strstr(base, "://");
    if (!*base) {
        SDL_strlcpy(error, "Base URL is required", error_size);
    } else if (!scheme_end || scheme_end == base || !scheme_end[3] || scheme_end[3] == '/') {
        size_t n = strcspn(base, "?#");  // a query can hold a key
        SDL_snprintf(error, error_size, "Invalid base URL: %.*s", (int)n, base);
    } else if (SDL_strncasecmp(base, "http://", 7) && SDL_strncasecmp(base, "https://", 8)) {
        SDL_snprintf(error, error_size, "Base URL must use http or https, got %.*s:", (int)(scheme_end - base), base);
    } else {
        size_t n = SDL_strlen(base);
        while (n && base[n - 1] == '/') n--;
        SDL_snprintf(url, url_size, "%.*s%s", (int)n, base, route);
        ok = true;
    }
    SDL_free(base);
    return ok;
}

typedef struct { eh_signal *signal; } watch;

static bool keep_going(void *user, size_t received, long long total) { return !eh_signal_aborted(((watch *)user)->signal); }

// One request, its JSON answer parsed. NULL with `error` set, in words that name the service and
// never its reply.
static eh_jv *request_json(gs_http_request *r, const char *service, int timeout_ms, eh_signal *signal, char *error, size_t error_size) {
    watch w = { signal };
    r->timeout_ms = timeout_ms;
    r->follow = true;
    r->progress = keep_going;
    r->progress_user = &w;
    char *body = NULL;
    size_t len = 0;
    Uint64 t0 = SDL_GetTicks();
    int status = gs_http_fetch(r, &body, &len);
    eh_jv *json = NULL;
    if (eh_signal_aborted(signal)) {
        SDL_strlcpy(error, "aborted", error_size);
    } else if (!status) {
        // No answer at all: out of time, or nobody there.
        const char *host = SDL_strstr(r->url, "://");
        host = host ? host + 3 : r->url;
        if (SDL_GetTicks() - t0 >= (Uint64)timeout_ms) SDL_snprintf(error, error_size, "%s didn't answer within %g s", service, timeout_ms / 1000.0);
        else SDL_snprintf(error, error_size, "Couldn't reach %.*s", (int)strcspn(host, "/?#"), host);
    } else if (status < 200 || status >= 300) {
        SDL_snprintf(error, error_size, "%s error %d%s", service, status, status == 401 || status == 403 ? " — check the API key in Settings" : "");
    } else if (!(json = body ? eh_jv_parse(body, len) : NULL)) {
        SDL_snprintf(error, error_size, "%s returned a response that isn't JSON", service);
    }
    SDL_free(body);
    return json;
}

static const char **auth_headers(const char *api_key, const char *extra, char *line, size_t line_size, const char *list[3]) {
    int n = 0;
    if (api_key && *api_key) {
        SDL_snprintf(line, line_size, "Authorization: Bearer %s", api_key);
        list[n++] = line;
    }
    if (extra) list[n++] = extra;
    list[n] = NULL;
    return list;
}

char *eh_remote_transcribe(const uint8_t *wav, size_t len, const eh_jv *stt, eh_signal *signal, char *error, size_t error_size) {
    char url[1200], auth[600], type[120];
    if (!service_url(eh_jv_string(eh_jv_get(stt, "baseUrl"), ""), "/audio/transcriptions", url, sizeof url, error, error_size)) return NULL;
    // The form: the WAV as a file, the model and language when set, and the reply wanted as JSON.
    char boundary[48];
    SDL_snprintf(boundary, sizeof boundary, "----earheart%016llx", (unsigned long long)SDL_GetTicksNS());
    const char *model = eh_jv_string(eh_jv_get(stt, "model"), ""), *language = eh_jv_string(eh_jv_get(stt, "language"), "");
    size_t cap = len + 2048, n = 0;
    char *body = SDL_malloc(cap);
#define FIELD(name, value) n += (size_t)SDL_snprintf(body + n, cap - n, "--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n", boundary, name, value)
    n += (size_t)SDL_snprintf(body + n, cap - n, "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\nContent-Type: audio/wav\r\n\r\n", boundary);
    SDL_memcpy(body + n, wav, len);
    n += len;
    n += (size_t)SDL_snprintf(body + n, cap - n, "\r\n");
    if (*model) FIELD("model", model);
    if (*language) FIELD("language", language);
    FIELD("response_format", "json");
#undef FIELD
    n += (size_t)SDL_snprintf(body + n, cap - n, "--%s--\r\n", boundary);
    SDL_snprintf(type, sizeof type, "Content-Type: multipart/form-data; boundary=%s", boundary);
    const char *headers[3];
    gs_http_request r = { .url = url, .method = "POST", .body = body, .body_len = n,
                          .headers = auth_headers(eh_jv_string(eh_jv_get(stt, "apiKey"), ""), type, auth, sizeof auth, headers) };
    double timeout = eh_jv_number(eh_jv_get(stt, "timeoutMs"), 0);
    eh_jv *data = request_json(&r, "STT service", timeout > 0 ? (int)timeout : 120000, signal, error, error_size);
    SDL_free(body);
    if (!data) return NULL;
    const char *text = eh_jv_string(eh_jv_get(data, "text"), NULL);
    char *out = text ? eh_trim(text) : NULL;
    if (!text) SDL_strlcpy(error, "STT service returned no `text` field", error_size);
    eh_jv_free(data);
    return out;
}

// A reasoning model's thoughts are not the cleaned text: every <think> block goes, with a thought
// left open to the end and a stray closing tag with everything before it.
char *eh_strip_thinking(const char *text) {
    char *s = SDL_strdup(text);
    for (char *open; (open = SDL_strcasestr(s, "<think>"));) {
        char *close = SDL_strcasestr(open + 7, "</think>");
        if (!close) { *open = 0; break; }
        SDL_memmove(open, close + 8, SDL_strlen(close + 8) + 1);
    }
    // The stray closing tags that are left: everything up to the last of them goes.
    char *last = NULL;
    for (char *close = s; (close = SDL_strcasestr(close, "</think>")); close += 8) last = close;
    char *out = eh_trim(last ? last + 8 : s);
    SDL_free(s);
    return out;
}

char *eh_remote_clean(const char *transcript, const eh_jv *cleanup, eh_signal *signal, char *error, size_t error_size) {
    char url[1200], auth[600];
    if (!service_url(eh_jv_string(eh_jv_get(cleanup, "baseUrl"), ""), "/chat/completions", url, sizeof url, error, error_size)) return NULL;
    eh_cleanup_plan plan = eh_resolve_cleanup(cleanup);
    // Only the settings every server takes go on the wire: top-k and min-p make strict servers
    // refuse the request, and a top-p of 1 says nothing.
    eh_jv *req = eh_jv_object(), *messages = eh_jv_array(), *system = eh_jv_object(), *user = eh_jv_object();
    eh_jv_set(req, "model", eh_jv_str(eh_jv_string(eh_jv_get(cleanup, "model"), "")));
    if (plan.has_temperature) eh_jv_set(req, "temperature", eh_jv_num(plan.sampling.temperature));
    if (plan.has_top_p && plan.sampling.top_p < 1) eh_jv_set(req, "top_p", eh_jv_num(plan.sampling.top_p));
    eh_jv_set(system, "role", eh_jv_str("system"));
    eh_jv_set(system, "content", eh_jv_str(plan.system_prompt));
    eh_jv_set(user, "role", eh_jv_str("user"));
    eh_jv_set(user, "content", eh_jv_str(transcript));
    eh_jv_push(messages, system);
    eh_jv_push(messages, user);
    eh_jv_set(req, "messages", messages);
    char *body = eh_jv_write(req, false);
    eh_jv_free(req);
    SDL_free(plan.system_prompt);
    const char *headers[3];
    gs_http_request r = { .url = url, .method = "POST", .body = body,
                          .headers = auth_headers(eh_jv_string(eh_jv_get(cleanup, "apiKey"), ""), "Content-Type: application/json", auth, sizeof auth, headers) };
    double timeout = eh_jv_number(eh_jv_get(cleanup, "timeoutMs"), 0);
    eh_jv *data = request_json(&r, "Cleanup service", timeout > 0 ? (int)timeout : 120000, signal, error, error_size);
    SDL_free(body);
    if (!data) return NULL;
    const eh_jv *choice = eh_jv_at(eh_jv_get(data, "choices"), 0);
    const char *content = eh_jv_string(eh_jv_path(choice, "message.content"), NULL);
    char *out = NULL;
    if (!SDL_strcmp(eh_jv_string(eh_jv_get(choice, "finish_reason"), ""), "length")) {
        SDL_strlcpy(error, "Cleanup reached its output token limit before finishing", error_size);
    } else if (!content) {
        SDL_strlcpy(error, "Cleanup service returned no message content", error_size);
    } else {
        out = eh_strip_thinking(content);
        if (!*out) {
            SDL_free(out);
            out = NULL;
            SDL_strlcpy(error, "Cleanup returned no usable text", error_size);
        }
    }
    eh_jv_free(data);
    return out;
}

static int compare_ids(const void *a, const void *b) {
    const char *x = (*(eh_jv *const *)a)->string, *y = (*(eh_jv *const *)b)->string;
    int c = SDL_strcasecmp(x, y);
    return c ? c : SDL_strcmp(x, y);
}

eh_jv *eh_remote_models(const eh_jv *cfg, eh_signal *signal, char *error, size_t error_size) {
    char url[1200], auth[600];
    if (!service_url(eh_jv_string(eh_jv_get(cfg, "baseUrl"), ""), "/models", url, sizeof url, error, error_size)) return NULL;
    const char *headers[3];
    gs_http_request r = { .url = url, .headers = auth_headers(eh_jv_string(eh_jv_get(cfg, "apiKey"), ""), NULL, auth, sizeof auth, headers) };
    eh_jv *body = request_json(&r, "Model list service", HTTP_TIMEOUT_MS, signal, error, error_size);
    if (!body) return NULL;
    // A list of ids, or of objects with an id, bare or under "data".
    const eh_jv *list = body->type == EH_JV_ARRAY ? body : eh_jv_get(body, "data");
    if (!list || list->type != EH_JV_ARRAY) {
        SDL_strlcpy(error, "Model list service returned an unexpected /models reply", error_size);
        eh_jv_free(body);
        return NULL;
    }
    eh_jv *ids = eh_jv_array();
    for (int i = 0; i < list->count; i++) {
        const eh_jv *m = list->items[i];
        const char *id = m->type == EH_JV_STRING ? m->string : eh_jv_string(eh_jv_get(m, "id"), NULL);
        bool known = !id || !*id;
        for (int k = 0; !known && k < ids->count; k++) known = !SDL_strcmp(ids->items[k]->string, id);
        if (!known) eh_jv_push(ids, eh_jv_str(id));
    }
    SDL_qsort(ids->items, (size_t)ids->count, sizeof *ids->items, compare_ids);
    eh_jv_free(body);
    return ids;
}
