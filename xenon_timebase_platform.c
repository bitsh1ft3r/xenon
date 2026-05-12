/*
 * xenon_timebase_platform.c
 * Implementaciones de platform_timer_set / cancel para Linux y Windows.
 *
 * Compilar con -DXENON_PLATFORM_LINUX   (timerfd + pthread)
 *          o  -DXENON_PLATFORM_WINDOWS  (WaitableTimer + thread pool)
 *
 * El objetivo de resolución es ≤1 µs de error en el deadline.
 * En Linux con HRTIMER configurado: resolución real ~100–500 ns.
 * En Windows con timeBeginPeriod(1): resolución ~0.5–1 ms (peor).
 */

#include "xenon_timebase.h"
#include <stdlib.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * LINUX — timerfd_create + hilo dedicado por timer
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Estrategia:
 *   · Cada llamada a xenon_platform_timer_set() crea un timerfd con
 *     TFD_TIMER_ABSTIME apuntando al deadline CLOCK_MONOTONIC.
 *   · Un hilo de bajo overhead espera en read() sobre ese fd.
 *   · Cuando read() retorna, llama al callback y se termina.
 *   · Si se cancela antes, el hilo ve cancelled=1 y sale sin llamar al cb.
 *
 * Alternativa de mayor rendimiento para el futuro:
 *   Un único hilo de timers con un min-heap de deadlines y epoll,
 *   en lugar de un hilo por timer. Con 6 threads × 2 decrementadores = 12
 *   timers concurrentes como máximo, el overhead actual es tolerable.
 */

#ifdef XENON_PLATFORM_LINUX

#include <sys/timerfd.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>

typedef struct {
    int              fd;
    pthread_t        thread;
    void           (*cb)(void*);
    void*            arg;
    volatile int     cancelled;
} XenonTimerHandle;

static void* xenon_timer_thread(void* p)
{
    XenonTimerHandle* h = (XenonTimerHandle*)p;

    uint64_t expirations;
    ssize_t  n;

    do {
        n = read(h->fd, &expirations, sizeof(expirations));
    } while (n < 0 && errno == EINTR);

    if (n > 0 && !h->cancelled) {
        h->cb(h->arg);
    }

    close(h->fd);
    free(h);
    return NULL;
}

void xenon_platform_timer_set(void**   out_handle,
                              uint64_t deadline_ns,
                              void   (*cb)(void*),
                              void*    arg)
{
    /* Cancela el timer anterior si existe */
    xenon_platform_timer_cancel(out_handle);

    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0) return;

    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec  = (time_t)(deadline_ns / XENON_NS_PER_SEC);
    its.it_value.tv_nsec = (long)  (deadline_ns % XENON_NS_PER_SEC);

    /*
     * TFD_TIMER_ABSTIME: deadline_ns es absoluto en CLOCK_MONOTONIC.
     * Si el deadline ya pasó, timerfd dispara inmediatamente.
     */
    if (timerfd_settime(fd, TFD_TIMER_ABSTIME, &its, NULL) < 0) {
        close(fd);
        return;
    }

    XenonTimerHandle* h = (XenonTimerHandle*)malloc(sizeof(*h));
    if (!h) { close(fd); return; }

    h->fd        = fd;
    h->cb        = cb;
    h->arg       = arg;
    h->cancelled = 0;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    /*
     * Elevamos la prioridad del hilo de timer para reducir latencia.
     * Requiere CAP_SYS_NICE o ejecutar como root; si falla, continúa
     * con prioridad normal.
     */
    struct sched_param sp = { .sched_priority = 10 };
    pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
    pthread_attr_setschedparam(&attr, &sp);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);

    if (pthread_create(&h->thread, &attr, xenon_timer_thread, h) != 0) {
        /* Fallback sin SCHED_FIFO */
        pthread_attr_setschedpolicy(&attr, SCHED_OTHER);
        if (pthread_create(&h->thread, &attr, xenon_timer_thread, h) != 0) {
            close(fd);
            free(h);
            pthread_attr_destroy(&attr);
            return;
        }
    }
    pthread_attr_destroy(&attr);

    *out_handle = h;
}

void xenon_platform_timer_cancel(void** handle)
{
    if (!handle || !*handle) return;

    XenonTimerHandle* h = (XenonTimerHandle*)*handle;
    h->cancelled = 1;

    /*
     * Disparar el fd inmediatamente para despertar al hilo bloqueado en read().
     * Ponemos it_value = 1 ns (no puede ser 0, eso desactiva el timer).
     */
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_nsec = 1;
    timerfd_settime(h->fd, 0, &its, NULL); /* relativo, dispara en 1 ns */

    *handle = NULL;
    /* El hilo detecta cancelled=1 y libera h y el fd. */
}

/* host_ns ya está definido en el header como inline para Linux */

#endif /* XENON_PLATFORM_LINUX */


/* ═══════════════════════════════════════════════════════════════════════════
 * WINDOWS — CreateWaitableTimerEx + thread pool
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Usamos CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Win10 1803+) para
 * resolución sub-milisegundo sin necesidad de timeBeginPeriod.
 *
 * SetWaitableTimerEx acepta tiempos en unidades de 100 ns. Negativos =
 * relativo, positivos = absoluto en tiempo de archivo (epoch 1601).
 * Convertimos desde nuestro CLOCK_MONOTONIC (base = QPC).
 */

#ifdef XENON_PLATFORM_WINDOWS

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef struct {
    HANDLE   timer;
    HANDLE   thread;
    void   (*cb)(void*);
    void*    arg;
    volatile LONG cancelled;
} XenonTimerHandle;

static DWORD WINAPI xenon_timer_thread_win(LPVOID p)
{
    XenonTimerHandle* h = (XenonTimerHandle*)p;

    WaitForSingleObject(h->timer, INFINITE);

    if (!InterlockedOr(&h->cancelled, 0)) {
        h->cb(h->arg);
    }

    CloseHandle(h->timer);
    free(h);
    return 0;
}

/*
 * Convierte nanosegundos de QPC (nuestra base monotónica) a FILETIME
 * (100-ns desde 1601-01-01) para SetWaitableTimer.
 *
 * QPC epoch ≠ FILETIME epoch. Necesitamos la diferencia:
 *   filetime_at_qpc_zero = GetSystemTimeAsFileTime() - qpc_ns_elapsed * 100
 */
static LARGE_INTEGER xenon_ns_to_filetime(uint64_t deadline_ns)
{
    /* Obtenemos FILETIME actual y QPC actual en la misma ventana */
    FILETIME    ft_now;
    LARGE_INTEGER qpc_now;
    GetSystemTimeAsFileTime(&ft_now);
    QueryPerformanceCounter(&qpc_now);

    LARGE_INTEGER qpc_freq;
    QueryPerformanceFrequency(&qpc_freq);

    /* FILETIME actual como uint64_t en unidades 100-ns */
    uint64_t ft_now64 = ((uint64_t)ft_now.dwHighDateTime << 32)
                      |  (uint64_t)ft_now.dwLowDateTime;

    /* QPC actual en ns (usando muldiv para evitar overflow) */
    uint64_t qpc_ns_now = muldiv64_floor((uint64_t)qpc_now.QuadPart,
                                          XENON_NS_PER_SEC,
                                          (uint64_t)qpc_freq.QuadPart);

    /*
     * deadline_ns es relativo a la misma base que qpc_ns_now.
     * Diferencia en ns: delta_ns = deadline_ns - qpc_ns_now
     * Diferencia en 100-ns: delta_100ns = delta_ns / 100
     * FILETIME del deadline: ft_now64 + delta_100ns
     */
    int64_t  delta_ns   = (int64_t)deadline_ns - (int64_t)qpc_ns_now;
    int64_t  delta_100  = delta_ns / 100;
    uint64_t ft_deadline = ft_now64 + (uint64_t)delta_100;

    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)ft_deadline;
    return li;
}

void xenon_platform_timer_set(void**   out_handle,
                              uint64_t deadline_ns,
                              void   (*cb)(void*),
                              void*    arg)
{
    xenon_platform_timer_cancel(out_handle);

    /*
     * CREATE_WAITABLE_TIMER_HIGH_RESOLUTION requiere Win10 1803+.
     * Fallback a CreateWaitableTimer si no está disponible.
     */
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) {
        timer = CreateWaitableTimerW(NULL, FALSE, NULL);
        if (!timer) return;
    }

    LARGE_INTEGER due = xenon_ns_to_filetime(deadline_ns);
    /* lPeriod=0: one-shot */
    SetWaitableTimerEx(timer, &due, 0, NULL, NULL, NULL, 0);

    XenonTimerHandle* h = (XenonTimerHandle*)malloc(sizeof(*h));
    if (!h) { CloseHandle(timer); return; }

    h->timer     = timer;
    h->cb        = cb;
    h->arg       = arg;
    h->cancelled = 0;

    h->thread = CreateThread(NULL, 0, xenon_timer_thread_win, h, 0, NULL);
    if (!h->thread) { CloseHandle(timer); free(h); return; }

    /* Elevamos la prioridad del hilo de timer */
    SetThreadPriority(h->thread, THREAD_PRIORITY_TIME_CRITICAL);
    CloseHandle(h->thread); /* el hilo es self-cleaning via free(h) al salir */

    *out_handle = h;
}

void xenon_platform_timer_cancel(void** handle)
{
    if (!handle || !*handle) return;
    XenonTimerHandle* h = (XenonTimerHandle*)*handle;
    InterlockedExchange(&h->cancelled, 1);
    /* Forzar que el timer dispare ya para despertar el hilo de espera */
    LARGE_INTEGER due = { .QuadPart = -1LL }; /* -100 ns = inmediato */
    SetWaitableTimerEx(h->timer, &due, 0, NULL, NULL, NULL, 0);
    *handle = NULL;
}

/* host_ns en Windows: QueryPerformanceCounter convertido a ns */
uint64_t xenon_host_ns(void)
{
    static LARGE_INTEGER freq = {0};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER count;
    QueryPerformanceCounter(&count);
    return muldiv64_floor((uint64_t)count.QuadPart,
                           XENON_NS_PER_SEC,
                           (uint64_t)freq.QuadPart);
}

#endif /* XENON_PLATFORM_WINDOWS */
