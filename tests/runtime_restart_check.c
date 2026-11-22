#include "restart.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *path = argv[1];
    char id[24];
    assert(restart_write(path, "1234abcd"));
    assert(restart_read(path, id, sizeof(id)) && !strcmp(id, "1234abcd"));
    assert(
        restart_read(path, id, sizeof(id))); /* a crash before successful launch preserves intent */
    assert(!restart_write(path, "../../invalid"));
    assert(restart_read(path, id, sizeof(id)) && !strcmp(id, "1234abcd"));
    assert(!restart_acknowledge(path, "different"));
    assert(restart_read(path, id, sizeof(id)));
    assert(restart_acknowledge(path, "1234abcd"));
    assert(!restart_read(path, id, sizeof(id)));
    FILE *f = fopen(path, "w");
    assert(f);
    fputs("1234abcd\ntrailing", f);
    fclose(f);
    assert(!restart_read(path, id, sizeof(id)));
    assert(access(path, F_OK) == 0);
    puts("PASS restart recovery, checked atomic write, matching acknowledgement, invalid/truncated "
         "retention");
}
