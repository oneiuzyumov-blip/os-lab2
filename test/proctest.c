
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "proclaunch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <time.h>
#endif

static void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;

    while (nanosleep(&ts, &ts) == -1 &&
           errno == EINTR) {}
#endif
}

static int test_async(const char *self) {
    const char *args[] = {
        self, "--child", "42", "1000", NULL
    };

    pl_process proc;
    int code = -1;
    int polls = 0;

    if (pl_start(args, &proc) != 0)
        return 0;

    while (1) {
        int result = pl_poll(&proc, &code);

        if (result == 1)
            break;

        if (result == -1)
            return 0;

        polls++;
        sleep_ms(100);
    }

    printf("Poll iterations: %d\n", polls);
    printf("Exit code: %d\n", code);

    return polls > 3 && code == 42;
}

static int test_wait(const char *self) {
    const char *args[] = {
        self, "--child", "7", "300", NULL
    };

    pl_process proc;
    int code = -1;

    if (pl_start(args, &proc) != 0)
        return 0;

    if (pl_wait(&proc, &code) != 0)
        return 0;

    printf("Exit code: %d\n", code);

    return code == 7;
}

static int test_arguments(const char *self) {
    const char *args[] = {
        self,
        "--child",
        "0",
        "0",
        "hello \"big\" world\\",
        NULL
    };

    pl_process proc;
    int code = -1;

    if (pl_start(args, &proc) != 0)
        return 0;

    if (pl_wait(&proc, &code) != 0)
        return 0;

    return code == 0;
}

static int test_missing(void) {
    const char *args[] = {
        "no_such_program_12345", NULL
    };

    pl_process proc;
    int result = pl_start(args, &proc);

    if (result == 0) {
        int code;
        pl_wait(&proc, &code);
        return 0;
    }

    return result == -1;
}

static int test_exit_300(const char *self) {
    const char *args[] = {
        self, "--child", "300", "0", NULL
    };

    pl_process proc;
    int code = -1;

    if (pl_start(args, &proc) != 0)
        return 0;

    if (pl_wait(&proc, &code) != 0)
        return 0;

    printf("Exit code for 300: %d\n", code);

#ifdef _WIN32
    return code == 300;
#else
    return code == 44;
#endif
}

static void print_result(
    const char *name,
    int passed,
    int *failures
) {
    printf("[%s] %s\n",
           passed ? " OK " : "FAIL",
           name);

    if (!passed)
        (*failures)++;
}

int main(int argc, char *argv[]) {
    if (argc >= 4 && strcmp(argv[1], "--child") == 0) { /* режим ребёнка */
        sleep_ms(atoi(argv[3])); /* поработать N мс */

        if (argc >= 5) {
            const char *expected =
                "hello \"big\" world\\";

            if (strcmp(argv[4], expected) != 0)
                return 91;
        }

        return atoi(argv[2]); /* вернуть заданный код */
    }

    int failures = 0;

    print_result(
        "Asynchronous launch",
        test_async(argv[0]),
        &failures
    );

    print_result(
        "Wait returns 7",
        test_wait(argv[0]),
        &failures
    );

    print_result(
        "Arguments with spaces",
        test_arguments(argv[0]),
        &failures
    );

    print_result(
        "Nonexistent program",
        test_missing(),
        &failures
    );

    print_result(
        "Exit code 300",
        test_exit_300(argv[0]),
        &failures
    );

    printf("\nFailures: %d\n", failures);

    return failures == 0 ? 0 : 1;
}
