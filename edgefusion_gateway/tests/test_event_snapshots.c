#include <assert.h>
#include <string.h>
#include <sys/statvfs.h>
static int low_space;
static int test_space(const char *path, struct statvfs *buf)
{
    (void)path;
    memset(buf, 0, sizeof(*buf));
    buf->f_frsize = 1024 * 1024;
    buf->f_bavail = low_space ? 1 : 1000;
    return 0;
}
#define statvfs(path, buf) test_space(path, buf)
#include "../src/event_bus.c"
#undef statvfs
#include "../src/web.c"

int main(void)
{
    char dir[] = "/tmp/edgefusion-events-XXXXXX";
    assert(mkdtemp(dir));
    assert(event_bus_init(":memory:", dir, 500) == 0);
    const uint8_t jpg[] = {0xff, 0xd8, 0xff, 0xd9};
    for (int i = 1; i <= 205; i++) {
        event_t ev = {0};
        strcpy(ev.event_type, "test");
        assert(event_bus_publish(&ev, jpg, sizeof(jpg)) == i);
    }
    char unrelated[512];
    snprintf(unrelated, sizeof(unrelated), "%s/other.jpg", dir);
    FILE *f = fopen(unrelated, "wb");
    assert(f); fputs("keep", f); fclose(f);
    low_space = 1;
    cleanup_snapshots();
    for (int i = 1; i <= 205; i++) {
        event_t ev;
        assert(event_bus_get(i, &ev) == 0);
        assert(strcmp(snapshot_state(&ev), i <= 5 ? "missing" : "available") == 0);
    }
    assert(access(unrelated, F_OK) == 0);
    /* Repeat page 1 -> page 2 -> page 1 under persistent low space. */
    for (int page = 0; page < 3; page++) {
        event_t evs[20];
        cleanup_snapshots();
        assert(event_bus_query_filtered(evs, 20, NULL, page == 1 ? 20 : 0) == 20);
        for (int j = 0; j < 20; j++)
            assert(strcmp(snapshot_state(&evs[j]), "available") == 0);
    }
    event_t ev = {0};
    assert(event_bus_publish(&ev, jpg, sizeof(jpg)) == 206);
    assert(strcmp(snapshot_state(&ev), "available") == 0);
    event_t old;
    assert(event_bus_get(6, &old) == 0);
    assert(strcmp(snapshot_state(&old), "missing") == 0);
    event_t no_image = {0};
    assert(strcmp(snapshot_state(&no_image), "none") == 0);
    event_bus_deinit();
    for (int i = 1; i <= 206; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/evt_%d.jpg", dir, i);
        unlink(path);
    }
    unlink(unrelated);
    assert(rmdir(dir) == 0);
    puts("PASS: numeric retention, recent pages survive low space, missing/none states");
    return 0;
}
