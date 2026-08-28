#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <wayland-server-core.h>

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    struct mywm_server server = {0};
    
    printf("Starting MyWM - Minimal Wayland Compositor\n");
    
    server_init(&server);
    server_run(&server);
    server_finish(&server);
    
    return 0;
}