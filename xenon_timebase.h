/*
 * xenon_timebase.h
 * Sistema de timebase LLE para Xenon (Xbox 360 PPE)
 *
 * ╔═══════════════════════════════════════════════════════════════════════╗
 * ║  HARDWARE TARGET                                                      ║
 * ║  CPU  : Xenon — PowerPC PPE derivado de Cell, 3 cores / 6 threads    ║
 * ║  TBR  : compartido entre los 2 SMT threads de cada core              ║
 * ║  DEC  : propio de cada thread            (SPR 22,  EXC 0x900)        ║
 * ║  HDEC : propio de cada thread            (SPR 310, EXC 0x980)        ║
 * ║  freq : 49,875,000 Hz  (FSB 1596 MHz / 32)                           ║
 * ╚═══════════════════════════════════════════════════════════════════════╝
 *
 * MAP DE SPRs RELEVANTES
 * ──────────────────────
 *  SPR 268  TBL  mfspr / mftb  lectura lower-32 del TBR      ← por core
 *  SPR 269  TBU  mfspr / mftb  lectura upper-32 del TBR      ← por core
 *  SPR 284  TBL  mtspr         escritura lower-32 del TBR    ← por core
 *  SPR 285  TBU  mtspr         escritura upper-32 del TBR    ← por core
 *  SPR  22  DEC  mfspr/mtspr   decrementer                   ← por thread
 *  SPR 310  HDEC mfspr/mtspr   hypervisor decrementer        ← por thread
 *
 * VECTORES DE EXCEPCIÓN
 * ─────────────────────
 *  0x0900  Decrementer              requiere MSR[EE]=1
 *  0x0980  Hypervisor Decrementer   ignora  MSR[EE]
 *
 * FÓRMULAS EXACTAS
 * ────────────────
 *  guest_tb(t) = ⌊ host_ns(t) · fg / 10⁹ ⌋ + tb_offset
 *
 *  al escribir TBR = V en instante t₀:
 *    tb_offset = V − ⌊ host_ns(t₀) · fg / 10⁹ ⌋
 *
 *  deadline del DEC/HDEC al escribir valor V:
 *    ticks_to_fire = (uint64_t)V + 1         ← exacto para todo V en [0, 2³²-1]
 *    ns_to_fire    = ⌈ ticks_to_fire · 10⁹ / fg ⌉
 *    deadline_ns   = host_ns() + ns_to_fire
 *
 *  lectura lazy del DEC/HDEC:
 *    current = V − ⌊ (host_ns() − write_ns) · fg / 10⁹ ⌋   (uint32_t, wrap natural)
 *
 * NOTAS DE IMPLEMENTACIÓN
 * ────────────────────────
 *  · Toda la aritmética usa __uint128_t como intermedio (sin punto flotante).
 *  · tb_offset es _Atomic int64_t porque los 2 SMT threads del core pueden
 *    leer/escribir el TBR concurrentemente.
 *  · Los callbacks de timer se ejecutan en hilos del host ajenos al CPU loop;
 *    la entrega real de interrupts se hace a través de xenon_tb_check_irq(),
 *    que el emulador llama al final de cada bloque de traducción.
 */

#pragma once

#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * CONSTANTES XENON
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Frecuencia del timebase: FSB Xenon 1596 MHz / 32 = 49.875 MHz exacto */
#define XENON_TB_FREQ        UINT64_C(49875000)

/* Nanosegundos por segundo (base del host) */
#define XENON_NS_PER_SEC     UINT64_C(1000000000)

/* Topología */
#define XENON_NUM_CORES      3
#define XENON_THREADS_PER_CORE 2
#define XENON_TOTAL_THREADS  (XENON_NUM_CORES * XENON_THREADS_PER_CORE)

/* SPR numbers (PowerPC) */
#define XENON_SPR_TBL_R      268   /* mfspr / mftb — solo lectura */
#define XENON_SPR_TBU_R      269   /* mfspr / mftb — solo lectura */
#define XENON_SPR_TBL_W      284   /* mtspr — solo escritura */
#define XENON_SPR_TBU_W      285   /* mtspr — solo escritura */
#define XENON_SPR_DEC        22    /* mfspr / mtspr */
#define XENON_SPR_HDEC       310   /* mfspr / mtspr — hypervisor */

/* Vectores de excepción (offset en la tabla del guest) */
#define XENON_EXC_DECREMENTER   UINT32_C(0x0900)
#define XENON_EXC_HDECREMENTER  UINT32_C(0x0980)

/* ═══════════════════════════════════════════════════════════════════════════
 * ARITMÉTICA EXACTA — sin punto flotante, sin overflow
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * muldiv64_floor(a, b, c) = ⌊ a·b / c ⌋
 *
 * Usa __uint128_t para el producto intermedio.
 * No desborda mientras el resultado quepa en uint64_t.
 * Para nuestra conversión ns→ticks: a < 2⁶³, b = 49875000 < 2²⁶, c = 10⁹
 * → producto intermedio < 2⁸⁹, resultado < 2⁶³. Seguro.
 */
static inline uint64_t muldiv64_floor(uint64_t a, uint64_t b, uint64_t c)
{
    return (uint64_t)((__uint128_t)a * b / c);
}

/*
 * muldiv64_ceil(a, b, c) = ⌈ a·b / c ⌉
 *
 * Se usa para calcular ns_to_fire del DEC/HDEC.
 * El ceiling garantiza que el timer NO dispara antes de que el tick real ocurra.
 * Error máximo: 1 - 1/fg ns ≈ 20 ns @ 49.875 MHz — aceptable para un timer.
 */
static inline uint64_t muldiv64_ceil(uint64_t a, uint64_t b, uint64_t c)
{
    return (uint64_t)(((__uint128_t)a * b + c - 1u) / c);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * TIEMPO DEL HOST
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * xenon_host_ns() — nanosegundos del reloj monotónico del host.
 *
 * CLOCK_MONOTONIC es la única fuente correcta:
 *  · No retrocede (CLOCK_REALTIME puede con adjtime/NTP)
 *  · No salta (CLOCK_MONOTONIC_RAW no compensa deriva, pero es válido)
 *  · Resolución sub-microsegundo en Linux/macOS moderno
 *
 * En Windows: usar xenon_host_ns_windows() definida en xenon_timebase_platform.c
 */
#ifndef XENON_PLATFORM_WINDOWS
static inline uint64_t xenon_host_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * XENON_NS_PER_SEC + (uint64_t)ts.tv_nsec;
}
#else
uint64_t xenon_host_ns(void);  /* implementado en xenon_timebase_platform.c */
#endif

/*
 * Convierte host_ns a ticks del guest (fracción del timebase Xenon).
 * Se llama en cada lectura de TBR, DEC y HDEC — debe ser rápido.
 */
static inline uint64_t xenon_host_ticks(uint64_t host_ns)
{
    return muldiv64_floor(host_ns, XENON_TB_FREQ, XENON_NS_PER_SEC);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PLATAFORMA — implementar en xenon_timebase_platform.c
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Programa un one-shot timer que llama cb(arg) en el instante deadline_ns
 * (nanosegundos de CLOCK_MONOTONIC). Cancela el timer anterior si existe.
 * Guarda el handle en *out_handle.
 */
void xenon_platform_timer_set(void**   out_handle,
                              uint64_t deadline_ns,
                              void   (*cb)(void*),
                              void*    arg);

/*
 * Cancela el timer en *handle. Idempotente si *handle == NULL.
 * Asigna NULL a *handle tras cancelar.
 */
void xenon_platform_timer_cancel(void** handle);

/* ═══════════════════════════════════════════════════════════════════════════
 * ESTRUCTURAS
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * XenonDecState — estado de un decrementer (DEC o HDEC).
 *
 * Instanciado 2 veces por thread: una para DEC, otra para HDEC.
 */
typedef struct {
    /* Valor escrito por el guest. */
    uint32_t initial;

    /* Snapshot de xenon_host_ns() en el momento exacto de la escritura. */
    uint64_t write_host_ns;

    /* Handle del timer del host. NULL si no hay timer activo. */
    void* timer_handle;

    /*
     * Flag atómico: el timer disparó pero la excepción aún no fue entregada
     * (porque MSR[EE]=0 para DEC, o porque el CPU loop no llegó a
     * xenon_tb_check_irq() todavía).
     *
     * El emulador consulta este flag al final de cada TB y cuando MSR[EE]
     * cambia de 0 a 1.
     */
    atomic_bool pending;
} XenonDecState;

/*
 * XenonThreadTimebase — estado de timebase propio de un SMT thread.
 *
 * Un thread se identifica como (core_id, thread_id) donde
 * core_id ∈ {0,1,2} y thread_id ∈ {0,1}.
 */
typedef struct {
    uint8_t core_id;    /* 0..2 */
    uint8_t thread_id;  /* 0..1 */

    XenonDecState dec;   /* SPR 22  — Decrementer */
    XenonDecState hdec;  /* SPR 310 — Hypervisor Decrementer */

    /*
     * Puntero al estado del core padre.
     * Se usa para leer/escribir el TBR compartido.
     * Inicializado por xenon_tb_init().
     */
    struct XenonCoreTimebase* core;

    /* Callback para entregar excepciones al CPU loop del emulador. */
    void (*raise_exception)(uint32_t vector, void* cpu_ctx);
    void* cpu_ctx;
} XenonThreadTimebase;

/*
 * XenonCoreTimebase — estado de timebase compartido por los 2 SMT threads.
 *
 * Un solo tb_offset por core porque el hardware expone un TBR único
 * visible idénticamente desde ambos threads.
 *
 * Es _Atomic porque los 2 threads del core pueden correr en paralelo
 * (cada uno en su propio hilo del host) y ambos pueden ejecutar
 * mtspr TBL/TBU simultáneamente (condición de carrera real en HW también,
 * pero el guest no debería hacerlo sin sincronización explícita).
 */
typedef struct XenonCoreTimebase {
    uint8_t core_id;

    /*
     * Offset en ticks del guest tal que:
     *   guest_tb(t) = xenon_host_ticks(xenon_host_ns(t)) + tb_offset
     *
     * Firmado: puede ser negativo cuando el guest escribe un TBR pequeño
     * después de que el host lleva tiempo corriendo.
     */
    _Atomic int64_t tb_offset;

    /* Los 2 threads de este core. */
    XenonThreadTimebase threads[XENON_THREADS_PER_CORE];
} XenonCoreTimebase;

/*
 * XenonTimebase — estado global de todo el sistema de timebase.
 * Hay una única instancia por emulador.
 */
typedef struct {
    XenonCoreTimebase cores[XENON_NUM_CORES];
} XenonTimebase;

/* ═══════════════════════════════════════════════════════════════════════════
 * INICIALIZACIÓN
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * xenon_tb_init() — Inicializa el sistema de timebase.
 *
 * raise_fn[i] es el callback para el thread global i (0..5),
 * donde i = core_id * XENON_THREADS_PER_CORE + thread_id.
 * cpu_ctx[i] es el contexto opaco pasado al callback.
 *
 * El TBR de todos los cores arranca a 0 (tb_offset = -host_ticks_ahora).
 * El DEC/HDEC arrancan en 0xFFFFFFFF con timer no activo.
 */
static inline void xenon_tb_init(
    XenonTimebase* tb,
    void (*raise_fn[XENON_TOTAL_THREADS])(uint32_t vector, void* ctx),
    void* cpu_ctx[XENON_TOTAL_THREADS])
{
    uint64_t now = xenon_host_ns();
    int64_t  initial_offset = -(int64_t)xenon_host_ticks(now);

    for (int c = 0; c < XENON_NUM_CORES; c++) {
        XenonCoreTimebase* core = &tb->cores[c];
        core->core_id = (uint8_t)c;
        atomic_store_explicit(&core->tb_offset, initial_offset,
                              memory_order_relaxed);

        for (int t = 0; t < XENON_THREADS_PER_CORE; t++) {
            int global_idx = c * XENON_THREADS_PER_CORE + t;
            XenonThreadTimebase* th = &core->threads[t];

            th->core_id   = (uint8_t)c;
            th->thread_id = (uint8_t)t;
            th->core      = core;

            th->raise_exception = raise_fn[global_idx];
            th->cpu_ctx         = cpu_ctx[global_idx];

            /* DEC */
            th->dec.initial       = 0xFFFFFFFFu;
            th->dec.write_host_ns = now;
            th->dec.timer_handle  = NULL;
            atomic_store_explicit(&th->dec.pending, false,
                                  memory_order_relaxed);

            /* HDEC */
            th->hdec.initial       = 0xFFFFFFFFu;
            th->hdec.write_host_ns = now;
            th->hdec.timer_handle  = NULL;
            atomic_store_explicit(&th->hdec.pending, false,
                                  memory_order_relaxed);
        }
    }
}

static inline void xenon_tb_destroy(XenonTimebase* tb)
{
    for (int c = 0; c < XENON_NUM_CORES; c++) {
        for (int t = 0; t < XENON_THREADS_PER_CORE; t++) {
            XenonThreadTimebase* th = &tb->cores[c].threads[t];
            xenon_platform_timer_cancel(&th->dec.timer_handle);
            xenon_platform_timer_cancel(&th->hdec.timer_handle);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * TIMEBASE (TBR) — handlers de SPR 268, 269, 284, 285
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * xenon_tb_read() — Lee el TBR de 64 bits del core.
 *
 * Cálculo lazy; solo ocurre cuando el guest ejecuta mftb o mfspr 268/269.
 *
 *   guest_tb = ⌊ host_ns() · fg / 10⁹ ⌋ + tb_offset
 */
static inline uint64_t xenon_tb_read(const XenonCoreTimebase* core)
{
    int64_t offset = atomic_load_explicit(&core->tb_offset,
                                          memory_order_relaxed);
    return (uint64_t)((int64_t)xenon_host_ticks(xenon_host_ns()) + offset);
}

/*
 * xenon_tb_write() — El guest escribe un nuevo valor completo al TBR.
 *
 * Nunca llamar directamente desde los handlers mtspr 284/285;
 * usar xenon_spr_tbl_write() y xenon_spr_tbu_write() (ver más abajo).
 *
 *   tb_offset = new_value − ⌊ host_ns() · fg / 10⁹ ⌋
 *
 * Usa memory_order_release para que los loads posteriores del otro
 * SMT thread vean el valor actualizado.
 */
static inline void xenon_tb_write(XenonCoreTimebase* core, uint64_t new_value)
{
    int64_t new_offset = (int64_t)new_value
                       - (int64_t)xenon_host_ticks(xenon_host_ns());
    atomic_store_explicit(&core->tb_offset, new_offset,
                          memory_order_release);
}

/*
 * xenon_spr_tbu_write() — mtspr 285 (TBU write)
 *
 * Escribe solo el upper-32 del TBR preservando el lower-32 actual.
 *
 *   new_tb = ((uint64_t)val << 32) | (guest_tb_current & 0xFFFFFFFF)
 *
 * NOTA CRÍTICA: se hace una sola lectura del TBR actual y luego
 * una sola escritura, NO dos operaciones separadas, para minimizar
 * el skew temporal entre la lectura del lower y la escritura.
 * (No es posible hacerlo atómico en 64 bits sin instrucciones especiales
 * del host, pero la ventana de race es ~decenas de ns — tolerable.)
 */
static inline void xenon_spr_tbu_write(XenonCoreTimebase* core, uint32_t val)
{
    uint64_t cur_tb  = xenon_tb_read(core);
    uint64_t new_tb  = ((uint64_t)val << 32) | (cur_tb & UINT64_C(0xFFFFFFFF));
    xenon_tb_write(core, new_tb);
}

/*
 * xenon_spr_tbl_write() — mtspr 284 (TBL write)
 *
 * Escribe solo el lower-32 del TBR preservando el upper-32 actual.
 *
 *   new_tb = (guest_tb_current & 0xFFFFFFFF00000000) | (uint64_t)val
 */
static inline void xenon_spr_tbl_write(XenonCoreTimebase* core, uint32_t val)
{
    uint64_t cur_tb  = xenon_tb_read(core);
    uint64_t new_tb  = (cur_tb & UINT64_C(0xFFFFFFFF00000000)) | (uint64_t)val;
    xenon_tb_write(core, new_tb);
}

/* Helpers de lectura para los SPRs individuales */
static inline uint32_t xenon_spr_tbl_read(const XenonCoreTimebase* core)
{
    return (uint32_t)(xenon_tb_read(core) & UINT64_C(0xFFFFFFFF));
}

static inline uint32_t xenon_spr_tbu_read(const XenonCoreTimebase* core)
{
    return (uint32_t)(xenon_tb_read(core) >> 32);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * DEC / HDEC — lógica interna compartida
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Forward declarations de los callbacks de timer */
static void xenon_dec_timer_fired(void* arg);
static void xenon_hdec_timer_fired(void* arg);

/*
 * xenon_dec_schedule() — programa el timer del host para un decrementer.
 *
 * La excepción de DEC/HDEC se dispara cuando el bit 31 (MSB) del registro
 * transiciona de 0 a 1, es decir cuando el valor pasa de 0x00000000
 * a 0xFFFFFFFF (de 0 a -1 en signed).
 *
 * Para cualquier valor V escrito (unsigned, 32-bit):
 *   ticks_to_fire = (uint64_t)V + 1
 *
 * Demostración: DEC decrementa 1 tick por tick del TB. Empieza en V.
 *   Después de N ticks: DEC = (V - N) mod 2³²
 *   Condición de excepción: DEC == 0xFFFFFFFF
 *   → V - N ≡ 0xFFFFFFFF (mod 2³²)
 *   → N ≡ V - 0xFFFFFFFF ≡ V + 1 (mod 2³²)
 *   → primera ocurrencia: N = (uint32_t)(V + 1) si V < 0xFFFFFFFF,
 *                          N = 2³²              si V == 0xFFFFFFFF
 *   = (uint64_t)V + 1  en todos los casos con aritmética de 64 bits. ✓
 *
 * Usamos ceiling en la conversión ticks→ns para que el timer NUNCA
 * dispare antes de que el tick real haya ocurrido en el guest.
 */
static inline void xenon_dec_schedule(XenonDecState* dec,
                                      uint64_t       write_host_ns,
                                      void         (*cb)(void*),
                                      void*          arg)
{
    uint64_t ticks_to_fire = (uint64_t)dec->initial + 1u;
    uint64_t ns_to_fire    = muldiv64_ceil(ticks_to_fire,
                                           XENON_NS_PER_SEC,
                                           XENON_TB_FREQ);
    uint64_t deadline      = write_host_ns + ns_to_fire;
    xenon_platform_timer_set(&dec->timer_handle, deadline, cb, arg);
}

/*
 * xenon_dec_read_raw() — lectura lazy de un decrementer.
 *
 *   current = initial − ⌊ (host_ns() − write_host_ns) · fg / 10⁹ ⌋
 *
 * El resultado es uint32_t; la resta modular produce el valor correcto
 * incluso después de expirar (devuelve 0xFFFFFFFF, 0xFFFFFFFE, ...).
 */
static inline uint32_t xenon_dec_read_raw(const XenonDecState* dec)
{
    uint64_t elapsed_ns    = xenon_host_ns() - dec->write_host_ns;
    uint64_t elapsed_ticks = muldiv64_floor(elapsed_ns,
                                            XENON_TB_FREQ,
                                            XENON_NS_PER_SEC);
    /* La resta en uint32_t wrappea igual que el hardware real */
    return (uint32_t)((int64_t)dec->initial - (int64_t)elapsed_ticks);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * DEC — handlers de SPR 22
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * xenon_spr_dec_write() — mtspr 22 (DEC write)
 *
 * El Xenon hypervisor escribe DEC frecuentemente para preempción de threads.
 * Cada escritura:
 *  1. Cancela el timer anterior si lo hay.
 *  2. Guarda el nuevo valor y el timestamp exacto.
 *  3. Programa un nuevo timer en el host.
 */
static inline void xenon_spr_dec_write(XenonThreadTimebase* th, uint32_t val)
{
    xenon_platform_timer_cancel(&th->dec.timer_handle);
    atomic_store_explicit(&th->dec.pending, false, memory_order_relaxed);

    uint64_t now         = xenon_host_ns();
    th->dec.initial      = val;
    th->dec.write_host_ns = now;

    xenon_dec_schedule(&th->dec, now, xenon_dec_timer_fired, th);
}

/*
 * xenon_spr_dec_read() — mfspr 22 (DEC read)
 */
static inline uint32_t xenon_spr_dec_read(const XenonThreadTimebase* th)
{
    return xenon_dec_read_raw(&th->dec);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * HDEC — handlers de SPR 310
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * xenon_spr_hdec_write() — mtspr 310 (HDEC write)
 *
 * Idéntico a DEC write en mecánica, distinto en:
 *  · Vector de excepción: 0x0980
 *  · No requiere MSR[EE]=1 para dispararse (es hypervisor-level)
 *  · Solo accessible en hypervisor mode (HV=1 en MSR)
 */
static inline void xenon_spr_hdec_write(XenonThreadTimebase* th, uint32_t val)
{
    xenon_platform_timer_cancel(&th->hdec.timer_handle);
    atomic_store_explicit(&th->hdec.pending, false, memory_order_relaxed);

    uint64_t now          = xenon_host_ns();
    th->hdec.initial      = val;
    th->hdec.write_host_ns = now;

    xenon_dec_schedule(&th->hdec, now, xenon_hdec_timer_fired, th);
}

/*
 * xenon_spr_hdec_read() — mfspr 310 (HDEC read)
 */
static inline uint32_t xenon_spr_hdec_read(const XenonThreadTimebase* th)
{
    return xenon_dec_read_raw(&th->hdec);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * CALLBACKS DE TIMER
 *
 * Se ejecutan en el hilo del timer del host (ajeno al CPU loop).
 * NUNCA entregan la excepción directamente; solo marcan el flag pending.
 * La entrega real ocurre en xenon_tb_check_irq(), llamado por el CPU loop.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void xenon_dec_timer_fired(void* arg)
{
    XenonThreadTimebase* th = (XenonThreadTimebase*)arg;
    atomic_store_explicit(&th->dec.pending, true, memory_order_release);
    /*
     * En un emulador con CPU loop bloqueado en un wait (p.ej. WFI emulado),
     * aquí se añadiría un kick al loop: basta con un write a un eventfd o
     * equivalente para despertar al hilo del CPU.
     */
}

static void xenon_hdec_timer_fired(void* arg)
{
    XenonThreadTimebase* th = (XenonThreadTimebase*)arg;
    atomic_store_explicit(&th->hdec.pending, true, memory_order_release);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * ENTREGA DE INTERRUPTS
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * xenon_tb_check_irq() — verifica y entrega interrupts pendientes.
 *
 * El emulador DEBE llamar esta función:
 *  · Al final de cada Translation Block (TB) ejecutado.
 *  · Cuando MSR[EE] cambia de 0 a 1 (msr_ee_enabled=true en ese caso).
 *  · Cuando MSR[HV] cambia a 1 (para HDEC).
 *
 * Parámetros:
 *   th          — thread actual
 *   msr_ee      — valor actual de MSR[EE] (External Enable)
 *   msr_hv      — valor actual de MSR[HV] (Hypervisor mode)
 *
 * Prioridad: HDEC > DEC (igual que el hardware real).
 *
 * Retorna true si entregó algún interrupt (el CPU loop debe redespachar).
 */
static inline bool xenon_tb_check_irq(XenonThreadTimebase* th,
                                      bool msr_ee,
                                      bool msr_hv)
{
    bool delivered = false;

    /*
     * HDEC — Hypervisor Decrementer
     * Dispara si pending Y el thread está en hypervisor mode (MSR[HV]=1).
     * No requiere MSR[EE].
     */
    if (msr_hv &&
        atomic_load_explicit(&th->hdec.pending, memory_order_acquire))
    {
        atomic_store_explicit(&th->hdec.pending, false, memory_order_relaxed);
        if (th->raise_exception) {
            th->raise_exception(XENON_EXC_HDECREMENTER, th->cpu_ctx);
        }
        delivered = true;
    }

    /*
     * DEC — Decrementer
     * Dispara si pending Y MSR[EE]=1.
     * Si MSR[EE]=0, el pending permanece hasta que EE vuelva a 1.
     */
    if (!delivered && msr_ee &&
        atomic_load_explicit(&th->dec.pending, memory_order_acquire))
    {
        atomic_store_explicit(&th->dec.pending, false, memory_order_relaxed);
        if (th->raise_exception) {
            th->raise_exception(XENON_EXC_DECREMENTER, th->cpu_ctx);
        }
        delivered = true;
    }

    return delivered;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * DESPACHO DE SPRs — integración directa con el decoder del emulador
 *
 * Estas funciones mapean 1-a-1 con los casos del switch en el handler
 * de mfspr/mtspr del emulador. Reciben el XenonThreadTimebase* del
 * thread actual, que ya tiene el puntero al core padre.
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * xenon_mfspr(th, spr) — despacha una instrucción mfspr.
 *
 * Uso en el emulador:
 *   uint32_t val = xenon_mfspr(th, spr_field_from_instr);
 *   GPR[RT] = (uint64_t)val; // SPRs son 32-bit en el PPE
 *
 * Para TBL/TBU la convención es leer el campo de 32 bits correspondiente.
 */
static inline uint32_t xenon_mfspr(const XenonThreadTimebase* th, uint32_t spr)
{
    switch (spr) {
    case XENON_SPR_TBL_R:  return xenon_spr_tbl_read(th->core);
    case XENON_SPR_TBU_R:  return xenon_spr_tbu_read(th->core);
    case XENON_SPR_DEC:    return xenon_spr_dec_read(th);
    case XENON_SPR_HDEC:   return xenon_spr_hdec_read(th);
    default:               return 0; /* SPR no relacionado con timebase */
    }
}

/*
 * xenon_mtspr(th, spr, val) — despacha una instrucción mtspr.
 *
 * Uso en el emulador:
 *   xenon_mtspr(th, spr_field_from_instr, (uint32_t)GPR[RS]);
 *
 * Retorna false si el SPR no es del dominio de timebase (el emulador
 * debe manejar el resto).
 */
static inline bool xenon_mtspr(XenonThreadTimebase* th,
                                uint32_t spr,
                                uint32_t val)
{
    switch (spr) {
    case XENON_SPR_TBL_W:  xenon_spr_tbl_write(th->core, val);  return true;
    case XENON_SPR_TBU_W:  xenon_spr_tbu_write(th->core, val);  return true;
    case XENON_SPR_DEC:    xenon_spr_dec_write(th, val);         return true;
    case XENON_SPR_HDEC:   xenon_spr_hdec_write(th, val);        return true;
    default:               return false;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * HELPER — índice global de thread
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline int xenon_thread_global_idx(uint8_t core_id, uint8_t thread_id)
{
    return (int)core_id * XENON_THREADS_PER_CORE + (int)thread_id;
}

static inline XenonThreadTimebase* xenon_get_thread(XenonTimebase* tb,
                                                     uint8_t core_id,
                                                     uint8_t thread_id)
{
    return &tb->cores[core_id].threads[thread_id];
}

#ifdef __cplusplus
}
#endif
