#include "recorder.h"
#include "conf.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void fixture(const char *dir, const char *name, time_t stamp)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb"); assert(f); fputs("fixture", f); fclose(f);
    struct timespec times[2] = {{stamp, 0}, {stamp, 0}};
    assert(utimensat(AT_FDCWD, path, times, 0) == 0);
}
static int exists(const char *dir, const char *name)
{
    char path[512]; struct stat st;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    return lstat(path, &st) == 0;
}
int main(void)
{
    char dir[] = "/tmp/edgefusion-retention-XXXXXX";
    assert(mkdtemp(dir));
    const time_t now = 1800000000, cutoff = now - 3 * 86400;
    const char *names[] = {
        "rec_20200101_120000_1.mp4", /* expired */
        "rec_20200101_120000_2.mp4", /* exact cutoff */
        "rec_20200101_120000_3.mp4", /* recent despite old filename */
        "rec_20200101_120000_4.mp4", /* active but old mtime */
        "rec_20200101_120000_5.mp4", /* future mtime */
        "other_20200101_120000_1.mp4",
        "rec_notes.mp4", "rec_20200101_120000_6.jpg",
        "rec_20200101_120000_123456789_7.mp4" /* current filename format */
    };
    time_t stamps[] = {cutoff-1,cutoff,cutoff+1,cutoff-100,now+86400,
                       cutoff-1,cutoff-1,cutoff-1,cutoff-1};
    for (int i=0;i<9;i++) fixture(dir,names[i],stamps[i]);
    char link[512], child[512];
    snprintf(link,sizeof(link),"%s/rec_20200101_120000_8.mp4",dir);
    assert(symlink(names[3],link)==0);
    snprintf(child,sizeof(child),"%s/rec_20200101_120000_9.mp4",dir);
    assert(mkdir(child,0700)==0);
    assert(recorder_cleanup_expired(dir,"rec",0,names[3],now)==0);
    assert(recorder_cleanup_expired(dir,"rec",3,names[3],now)==2);
    assert(!exists(dir,names[0]) && !exists(dir,names[8]));
    for (int i=1;i<8;i++) assert(exists(dir,names[i]));
    assert(exists(dir,"rec_20200101_120000_8.mp4"));
    assert(exists(dir,"rec_20200101_120000_9.mp4"));
    assert(recorder_cleanup_expired(dir,"rec",3,names[3],now)==0);
    /* Simulate the next maintenance tick without any camera or recorder. */
    assert(recorder_cleanup_expired(dir,"rec",3,names[3],now+60)==2);
    assert(exists(dir,names[3]));
    assert(recorder_cleanup_expired(dir,"rec",3,"",now+60)==1);

    char cfgpath[512]; snprintf(cfgpath,sizeof(cfgpath),"%s/test.conf",dir);
    FILE *f=fopen(cfgpath,"w"); assert(f); fclose(f);
    gateway_conf_t cfg;
    assert(conf_load(&cfg,cfgpath)==0 && cfg.storage_retention_days==3);
    const char *values[]={"3","7","0","-1","3abc","999999999999999999999999","3651"};
    for (int i=0;i<7;i++) {
        f=fopen(cfgpath,"w"); assert(f);
        fprintf(f,"storage_retention_days = %s\n",values[i]); fclose(f);
        int rc=conf_load(&cfg,cfgpath);
        if (i<2) {
            assert(rc==0 && cfg.storage_retention_days==atoi(values[i]));
            assert(conf_save(&cfg)==0);
            assert(conf_load(&cfg,cfgpath)==0 && cfg.storage_retention_days==atoi(values[i]));
        } else assert(rc!=0);
    }
    for (int i=0;i<9;i++) {
        char path[512]; snprintf(path,sizeof(path),"%s/%s",dir,names[i]); unlink(path);
    }
    unlink(link); rmdir(child); unlink(cfgpath); assert(rmdir(dir)==0);
    puts("PASS: 72h boundary, periodic expiry, active/recent/future protection, symlinks, unrelated files, config persistence");
    return 0;
}
