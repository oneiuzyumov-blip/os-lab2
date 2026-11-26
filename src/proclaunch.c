
#include "proclaunch.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

#include <windows.h>

static char *make_command_line(const char *const argv[]) {
    size_t capacity = 1;

    for (int i = 0; argv[i]; ++i)
        capacity += strlen(argv[i]) * 2 + 4;

    char *cmd = malloc(capacity);
    if (!cmd)
        return NULL;

    char *out = cmd;

    for (int i = 0; argv[i]; ++i) {
        if (i)
            *out++ = ' ';

        *out++ = '"';

        const char *p = argv[i];

        while (*p) {
            size_t slashes = 0;

            while (*p == '\\') {
                ++slashes;
                ++p;
            }

            if (*p == '"') {
                for (size_t j = 0; j < 2 * slashes + 1; ++j)
                    *out++ = '\\';

                *out++ = '"';
                ++p;
            } else if (*p == '\0') {
                for (size_t j = 0; j < 2 * slashes; ++j)
                    *out++ = '\\';
            } else {
                for (size_t j = 0; j < slashes; ++j)
                    *out++ = '\\';

                *out++ = *p++;
            }
        }

        *out++ = '"';
    }

    *out = '\0';
    return cmd;
}

int pl_start(const char *const argv[], pl_process *proc) {
    if (!argv || !argv[0] || !proc)
        return -1;

    char *cmd = make_command_line(argv);
    if (!cmd)
        return -1;

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;

    ZeroMemory(&si, sizeof si);
    ZeroMemory(&pi, sizeof pi);
    si.cb = sizeof si;

    BOOL ok = CreateProcessA(
        NULL, cmd, NULL, NULL, FALSE,
        0, NULL, NULL, &si, &pi
    );

    free(cmd);

    if (!ok)
        return -1; /* GetLastError() == 2: файл не найден */

    CloseHandle(pi.hThread); /* описатель потока не нужен */

    proc->handle = (intptr_t)pi.hProcess;
    proc->pid = (long)pi.dwProcessId;

    return 0;
}

int pl_wait(pl_process *proc, int *exit_code) {
    if (!proc || !exit_code || !proc->handle)
        return -1;

    HANDLE h = (HANDLE)proc->handle;

    if (WaitForSingleObject(h, INFINITE) != WAIT_OBJECT_0)
        return -1;

    DWORD code;
    if (!GetExitCodeProcess(h, &code))
        return -1;

    *exit_code = (int)code;
    CloseHandle(h);
    proc->handle = 0;
    proc->pid = 0;

    return 0;
}

int pl_poll(pl_process *proc, int *exit_code) {
    if (!proc || !exit_code || !proc->handle)
        return -1;

    HANDLE h = (HANDLE)proc->handle;
    DWORD result = WaitForSingleObject(h, 0);

    if (result == WAIT_TIMEOUT)
        return 0;

    if (result != WAIT_OBJECT_0)
        return -1;

    DWORD code;
    if (!GetExitCodeProcess(h, &code))
        return -1;

    *exit_code = (int)code;
    CloseHandle(h);
    proc->handle = 0;
    proc->pid = 0;

    return 1;
}

#else

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int decode_status(int status) {
    if (WIFEXITED(status))
        return WEXITSTATUS(status);

    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);

    return -1;
}

int pl_start(const char *const argv[], pl_process *proc) {
    if (!argv || !argv[0] || !proc) {
        errno = EINVAL;
        return -1;
    }

    int fd[2];

    if (pipe(fd) == -1)
        return -1;

    if (fcntl(fd[1], F_SETFD, FD_CLOEXEC) == -1) {
        int err = errno;
        close(fd[0]);
        close(fd[1]);
        errno = err;
        return -1;
    }

    pid_t pid = fork();

    if (pid < 0) { /* процесс не создан */
        int err = errno;
        close(fd[0]);
        close(fd[1]);
        errno = err;
        return -1;
    }

    if (pid == 0) {
        close(fd[0]);

        execvp(argv[0], (char *const *)argv); /* при успехе не возвращается */

        int err = errno;
        const char *data = (const char *)&err;
        size_t left = sizeof(err);

        while (left > 0) {
            ssize_t n = write(fd[1], data, left);

            if (n == -1 && errno == EINTR)
                continue;

            if (n <= 0)
                break;

            data += n;
            left -= (size_t)n;
        }

        _exit(127); /* exec не удался */
    }

    close(fd[1]);

    int child_error = 0;
    char *data = (char *)&child_error;
    size_t received = 0;

    while (received < sizeof(child_error)) {
        ssize_t n = read(
            fd[0],
            data + received,
            sizeof(child_error) - received
        );

        if (n == -1 && errno == EINTR)
            continue;

        if (n == -1) {
            int err = errno;
            close(fd[0]);

            int status;
            while (waitpid(pid, &status, 0) == -1 &&
                   errno == EINTR) {}

            errno = err;
            return -1;
        }

        if (n == 0)
            break;

        received += (size_t)n;
    }

    close(fd[0]);

    if (received != 0) {
        int status;
        while (waitpid(pid, &status, 0) == -1 &&
               errno == EINTR) {}

        errno = received == sizeof(child_error)
                    ? child_error : EIO;
        return -1;
    }

    proc->pid = (long)pid; /* родитель: сразу выходим */
    proc->handle = 0;

    return 0;
}

int pl_wait(pl_process *proc, int *exit_code) {
    if (!proc || !exit_code || proc->pid <= 0) {
        errno = EINVAL;
        return -1;
    }

    int status;
    pid_t result;

    do {
        result = waitpid((pid_t)proc->pid, &status, 0);
    } while (result == -1 && errno == EINTR);

    if (result == -1)
        return -1;

    *exit_code = decode_status(status);
    proc->pid = 0;

    return 0;
}

int pl_poll(pl_process *proc, int *exit_code) {
    if (!proc || !exit_code || proc->pid <= 0) {
        errno = EINVAL;
        return -1;
    }

    int status;
    pid_t result;

    do {
        result = waitpid(
            (pid_t)proc->pid,
            &status,
            WNOHANG
        );
    } while (result == -1 && errno == EINTR);

    if (result == -1)
        return -1;

    if (result == 0)
        return 0;

    *exit_code = decode_status(status);
    proc->pid = 0;

    return 1;
}

#endif
