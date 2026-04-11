/*
 * swarmrt_platform.h — Cross-platform abstraction layer
 *
 * Detects OS and provides unified API for:
 * - Temp directories, home directory, path separators
 * - popen/getpid/arc4random portability
 * - Socket compat (close vs closesocket, read/write vs recv/send)
 * - mmap vs VirtualAlloc
 * - Nonblocking I/O
 * - Wake pipe (pipe() on POSIX, loopback socket on Windows)
 *
 * otonomy.ai
 */

#ifndef SWARMRT_PLATFORM_H
#define SWARMRT_PLATFORM_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ============================================================
 * OS Detection
 * ============================================================ */

#if defined(_WIN32) || defined(_WIN64)
  #define SW_OS_WINDOWS 1
#elif defined(__APPLE__) && defined(__MACH__)
  #define SW_OS_MACOS 1
#elif defined(__linux__)
  #define SW_OS_LINUX 1
#else
  #error "Unsupported platform"
#endif

/* ============================================================
 * Arch Detection
 * ============================================================ */

#if defined(__x86_64__) || defined(_M_X64)
  #define SW_ARCH_X86_64 1
#elif defined(__aarch64__) || defined(_M_ARM64)
  #define SW_ARCH_ARM64 1
#else
  #define SW_ARCH_OTHER 1
#endif

/* ============================================================
 * Includes
 * ============================================================ */

#ifdef SW_OS_WINDOWS
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <io.h>
  #include <process.h>
  #pragma comment(lib, "ws2_32.lib")
#else
  #include <unistd.h>
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <fcntl.h>
  #include <errno.h>
  #include <pthread.h>
  #include <signal.h>
  #include <sched.h>
#endif

/* ============================================================
 * Temp Directory
 * ============================================================ */

static inline const char *sw_tmpdir(void) {
#ifdef SW_OS_WINDOWS
    static char buf[MAX_PATH];
    DWORD len = GetTempPathA(MAX_PATH, buf);
    if (len > 0 && len < MAX_PATH) return buf;
    return "C:\\Temp";
#else
    return "/tmp";
#endif
}

/* ============================================================
 * Home Directory
 * ============================================================ */

static inline const char *sw_homedir(void) {
    const char *home = getenv("HOME");
#ifdef SW_OS_WINDOWS
    if (!home) home = getenv("USERPROFILE");
    if (!home) home = "C:\\Users\\Default";
#endif
    return home;
}

/* ============================================================
 * popen / pclose / getpid
 * ============================================================ */

#ifdef SW_OS_WINDOWS
  #define sw_popen(cmd, mode)  _popen(cmd, mode)
  #define sw_pclose(fp)        _pclose(fp)
  #define sw_getpid_os()       _getpid()
#else
  #define sw_popen(cmd, mode)  popen(cmd, mode)
  #define sw_pclose(fp)        pclose(fp)
  #define sw_getpid_os()       getpid()
#endif

/* ============================================================
 * Random Numbers (arc4random compat)
 * ============================================================ */

#if defined(SW_OS_MACOS)
  /* arc4random available natively */
  #define sw_random_u32()          arc4random()
  #define sw_random_uniform(upper) arc4random_uniform(upper)
#elif defined(SW_OS_LINUX)
  /* glibc 2.36+ has arc4random, older needs fallback */
  #include <features.h>
  #if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 36))
    #define sw_random_u32()          arc4random()
    #define sw_random_uniform(upper) arc4random_uniform(upper)
  #else
    static inline uint32_t sw_random_u32(void) {
        uint32_t r;
        FILE *f = fopen("/dev/urandom", "rb");
        if (f) { fread(&r, sizeof(r), 1, f); fclose(f); return r; }
        return (uint32_t)rand();
    }
    static inline uint32_t sw_random_uniform(uint32_t upper) {
        return sw_random_u32() % upper;
    }
  #endif
#elif defined(SW_OS_WINDOWS)
  static inline uint32_t sw_random_u32(void) {
      unsigned int r;
      if (rand_s(&r) == 0) return (uint32_t)r;
      return (uint32_t)rand();
  }
  static inline uint32_t sw_random_uniform(uint32_t upper) {
      if (upper == 0) return 0;
      return sw_random_u32() % upper;
  }
#endif

/* ============================================================
 * Socket Compatibility
 * ============================================================ */

#ifdef SW_OS_WINDOWS
  typedef SOCKET sw_fd_t;
  #define SW_INVALID_FD    INVALID_SOCKET
  #define sw_close_socket  closesocket
  #define sw_socket_read(fd, buf, len)   recv(fd, (char*)(buf), len, 0)
  #define sw_socket_write(fd, buf, len)  send(fd, (const char*)(buf), len, 0)
  #define sw_socket_errno  WSAGetLastError()
  #define SW_EAGAIN        WSAEWOULDBLOCK
  #define SW_EWOULDBLOCK   WSAEWOULDBLOCK

  static inline int sw_winsock_init(void) {
      WSADATA wsa;
      return WSAStartup(MAKEWORD(2, 2), &wsa);
  }
  static inline void sw_winsock_cleanup(void) {
      WSACleanup();
  }
#else
  typedef int sw_fd_t;
  #define SW_INVALID_FD    (-1)
  #define sw_close_socket  close
  #define sw_socket_read(fd, buf, len)   read(fd, buf, len)
  #define sw_socket_write(fd, buf, len)  write(fd, buf, len)
  #define sw_socket_errno  errno
  #define SW_EAGAIN        EAGAIN
  #define SW_EWOULDBLOCK   EWOULDBLOCK

  static inline int sw_winsock_init(void) { return 0; }
  static inline void sw_winsock_cleanup(void) {}
#endif

/* ============================================================
 * Nonblocking I/O
 * ============================================================ */

static inline void sw_set_nonblocking(sw_fd_t fd) {
#ifdef SW_OS_WINDOWS
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

/* ============================================================
 * Wake Pipe (pipe on POSIX, loopback socket pair on Windows)
 * ============================================================ */

#ifdef SW_OS_WINDOWS
static inline int sw_wake_pipe_create(sw_fd_t fds[2]) {
    /* Use a loopback TCP socket pair as pipe replacement */
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(listener);
        return -1;
    }

    int addrlen = sizeof(addr);
    if (getsockname(listener, (struct sockaddr *)&addr, &addrlen) == SOCKET_ERROR) {
        closesocket(listener);
        return -1;
    }

    if (listen(listener, 1) == SOCKET_ERROR) {
        closesocket(listener);
        return -1;
    }

    fds[1] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fds[1] == INVALID_SOCKET) {
        closesocket(listener);
        return -1;
    }

    if (connect(fds[1], (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(fds[1]);
        closesocket(listener);
        return -1;
    }

    fds[0] = accept(listener, NULL, NULL);
    closesocket(listener);
    if (fds[0] == INVALID_SOCKET) {
        closesocket(fds[1]);
        return -1;
    }

    return 0;
}
#define sw_wake_pipe_close(fds) do { closesocket(fds[0]); closesocket(fds[1]); } while(0)
#else
static inline int sw_wake_pipe_create(sw_fd_t fds[2]) {
    return pipe(fds);
}
#define sw_wake_pipe_close(fds) do { close(fds[0]); close(fds[1]); } while(0)
#endif

/* ============================================================
 * Memory Mapping (mmap vs VirtualAlloc)
 * ============================================================ */

#ifndef SW_OS_WINDOWS
#include <sys/mman.h>
#endif

static inline void *sw_mmap_anon(size_t size) {
#ifdef SW_OS_WINDOWS
    return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (p == MAP_FAILED) ? NULL : p;
#endif
}

static inline void sw_munmap(void *ptr, size_t size) {
#ifdef SW_OS_WINDOWS
    (void)size;
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    munmap(ptr, size);
#endif
}

/* ============================================================
 * Thread Yield
 * ============================================================ */

static inline void sw_thread_yield(void) {
#ifdef SW_OS_WINDOWS
    SwitchToThread();
#else
    sched_yield();
#endif
}

/* ============================================================
 * High-resolution Timer (for preemption)
 * ============================================================ */

#ifdef SW_OS_WINDOWS
typedef struct {
    HANDLE timer_queue;
    HANDLE timer;
} sw_preempt_timer_t;

typedef void (*sw_timer_callback_t)(void);

static sw_timer_callback_t g_sw_timer_cb = NULL;

static VOID CALLBACK sw_timer_callback_wrapper(PVOID param, BOOLEAN fired) {
    (void)param; (void)fired;
    if (g_sw_timer_cb) g_sw_timer_cb();
}

static inline int sw_preempt_timer_start(sw_preempt_timer_t *t, int interval_ms, sw_timer_callback_t cb) {
    g_sw_timer_cb = cb;
    t->timer_queue = CreateTimerQueue();
    if (!t->timer_queue) return -1;
    if (!CreateTimerQueueTimer(&t->timer, t->timer_queue,
            sw_timer_callback_wrapper, NULL, 0, interval_ms, WT_EXECUTEDEFAULT)) {
        DeleteTimerQueue(t->timer_queue);
        return -1;
    }
    return 0;
}

static inline void sw_preempt_timer_stop(sw_preempt_timer_t *t) {
    if (t->timer_queue) {
        DeleteTimerQueueTimer(t->timer_queue, t->timer, INVALID_HANDLE_VALUE);
        DeleteTimerQueue(t->timer_queue);
        t->timer_queue = NULL;
    }
}
#endif /* SW_OS_WINDOWS */

/* ============================================================
 * SIMD Headers
 * ============================================================ */

#if defined(SW_ARCH_ARM64) && !defined(SW_OS_WINDOWS)
  #include <arm_neon.h>
#elif defined(SW_ARCH_X86_64)
  #ifdef _MSC_VER
    #include <intrin.h>
  #else
    #include <immintrin.h>
  #endif
#endif

#endif /* SWARMRT_PLATFORM_H */
