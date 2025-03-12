/*
 * Copyright (C) 2007-2021 Intel Corporation.
 * SPDX-License-Identifier: MIT
 */

/*
 * this application calls a user-written version of free which contains
 * a specific code pattern.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void call_function();
extern void nothing_doing();

int main(int argc, char* argv[])
{
    char* buffer;

    buffer = (char*)malloc(64);
    strcpy(buffer, "abc");
    printf("%s\n", buffer);
    call_function();
    printf("returned from call_function & do_nothing.\n");
    nothing_doing();
    printf("returned from nothing_doing.\n");
    free(buffer);
    return 0;
}
