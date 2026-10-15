/* Test the real HTTP handler with a stub MQTT publisher: no broker/device needed.
 * Unused web routes are discarded by --gc-sections at link time. */
#include <assert.h>
#include "../src/web.c"

static int publish_rc, publish_count;
static char sent_device[1024], sent_command[4096];

int mqtt_bridge_send_cmd(const char *device, const char *command)
{
    publish_count++;
    snprintf(sent_device, sizeof(sent_device), "%s", device);
    snprintf(sent_command, sizeof(sent_command), "%s", command);
    return publish_rc;
}

static struct json_object *request(const char *body, int status, int expected_calls)
{
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    publish_count = 0;
    serve_mqtt_cmd(sv[0], body, body ? (int)strlen(body) : 0);
    shutdown(sv[0], SHUT_WR);
    char response[8192];
    size_t used = 0;
    ssize_t n;
    while ((n = read(sv[1], response + used, sizeof(response) - 1 - used)) > 0)
        used += (size_t)n;
    response[used] = 0;
    close(sv[0]);
    close(sv[1]);
    assert(publish_count == expected_calls);
    int actual;
    assert(sscanf(response, "HTTP/1.1 %d", &actual) == 1 && actual == status);
    const char *payload = strstr(response, "\r\n\r\n");
    const char *length = strstr(response, "Content-Length: ");
    assert(payload && length);
    payload += 4;
    assert((size_t)atoi(length + strlen("Content-Length: ")) == strlen(payload));
    struct json_object *j = json_tokener_parse(payload);
    assert(j && json_object_is_type(j, json_type_object));
    struct json_object *ok;
    assert(json_object_object_get_ex(j, "ok", &ok));
    assert(json_object_is_type(ok, json_type_boolean));
    assert(json_object_get_boolean(ok) == (status == 200));
    return j;
}

static void success(const char *cmd, const char *device, struct json_object *value)
{
    struct json_object *input = json_object_new_object();
    json_object_object_add(input, "cmd", json_object_new_string(cmd));
    if (device) json_object_object_add(input, "device", json_object_new_string(device));
    if (value) json_object_object_add(input, "value", json_object_get(value));
    struct json_object *response = request(json_object_to_json_string(input), 200, 1);
    struct json_object *payload = json_tokener_parse(sent_command), *field;
    assert(payload);
    assert(json_object_object_get_ex(payload, "cmd", &field));
    assert(strcmp(json_object_get_string(field), cmd) == 0);
    assert(strcmp(sent_device, device ? device : "A1") == 0);
    assert(json_object_object_get_ex(response, "cmd", &field));
    assert(strcmp(json_object_get_string(field), cmd) == 0);
    assert(json_object_object_get_ex(response, "device", &field));
    assert(strcmp(json_object_get_string(field), device ? device : "A1") == 0);
    if (value) {
        assert(json_object_object_get_ex(payload, "value", &field));
        assert(json_object_equal(field, value));
    }
    json_object_put(payload);
    json_object_put(response);
    json_object_put(input);
}

int main(void)
{
    success("snapshot", NULL, NULL);
    success("pause", "A1", NULL);
    success("resume", "A1", NULL);
    struct json_object *number = json_object_new_int(2);
    success("set_snapshot_interval", "A1", number);
    json_object_put(number);
    struct json_object *value = json_object_new_string("quoted \"text\" \\ newline\n");
    success("quoted\"command\\", "device\"\\", value);
    json_object_put(value);
    char long_cmd[600];
    memset(long_cmd, 'x', sizeof(long_cmd) - 1);
    long_cmd[sizeof(long_cmd) - 1] = 0;
    success(long_cmd, "A1", NULL); /* response must not be silently truncated */

    const char *invalid[] = { "", "{", "[]", "null", "1", "{}",
        "{\"cmd\":null}", "{\"cmd\":42}", "{\"cmd\":\"\"}",
        "{\"cmd\":\"pause\",\"device\":123}" };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        json_object_put(request(invalid[i], 400, 0));
    publish_rc = -1;
    struct json_object *failure = request("{\"cmd\":\"pause\"}", 503, 1), *error;
    assert(json_object_object_get_ex(failure, "error", &error));
    assert(strlen(json_object_get_string(error)) > 0);
    json_object_put(failure);
    puts("PASS: MQTT HTTP response JSON, escaping, long payload, validation and HTTP 503");
    return 0;
}
