// SPDX-License-Identifier: Apache-2.0
// The HTTP route matcher, URL decoding, the response envelope and ETags.
#include "http_envelope.h"
#include "http_route.h"
#include "p3a_err.h"
#include "unity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void h_status(struct p3a_http_request *r) { (void)r; }
static void h_settings_get(struct p3a_http_request *r) { (void)r; }
static void h_settings_patch(struct p3a_http_request *r) { (void)r; }
static void h_job(struct p3a_http_request *r) { (void)r; }
static void h_job_latest(struct p3a_http_request *r) { (void)r; }
static void h_pin(struct p3a_http_request *r) { (void)r; }
static void h_static(struct p3a_http_request *r) { (void)r; }
static void h_root(struct p3a_http_request *r) { (void)r; }

static const p3a_http_route_t ROUTES[] = {
    {P3A_HTTP_GET, 0, "/", h_root},
    {P3A_HTTP_GET, 0, "/api/v2/status", h_status},
    {P3A_HTTP_GET, 0, "/api/v2/settings", h_settings_get},
    {P3A_HTTP_PATCH, P3A_HTTP_BODY_JSON, "/api/v2/settings", h_settings_patch},
    {P3A_HTTP_GET, 0, "/api/v2/jobs/latest", h_job_latest},
    {P3A_HTTP_GET, 0, "/api/v2/jobs/{id}", h_job},
    {P3A_HTTP_DELETE, 0, "/api/v2/lists/{list}/items/{item}", h_pin},
    {P3A_HTTP_GET, P3A_HTTP_RAW, "/static/*", h_static},
};
#define N (sizeof(ROUTES) / sizeof(ROUTES[0]))

static const p3a_http_route_t *found;
static p3a_http_param_t params[P3A_HTTP_MAX_PARAMS];
static unsigned allowed;

static p3a_http_match_t match(p3a_http_method_t m, const char *path)
{
    found = (const p3a_http_route_t *)0x1;
    memset(params, 0xAA, sizeof(params));
    allowed = 0xFFFF;
    return p3a_http_match(ROUTES, N, m, path, strlen(path), &found, params, &allowed);
}

#define PARAM_IS(i, text)                                              \
    do {                                                               \
        TEST_ASSERT_EQUAL(strlen(text), params[i].len);                \
        TEST_ASSERT_EQUAL_MEMORY(text, params[i].start, strlen(text)); \
    } while (0)

// --- matching -------------------------------------------------------------------

static void test_literal_routes(void)
{
    TEST_ASSERT_EQUAL(P3A_HTTP_MATCHED, match(P3A_HTTP_GET, "/api/v2/status"));
    TEST_ASSERT_TRUE(found->handler == h_status);
    TEST_ASSERT_NULL(params[0].start);
    TEST_ASSERT_EQUAL(0, params[0].len);
    TEST_ASSERT_EQUAL(P3A_HTTP_MATCHED, match(P3A_HTTP_GET, "/"));
    TEST_ASSERT_TRUE(found->handler == h_root);
}

static void test_method_selects_among_routes_for_one_path(void)
{
    TEST_ASSERT_EQUAL(P3A_HTTP_MATCHED, match(P3A_HTTP_GET, "/api/v2/settings"));
    TEST_ASSERT_TRUE(found->handler == h_settings_get);
    TEST_ASSERT_EQUAL(P3A_HTTP_MATCHED, match(P3A_HTTP_PATCH, "/api/v2/settings"));
    TEST_ASSERT_TRUE(found->handler == h_settings_patch);
    TEST_ASSERT_EQUAL(P3A_HTTP_BODY_JSON, found->flags);
}

static void test_wrong_method_is_told_apart_from_no_such_path(void)
{
    TEST_ASSERT_EQUAL(P3A_HTTP_WRONG_METHOD, match(P3A_HTTP_POST, "/api/v2/settings"));
    TEST_ASSERT_NULL(found);
    TEST_ASSERT_EQUAL_HEX((1u << P3A_HTTP_GET) | (1u << P3A_HTTP_PATCH), allowed);
    TEST_ASSERT_EQUAL(P3A_HTTP_WRONG_METHOD, match(P3A_HTTP_GET, "/api/v2/lists/a/items/b"));
    TEST_ASSERT_EQUAL_HEX(1u << P3A_HTTP_DELETE, allowed);
    TEST_ASSERT_EQUAL(P3A_HTTP_NO_SUCH_PATH, match(P3A_HTTP_GET, "/api/v2/nothing"));
    TEST_ASSERT_NULL(found);
    TEST_ASSERT_EQUAL_HEX(0, allowed);
}

static void test_matching_is_exact(void)
{
    TEST_ASSERT_EQUAL(P3A_HTTP_NO_SUCH_PATH, match(P3A_HTTP_GET, "/api/v2/status/"));
    TEST_ASSERT_EQUAL(P3A_HTTP_NO_SUCH_PATH, match(P3A_HTTP_GET, "/api/v2/statu"));
    TEST_ASSERT_EQUAL(P3A_HTTP_NO_SUCH_PATH, match(P3A_HTTP_GET, "/api/v2/statuses"));
    TEST_ASSERT_EQUAL(P3A_HTTP_NO_SUCH_PATH, match(P3A_HTTP_GET, "/API/v2/status"));
    TEST_ASSERT_EQUAL(P3A_HTTP_NO_SUCH_PATH, match(P3A_HTTP_GET, "api/v2/status"));
    TEST_ASSERT_EQUAL(P3A_HTTP_NO_SUCH_PATH, match(P3A_HTTP_GET, ""));
    TEST_ASSERT_EQUAL(P3A_HTTP_NO_SUCH_PATH, match(P3A_HTTP_GET, "//"));
}

static void test_one_parameter(void)
{
    TEST_ASSERT_EQUAL(P3A_HTTP_MATCHED, match(P3A_HTTP_GET, "/api/v2/jobs/17"));
    TEST_ASSERT_TRUE(found->handler == h_job);
    PARAM_IS(0, "17");
    TEST_ASSERT_NULL(params[1].start);
    // A parameter is one non-empty segment.
    TEST_ASSERT_EQUAL(P3A_HTTP_NO_SUCH_PATH, match(P3A_HTTP_GET, "/api/v2/jobs/"));
    TEST_ASSERT_EQUAL(P3A_HTTP_NO_SUCH_PATH, match(P3A_HTTP_GET, "/api/v2/jobs/17/x"));
    TEST_ASSERT_EQUAL(P3A_HTTP_NO_SUCH_PATH, match(P3A_HTTP_GET, "/api/v2/jobs"));
}

static void test_a_literal_listed_first_wins_over_a_parameter(void)
{
    TEST_ASSERT_EQUAL(P3A_HTTP_MATCHED, match(P3A_HTTP_GET, "/api/v2/jobs/latest"));
    TEST_ASSERT_TRUE(found->handler == h_job_latest);
    TEST_ASSERT_EQUAL(P3A_HTTP_MATCHED, match(P3A_HTTP_GET, "/api/v2/jobs/latest2"));
    TEST_ASSERT_TRUE(found->handler == h_job);
}

static void test_two_parameters_arrive_in_pattern_order(void)
{
    TEST_ASSERT_EQUAL(P3A_HTTP_MATCHED, match(P3A_HTTP_DELETE, "/api/v2/lists/my%20list/items/urn:x-1"));
    PARAM_IS(0, "my%20list");
    PARAM_IS(1, "urn:x-1");
    TEST_ASSERT_EQUAL(P3A_HTTP_NO_SUCH_PATH, match(P3A_HTTP_DELETE, "/api/v2/lists//items/x"));
}

static void test_a_trailing_star_takes_the_rest(void)
{
    TEST_ASSERT_EQUAL(P3A_HTTP_MATCHED, match(P3A_HTTP_GET, "/static/app.js"));
    PARAM_IS(0, "app.js");
    TEST_ASSERT_EQUAL(P3A_HTTP_MATCHED, match(P3A_HTTP_GET, "/static/img/logo.png"));
    PARAM_IS(0, "img/logo.png");
    TEST_ASSERT_EQUAL(P3A_HTTP_NO_SUCH_PATH, match(P3A_HTTP_GET, "/static/"));
    TEST_ASSERT_EQUAL(P3A_HTTP_NO_SUCH_PATH, match(P3A_HTTP_GET, "/static"));
}

static void test_path_length_is_honoured_not_the_terminator(void)
{
    // As the server calls it: the path is the URI up to the '?'.
    const char *uri = "/api/v2/jobs/17?verbose=1";
    TEST_ASSERT_EQUAL(P3A_HTTP_MATCHED,
                      p3a_http_match(ROUTES, N, P3A_HTTP_GET, uri, 15, &found, params, NULL));
    PARAM_IS(0, "17");
    // Given the query too, the matcher takes it for part of the segment: it is
    // the caller's job to cut the URI at the '?'.
    TEST_ASSERT_EQUAL(P3A_HTTP_MATCHED,
                      p3a_http_match(ROUTES, N, P3A_HTTP_GET, uri, strlen(uri), &found, params, NULL));
    PARAM_IS(0, "17?verbose=1");
}

static void test_bad_tables_and_arguments_match_nothing(void)
{
    static const p3a_http_route_t broken[] = {
        {P3A_HTTP_GET, 0, "/a/{unclosed", h_job},
        {P3A_HTTP_GET, 0, NULL, h_job},
        {P3A_HTTP_GET, 0, "/a/{x}/{y}/{z}", h_job},  // more parameters than the matcher holds
    };
    TEST_ASSERT_EQUAL(P3A_HTTP_NO_SUCH_PATH, p3a_http_match(broken, 3, P3A_HTTP_GET, "/a/b", 4, &found, params, NULL));
    TEST_ASSERT_EQUAL(P3A_HTTP_NO_SUCH_PATH,
                      p3a_http_match(broken, 3, P3A_HTTP_GET, "/a/1/2/3", 8, &found, params, NULL));
    TEST_ASSERT_EQUAL(P3A_HTTP_NO_SUCH_PATH, p3a_http_match(NULL, 0, P3A_HTTP_GET, "/", 1, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL(P3A_HTTP_NO_SUCH_PATH, p3a_http_match(ROUTES, N, P3A_HTTP_GET, NULL, 0, NULL, NULL, NULL));
    // Optional outputs may be NULL on a match.
    TEST_ASSERT_EQUAL(P3A_HTTP_MATCHED, p3a_http_match(ROUTES, N, P3A_HTTP_GET, "/api/v2/jobs/1", 14, NULL, NULL, NULL));
}

static void test_method_names(void)
{
    TEST_ASSERT_EQUAL_STRING("GET", p3a_http_method_name(P3A_HTTP_GET));
    TEST_ASSERT_EQUAL_STRING("PATCH", p3a_http_method_name(P3A_HTTP_PATCH));
    TEST_ASSERT_EQUAL_STRING("DELETE", p3a_http_method_name(P3A_HTTP_DELETE));
    TEST_ASSERT_EQUAL_STRING("?", p3a_http_method_name(P3A_HTTP_METHOD_COUNT));
    TEST_ASSERT_EQUAL_STRING("?", p3a_http_method_name((p3a_http_method_t)-1));
}

// --- URL decoding ---------------------------------------------------------------

static char dec[16];

static int decode(const char *in)
{
    memset(dec, 'x', sizeof(dec));
    return p3a_http_url_decode(in, strlen(in), dec, sizeof(dec));
}

static void test_url_decode(void)
{
    TEST_ASSERT_EQUAL(7, decode("my%20list"));
    TEST_ASSERT_EQUAL_STRING("my list", dec);
    TEST_ASSERT_EQUAL(3, decode("a+b"));
    TEST_ASSERT_EQUAL_STRING("a b", dec);
    TEST_ASSERT_EQUAL(3, decode("%41%2f%7A"));
    TEST_ASSERT_EQUAL_STRING("A/z", dec);
    TEST_ASSERT_EQUAL(0, decode(""));
    TEST_ASSERT_EQUAL_STRING("", dec);
    TEST_ASSERT_EQUAL(2, decode("%C3%A9"));  // UTF-8 passes through
}

static void test_url_decode_refuses_malformed_and_dangerous_input(void)
{
    TEST_ASSERT_EQUAL(-1, decode("%"));
    TEST_ASSERT_EQUAL(-1, decode("%4"));
    TEST_ASSERT_EQUAL(-1, decode("abc%4"));
    TEST_ASSERT_EQUAL(-1, decode("%zz"));
    TEST_ASSERT_EQUAL(-1, decode("%00"));      // an embedded terminator
    TEST_ASSERT_EQUAL(-1, decode("a%0Ab"));    // a line break
    TEST_ASSERT_EQUAL(-1, decode("a%7Fb"));
    // Exactly fits: 15 characters and the terminator.
    TEST_ASSERT_EQUAL(15, decode("123456789012345"));
    TEST_ASSERT_EQUAL(-1, decode("1234567890123456"));
    TEST_ASSERT_EQUAL(-1, p3a_http_url_decode("a", 1, dec, 0));
    TEST_ASSERT_EQUAL(-1, p3a_http_url_decode(NULL, 0, dec, sizeof(dec)));
}

// --- envelopes ------------------------------------------------------------------

static void test_ok_envelope(void)
{
    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "n", 3);
    char *text = p3a_http_envelope_ok(data);
    TEST_ASSERT_EQUAL_STRING("{\"ok\":true,\"data\":{\"n\":3}}", text);
    cJSON_free(text);

    text = p3a_http_envelope_ok(NULL);
    TEST_ASSERT_EQUAL_STRING("{\"ok\":true,\"data\":{}}", text);
    cJSON_free(text);

    text = p3a_http_envelope_ok(cJSON_CreateArray());
    TEST_ASSERT_EQUAL_STRING("{\"ok\":true,\"data\":[]}", text);
    cJSON_free(text);
}

static char err[160];

static void test_error_envelope(void)
{
    size_t n = p3a_http_envelope_error(err, sizeof(err), "NOT_FOUND", "no such job");
    TEST_ASSERT_EQUAL_STRING("{\"ok\":false,\"error\":\"no such job\",\"code\":\"NOT_FOUND\"}", err);
    TEST_ASSERT_EQUAL(strlen(err), n);
    // Missing parts get stand-ins rather than a broken envelope.
    p3a_http_envelope_error(err, sizeof(err), NULL, NULL);
    TEST_ASSERT_EQUAL_STRING("{\"ok\":false,\"error\":\"ERROR\",\"code\":\"ERROR\"}", err);
    p3a_http_envelope_error(err, sizeof(err), "X", "");
    TEST_ASSERT_EQUAL_STRING("{\"ok\":false,\"error\":\"X\",\"code\":\"X\"}", err);
}

// Whatever goes in, what comes out parses as JSON and carries the three fields.
static void assert_valid_error(const char *code, const char *message, size_t buf_len)
{
    char buf[512];
    memset(buf, '#', sizeof(buf));
    size_t n = p3a_http_envelope_error(buf, buf_len, code, message);
    TEST_ASSERT_TRUE(n > 0 && n < buf_len);
    TEST_ASSERT_EQUAL('\0', buf[n]);
    TEST_ASSERT_EQUAL('#', buf[buf_len]);  // nothing written past the buffer
    cJSON *parsed = cJSON_Parse(buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(parsed, buf);
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(parsed, "ok")));
    TEST_ASSERT_TRUE(cJSON_IsString(cJSON_GetObjectItem(parsed, "error")));
    TEST_ASSERT_TRUE(cJSON_IsString(cJSON_GetObjectItem(parsed, "code")));
    cJSON_Delete(parsed);
}

static void test_error_envelope_escapes_and_truncates(void)
{
    p3a_http_envelope_error(err, sizeof(err), "INVALID", "bad \"name\"\n\\path\t!");
    cJSON *parsed = cJSON_Parse(err);
    TEST_ASSERT_NOT_NULL(parsed);
    TEST_ASSERT_EQUAL_STRING("bad \"name\"\n\\path\t!", cJSON_GetObjectItem(parsed, "error")->valuestring);
    cJSON_Delete(parsed);

    char huge[400];
    memset(huge, 'm', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    for (size_t len = P3A_HTTP_ERROR_MIN_BUF; len <= 300; len += 7) {
        assert_valid_error("TOO_LONG", huge, len);
    }
    // A message made only of characters that expand when escaped.
    memset(huge, '"', sizeof(huge) - 1);
    for (size_t len = P3A_HTTP_ERROR_MIN_BUF; len <= 200; len++) {
        assert_valid_error("Q", huge, len);
    }
    memset(huge, '\x01', sizeof(huge) - 1);
    for (size_t len = P3A_HTTP_ERROR_MIN_BUF; len <= 200; len++) {
        assert_valid_error("C", huge, len);
    }
    // A code that is too long or holds anything odd is cleaned, not trusted.
    assert_valid_error("lower\"case\\and-a-very-long-code-that-goes-on-and-on-and-on", "m", P3A_HTTP_ERROR_MIN_BUF);
    assert_valid_error("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", huge, P3A_HTTP_ERROR_MIN_BUF);

    TEST_ASSERT_EQUAL(0, p3a_http_envelope_error(err, P3A_HTTP_ERROR_MIN_BUF - 1, "X", "y"));
    TEST_ASSERT_EQUAL(0, p3a_http_envelope_error(NULL, 200, "X", "y"));
}

static void test_errors_map_to_statuses_and_codes(void)
{
    int status;
    const char *code;
#define MAPS(e, s, c)                         \
    p3a_http_error_for(e, &status, &code);    \
    TEST_ASSERT_EQUAL(s, status);             \
    TEST_ASSERT_EQUAL_STRING(c, code)
    MAPS(ESP_ERR_INVALID_ARG, 400, "INVALID");
    MAPS(ESP_ERR_NOT_FOUND, 404, "NOT_FOUND");
    MAPS(ESP_ERR_INVALID_STATE, 409, "NOT_NOW");
    MAPS(ESP_ERR_NO_MEM, 503, "NO_MEMORY");
    MAPS(ESP_ERR_TIMEOUT, 504, "TIMEOUT");
    MAPS(P3A_ERR_MISSING, 404, "MISSING");
    MAPS(P3A_ERR_NO_KEY, 409, "NO_KEY");
    MAPS(P3A_ERR_RATE_LIMITED, 429, "RATE_LIMITED");
    MAPS(P3A_ERR_SD_FAILED, 503, "SD_FAILED");
    MAPS(P3A_ERR_AUTH, 502, "AUTH");
    MAPS(ESP_FAIL, 500, "INTERNAL");
    MAPS(0x7777, 500, "INTERNAL");
#undef MAPS
    p3a_http_error_for(ESP_FAIL, NULL, NULL);  // optional outputs
    // Every p3a error has a mapping of its own, and every status has a line.
    for (int e = P3A_ERR_BASE + 1; e < P3A_ERR_END_; e++) {
        p3a_http_error_for(e, &status, &code);
        TEST_ASSERT_NOT_EQUAL_MESSAGE(500, status, p3a_err_name(e));
        TEST_ASSERT_NULL_MESSAGE(strstr(p3a_http_status_line(status), "500"), p3a_err_name(e));
    }
    TEST_ASSERT_EQUAL_STRING("200 OK", p3a_http_status_line(200));
    TEST_ASSERT_EQUAL_STRING("405 Method Not Allowed", p3a_http_status_line(405));
    TEST_ASSERT_EQUAL_STRING("500 Internal Server Error", p3a_http_status_line(799));
}

// --- ETags ----------------------------------------------------------------------

static void test_etag(void)
{
    char a[P3A_HTTP_ETAG_LEN], b[P3A_HTTP_ETAG_LEN], c[P3A_HTTP_ETAG_LEN];
    p3a_http_etag("{\"ok\":true}", 11, a);
    p3a_http_etag("{\"ok\":true}", 11, b);
    p3a_http_etag("{\"ok\":false}", 12, c);
    TEST_ASSERT_EQUAL_STRING(a, b);
    TEST_ASSERT_TRUE(strcmp(a, c) != 0);
    TEST_ASSERT_EQUAL(P3A_HTTP_ETAG_LEN - 2, strlen(a));
    TEST_ASSERT_EQUAL_MEMORY("W/\"", a, 3);
    TEST_ASSERT_EQUAL('"', a[11]);
    // CRC-32 of "123456789" is the standard check value cbf43926.
    p3a_http_etag("123456789", 9, a);
    TEST_ASSERT_EQUAL_STRING("W/\"cbf43926\"", a);
}

static void test_etag_matching(void)
{
    const char *tag = "W/\"cbf43926\"";
    TEST_ASSERT_TRUE(p3a_http_etag_matches("W/\"cbf43926\"", tag));
    TEST_ASSERT_TRUE(p3a_http_etag_matches("\"cbf43926\"", tag));  // weak comparison
    TEST_ASSERT_TRUE(p3a_http_etag_matches("\"aaaa\", W/\"cbf43926\"", tag));
    TEST_ASSERT_TRUE(p3a_http_etag_matches("W/\"cbf43926\" , \"bbbb\"", tag));
    TEST_ASSERT_TRUE(p3a_http_etag_matches("*", tag));
    TEST_ASSERT_FALSE(p3a_http_etag_matches("W/\"cbf43927\"", tag));
    TEST_ASSERT_FALSE(p3a_http_etag_matches("\"cbf43926", tag));
    TEST_ASSERT_FALSE(p3a_http_etag_matches("\"cbf43926\"x", tag));
    TEST_ASSERT_FALSE(p3a_http_etag_matches("", tag));
    TEST_ASSERT_FALSE(p3a_http_etag_matches(NULL, tag));
    TEST_ASSERT_FALSE(p3a_http_etag_matches("*", NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_literal_routes);
    RUN_TEST(test_method_selects_among_routes_for_one_path);
    RUN_TEST(test_wrong_method_is_told_apart_from_no_such_path);
    RUN_TEST(test_matching_is_exact);
    RUN_TEST(test_one_parameter);
    RUN_TEST(test_a_literal_listed_first_wins_over_a_parameter);
    RUN_TEST(test_two_parameters_arrive_in_pattern_order);
    RUN_TEST(test_a_trailing_star_takes_the_rest);
    RUN_TEST(test_path_length_is_honoured_not_the_terminator);
    RUN_TEST(test_bad_tables_and_arguments_match_nothing);
    RUN_TEST(test_method_names);
    RUN_TEST(test_url_decode);
    RUN_TEST(test_url_decode_refuses_malformed_and_dangerous_input);
    RUN_TEST(test_ok_envelope);
    RUN_TEST(test_error_envelope);
    RUN_TEST(test_error_envelope_escapes_and_truncates);
    RUN_TEST(test_errors_map_to_statuses_and_codes);
    RUN_TEST(test_etag);
    RUN_TEST(test_etag_matching);
    return UNITY_END();
}
