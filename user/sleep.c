#include "types.h"
#include "user.h"

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf(2, "Usage: sleep <ticks>\n");
        exit();
    }
    int ticks = atoi(argv[1]);
    sleep(ticks);
    exit();
}
