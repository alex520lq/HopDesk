#ifndef HOP_CONFIG_MIGRATE_H
#define HOP_CONFIG_MIGRATE_H
/* macOS configuration migration. Never publish a partial canonical config.
 * mkstemp supplies a private, unique sibling file; every failure leaves the
 * original destination intact and the legacy source available for retry. */
#ifndef _WIN32
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <errno.h>

static inline int hop_config_copy_atomic(const char *source, const char *target) {
    char temporary[PATH_MAX];
    int n=snprintf(temporary,sizeof(temporary),"%s.tmp.XXXXXX",target);
    if(n<0 || n>=(int)sizeof(temporary)){errno=ENAMETOOLONG;return 0;}
    FILE *src=fopen(source,"rb");
    if(!src)return 0;
    int fd=mkstemp(temporary);
    if(fd<0){int e=errno;fclose(src);errno=e;return 0;}
    FILE *dst=fdopen(fd,"wb");
    if(!dst){int e=errno;close(fd);unlink(temporary);fclose(src);errno=e;return 0;}
    int ok=1, saved_error=0;
    char buffer[4096];size_t got;
    while((got=fread(buffer,1,sizeof(buffer),src))>0) {
        if(fwrite(buffer,1,got,dst)!=got){ok=0;break;}
    }
    if(ferror(src))ok=0;
    if(!ok)saved_error=errno?errno:EIO;
    if(fclose(src)!=0){ok=0;saved_error=errno;}
    if(ok && (fflush(dst)!=0 || fsync(fileno(dst))!=0)){ok=0;saved_error=errno;}
    if(fclose(dst)!=0){ok=0;saved_error=errno;}
    if(ok && rename(temporary,target)!=0){ok=0;saved_error=errno;}
    if(!ok){unlink(temporary);errno=saved_error?saved_error:EIO;}
    return ok;
}
#endif
#endif
