#include "saves.h"
#include <stdio.h>
int main(int argc, char **argv)
{
    if (argc != 4)
        return 2;
    char error[256];
    if (!save_recover(argv[1], NULL, error, sizeof(error)))
    {
        fprintf(stderr, "%s\n", error);
        return 3;
    }
    SaveResult result;
    if (!save_restore(argv[1], argv[2], argv[3], true, NULL, &result))
    {
        fprintf(stderr, "%s\n", result.error);
        return 1;
    }
    puts("verified native snapshot restored");
    return 0;
}
