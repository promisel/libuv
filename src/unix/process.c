/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "uv.h"
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>

#if defined(__APPLE__) && !TARGET_OS_IPHONE
# include <crt_externs.h>
# define environ (*_NSGetEnviron())
#else
extern char **environ;
#endif

#ifdef __linux__
# include <grp.h>
# ifdef __GLIBC_PREREQ
#  if ! (__GLIBC_PREREQ(2,11))
int execvpe(const char *file, char *const argv[], char *const envp[])
{
	const char *p, *z, *path = getenv("PATH");
	size_t l, k;

	errno = ENOENT;
	if (!*file) return -1;

	if (strchr(file, '/'))
		return execve(file, argv, envp);

	if (!path) path = "/usr/local/bin:/bin:/usr/bin";
	k = strnlen(file, NAME_MAX+1);
	if (k > NAME_MAX) {
		errno = ENAMETOOLONG;
		return -1;
	}
	l = strnlen(path, PATH_MAX-1)+1;

	for(p=path; ; p=z) {
		char b[l+k+1];
		z = strchr(p, ':');
		if (!z) z = p+strlen(p);
		if (z-p >= l) {
			if (!*z++) break;
			continue;
		}
		memcpy(b, p, z-p);
		b[z-p] = '/';
		memcpy(b+(z-p)+(z>p), file, k+1);
		execve(b, argv, envp);
		if (errno != ENOENT) return -1;
		if (!*z++) break;
	}
	return -1;
}
#  endif
# endif
#endif


static void uv__chld(uv_signal_t* handle, int signum) {
  uv_process_t* process;
  uv_loop_t* loop;
  int exit_status;
  int term_signal;
  int status;
  pid_t pid;
  QUEUE pending;
  QUEUE* q;
  QUEUE* h;

  assert(signum == SIGCHLD);

  QUEUE_INIT(&pending);
  loop = handle->loop;

  h = &loop->process_handles;
  q = QUEUE_HEAD(h);
  while (q != h) {
    process = QUEUE_DATA(q, uv_process_t, queue);
    q = QUEUE_NEXT(q);

    do
      pid = waitpid(process->pid, &status, WNOHANG);
    while (pid == -1 && errno == EINTR);

    if (pid == 0)
      continue;

    if (pid == -1) {
      if (errno != ECHILD)
        abort();
      continue;
    }

    process->pid = 0; // pid is no longer valid (or unique)
    process->status = status;
    QUEUE_REMOVE(&process->queue);
    QUEUE_INSERT_TAIL(&pending, &process->queue);
  }

  h = &pending;
  q = QUEUE_HEAD(h);
  while (q != h) {
    process = QUEUE_DATA(q, uv_process_t, queue);
    q = QUEUE_NEXT(q);

    QUEUE_REMOVE(&process->queue);
    QUEUE_INIT(&process->queue);
    uv__handle_stop(process);

    if (process->exit_cb == NULL)
      continue;

    exit_status = 0;
    if (WIFEXITED(process->status))
      exit_status = WEXITSTATUS(process->status);

    term_signal = 0;
    if (WIFSIGNALED(process->status))
      term_signal = WTERMSIG(process->status);

    process->exit_cb(process, exit_status, term_signal);
  }
  assert(QUEUE_EMPTY(&pending));
}


int uv__make_pipe(int fds[2], int flags) {
#if defined(__linux__)
  static int no_pipe2;

  if (no_pipe2)
    goto skip;

  if (uv__pipe2(fds, flags | UV__O_CLOEXEC) == 0)
    return 0;

  if (errno != ENOSYS)
    return -errno;

  no_pipe2 = 1;

skip:
#endif

  if (pipe(fds))
    return -errno;

  uv__cloexec(fds[0], 1);
  uv__cloexec(fds[1], 1);

  if (flags & UV__F_NONBLOCK) {
    uv__nonblock(fds[0], 1);
    uv__nonblock(fds[1], 1);
  }

  return 0;
}


/*
 * Used for initializing stdio streams like options.stdin_stream. Returns
 * zero on success. See also the cleanup section in uv_spawn().
 */
static int uv__process_init_stdio(uv_stdio_container_t* container, int *fd) {
  switch (container->type) {
    case UV_STREAM:
      if (container->data.stream == NULL) {
        *fd = -1;
        return 0;
      } else {
        *fd = container->data.stream->io_watcher.fd;
      }
      break;
    case UV_RAW_FD:
    case UV_RAW_HANDLE:
      *fd = container->data.fd;
      break;
    default:
      assert (0 && "Unexpected flags");
      *fd = -1;
    return -EINVAL;
  }
  if (*fd == -1) {
    return -EINVAL;
  } else {
    return 0;
  }
}


#ifndef __linux__
static void uv__write_int(int fd, int val) {
  ssize_t n;

  do
    n = write(fd, &val, sizeof(val));
  while (n == -1 && errno == EINTR);

  if (n == -1 && errno == EPIPE)
    return; /* parent process has quit */

  assert(n == sizeof(val));
}
#endif


#if !(defined(__APPLE__) && (TARGET_OS_TV || TARGET_OS_WATCH))
/* May share the parent's memory space. Do not alter global state.
 *
 * execvp is marked __WATCHOS_PROHIBITED __TVOS_PROHIBITED, so must be
 * avoided. Since this isn't called on those targets, the function
 * doesn't even need to be defined for them.
 */
static void uv__process_child_init(const uv_process_options_t* options,
                                   int stdio_count,
                                   int *pipes,
#ifdef __linux__
                                   volatile int* error_out
#else
                                   int error_fd
#endif
) {
  int close_fd;
  int use_fd;
  int fd;
  int err;
  sigset_t sigset;

  // make sure to start the child without any mask
  sigemptyset(&sigset);
  sigprocmask(SIG_SETMASK, &sigset, NULL);

  if (options->flags & UV_PROCESS_DETACHED)
    setsid();

  /* First duplicate low numbered fds, since it's not safe to duplicate them,
   * they could get replaced. Example: swapping stdout and stderr; without
   * this fd 2 (stderr) would be duplicated into fd 1, thus making both
   * stdout and stderr go to the same fd, which was not the intention. */
  for (fd = 0; fd < stdio_count; fd++) {
    use_fd = pipes[fd];
    if (use_fd < 0 || use_fd >= fd)
      continue;
    pipes[fd] = fcntl(use_fd, F_DUPFD, stdio_count);
    if (pipes[fd] == -1) {
      err = -errno;
      goto error;
    }
  }

  for (fd = 0; fd < stdio_count; fd++) {
    close_fd = -1;
    use_fd = pipes[fd];

    if (use_fd < 0) {
      if (fd >= 3)
        continue;
      else {
        /* redirect stdin, stdout and stderr to /dev/null even if UV_IGNORE is
         * set
         */
        use_fd = open("/dev/null", fd == 0 ? O_RDONLY : O_RDWR);
        close_fd = use_fd;

        if (use_fd == -1) {
          err = -errno;
          goto error;
        }
      }
    }

    if (fd == use_fd)
      uv__cloexec(use_fd, 0);
    else
      fd = dup2(use_fd, fd);

    if (fd == -1) {
      err = -errno;
      goto error;
    }

    if (fd <= 2)
      uv__nonblock(fd, 0);

    if (close_fd >= stdio_count)
      uv__close(close_fd);
  }

  for (fd = 0; fd < stdio_count; fd++) {
    use_fd = pipes[fd];

    if (use_fd >= stdio_count)
      uv__close(use_fd);
  }

  if (options->cwd != NULL && chdir(options->cwd)) {
    err = -errno;
    goto error;
  }

  if (options->flags & (UV_PROCESS_SETUID | UV_PROCESS_SETGID)) {
    /* When dropping privileges from root, the `setgroups` call will
     * remove any extraneous groups. If we don't call this, then
     * even though our uid has dropped, we may still have groups
     * that enable us to do super-user things. This will fail if we
     * aren't root, so don't bother checking the return value, this
     * is just done as an optimistic privilege dropping function.
     */
    SAVE_ERRNO(setgroups(0, NULL));
  }

  if ((options->flags & UV_PROCESS_SETGID) && setgid(options->gid)) {
    err = -errno;
    goto error;
  }

  if ((options->flags & UV_PROCESS_SETUID) && setuid(options->uid)) {
    err = -errno;
    goto error;
  }

  if ((options->flags & UV_PROCESS_RESET_SIGPIPE) && signal(SIGPIPE,SIG_DFL) == SIG_ERR)
  {
    err = -errno;
    goto error;
  }


#ifdef __linux__
  if (options->env != NULL) {
    execvpe(options->file, options->args, options->env);
  } else {
    execvp(options->file, options->args);
  }
#else
  if (options->env != NULL) {
    environ = options->env;
  }

  execvp(options->file, options->args);
#endif

  err = -errno;

error:
#ifdef __linux__
  *error_out = err;
#else
  uv__write_int(error_fd, err);
#endif
  _exit(127);
}
#endif


int uv_spawn(uv_loop_t* loop,
             uv_process_t* process,
             const uv_process_options_t* options) {
#if defined(__APPLE__) && (TARGET_OS_TV || TARGET_OS_WATCH)
  /* fork is marked __WATCHOS_PROHIBITED __TVOS_PROHIBITED. */
  uv__handle_init(loop, (uv_handle_t*)process, UV_PROCESS);
  QUEUE_INIT(&process->queue);
  process->pid = 0;
  return -ENOSYS;
#else
  int *pipes;
  int stdio_count;
  pid_t pid;
  int err;
  int i;
  int status;
  sigset_t sigset;
  sigset_t sigoset;
#ifdef __linux__
  volatile int exec_errorno;
  int cancelstate;
#else
  int exec_errorno;
  int signal_pipe[2] = { -1, -1 };
  ssize_t r;
#endif

  assert(options->file != NULL);
  assert(!(options->flags & ~(UV_PROCESS_DETACHED |
                              UV_PROCESS_SETGID |
                              UV_PROCESS_SETUID |
                              UV_PROCESS_WINDOWS_HIDE |
                              UV_PROCESS_WINDOWS_VERBATIM_ARGUMENTS |
                              UV_PROCESS_RESET_SIGPIPE)));

  uv__handle_init(loop, (uv_handle_t*)process, UV_PROCESS);
  QUEUE_INIT(&process->queue);
  process->pid = 0;

  stdio_count = options->stdio_count;
  if (stdio_count < 3)
    stdio_count = 3;

  err = -ENOMEM;
  pipes = uv__malloc(stdio_count * sizeof(*pipes));
  if (pipes == NULL)
    goto error;

  for (i = 0; i < stdio_count; i++) {
    pipes[i] = -1;
  }

  for (i = 0; i < options->stdio_count; i++) {
    err = uv__process_init_stdio(options->stdio + i, &pipes[i]);
    if (err)
      goto error;
  }

  process->status = 0;
  exec_errorno = 0;

  uv_signal_start(&loop->child_watcher, uv__chld, SIGCHLD);

  sigfillset(&sigset);
  sigprocmask(SIG_SETMASK, &sigset, &sigoset);

#ifdef __linux__
  /* Acquire write lock to prevent opening new fds in worker threads */
  uv_rwlock_wrlock(&loop->cloexec_lock);
  pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancelstate);

  pid = vfork();

  if (pid == -1) {
    err = -errno;
    uv_rwlock_wrunlock(&loop->cloexec_lock);
    sigprocmask(SIG_SETMASK, &sigoset, NULL);
    goto error;
  }

  if (pid == 0) {
    uv__process_child_init(options, stdio_count, pipes, &exec_errorno);
    abort();
  }

  pthread_setcancelstate(cancelstate, NULL);
  uv_rwlock_wrunlock(&loop->cloexec_lock);
#else /* !__linux__ */
  /* This pipe is used by the parent to wait until
   * the child has called `execve()`. We need this
   * to avoid the following race condition:
   *
   *    if ((pid = fork()) > 0) {
   *      kill(pid, SIGTERM);
   *    }
   *    else if (pid == 0) {
   *      execve("/bin/cat", argp, envp);
   *    }
   *
   * The parent sends a signal immediately after forking.
   * Since the child may not have called `execve()` yet,
   * there is no telling what process receives the signal,
   * our fork or /bin/cat.
   *
   * To avoid ambiguity, we create a pipe with both ends
   * marked close-on-exec. Then, after the call to `fork()`,
   * the parent polls the read end until it EOFs or errors with EPIPE.
   */
  err = uv__make_pipe(signal_pipe, 0);
  if (err) {
    sigprocmask(SIG_SETMASK, &sigoset, NULL);
    goto error;
  }

  /* Acquire write lock to prevent opening new fds in worker threads */
  uv_rwlock_wrlock(&loop->cloexec_lock);

  pid = fork();

  if (pid == -1) {
    err = -errno;
    uv_rwlock_wrunlock(&loop->cloexec_lock);
    uv__close(signal_pipe[0]);
    uv__close(signal_pipe[1]);
    sigprocmask(SIG_SETMASK, &sigoset, NULL);
    goto error;
  }

  if (pid == 0) {
    uv__process_child_init(options, stdio_count, pipes, signal_pipe[1]);
    abort();
  }

  /* Release lock in parent process */
  uv_rwlock_wrunlock(&loop->cloexec_lock);
  uv__close(signal_pipe[1]);

  do
    r = read(signal_pipe[0], &exec_errorno, sizeof(exec_errorno));
  while (r == -1 && errno == EINTR);

  if (r == 0)
    ; /* okay, EOF */
  else if (r == sizeof(exec_errorno)) {
    do
      err = waitpid(pid, &status, 0); /* okay, read errorno */
    while (err == -1 && errno == EINTR);
    assert(err == pid);
  } else if (r == -1 && errno == EPIPE) {
    do
      err = waitpid(pid, &status, 0); /* okay, got EPIPE */
    while (err == -1 && errno == EINTR);
    assert(err == pid);
  } else
    abort();

  uv__close_nocheckstdio(signal_pipe[0]);
#endif /* __linux__ */

  /* Only activate this handle if exec() happened successfully */
  if (exec_errorno == 0) {
    QUEUE_INSERT_TAIL(&loop->process_handles, &process->queue);
    uv__handle_start(process);
    process->pid = pid;
  }

  process->exit_cb = options->exit_cb;

  uv__free(pipes);
  sigprocmask(SIG_SETMASK, &sigoset, NULL);
  return exec_errorno;

error:
  if (pipes != NULL) {
    for (i = 0; i < stdio_count; i++) {
      if (options->stdio[i].type == UV_STREAM && options->stdio[i].data.stream == NULL)
        if (pipes[i] != -1)
          uv__close_nocheckstdio(pipes[i]);
    }
    uv__free(pipes);
  }

  return err;
#endif
}


int uv_process_kill(uv_process_t* process, int signum) {
  if (process->pid == 0)
    return -ESRCH;
  return uv_kill(process->pid, signum);
}


int uv_kill(int pid, int signum) {
  if (kill(pid, signum))
    return -errno;
  else
    return 0;
}


void uv__process_close(uv_process_t* handle) {
  assert(handle->pid == 0);
  QUEUE_REMOVE(&handle->queue);
  uv__handle_stop(handle);
  if (QUEUE_EMPTY(&handle->loop->process_handles))
    uv_signal_stop(&handle->loop->child_watcher);
}
