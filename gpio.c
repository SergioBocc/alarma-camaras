/*
 * gpio.c — Control de GPIO con libgpiod
 *
 * Salida C (HW_GPIO_FRENTE_PIN): actúa con demora configurable en hilo separado
 * Salida F (HW_GPIO_FONDO_PIN):  actúa inmediatamente en hilo separado
 * Ambas son no bloqueantes para el loop principal.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <gpiod.h>
#include "alarm_processor.h"

#define GPIO_CHIP "/dev/gpiochip0"

static struct gpiod_chip *g_chip        = NULL;
static struct gpiod_line *g_line_frente = NULL;
static struct gpiod_line *g_line_fondo  = NULL;

/* ------------------------------------------------------------------ */
/* Hilos de pulso                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    struct gpiod_line *line;
    int                pulse_ms;
    int                delay_ms;
    pthread_mutex_t   *lock;
} PulseArgs;

static void *pulse_thread(void *arg) {
    PulseArgs *p = (PulseArgs *)arg;

    if (p->delay_ms > 0)
        usleep(p->delay_ms * 1000);

    pthread_mutex_lock(p->lock);
    gpiod_line_set_value(p->line, 1);
    usleep(p->pulse_ms * 1000);
    gpiod_line_set_value(p->line, 0);
    pthread_mutex_unlock(p->lock);

    free(p);
    return NULL;
}

static void fire_pulse(struct gpiod_line *line, int pulse_ms,
                        int delay_ms, pthread_mutex_t *lock,
                        const char *name) {
    PulseArgs *args = (PulseArgs *)malloc(sizeof(PulseArgs));
    if (!args) { log_msg(LOG_ERROR, "gpio: sin memoria para pulso %s", name); return; }

    args->line     = line;
    args->pulse_ms = pulse_ms;
    args->delay_ms = delay_ms;
    args->lock     = lock;

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&tid, &attr, pulse_thread, args);
    pthread_attr_destroy(&attr);

    if (delay_ms > 0)
        log_msg(LOG_INFO, "gpio: %s programado con demora %ds", name, delay_ms/1000);
    else
        log_msg(LOG_INFO, "gpio: %s disparado inmediatamente", name);
}

/* ------------------------------------------------------------------ */
/* API pública                                                          */
/* ------------------------------------------------------------------ */

int gpio_init(AppContext *ctx) {
    g_chip = gpiod_chip_open(GPIO_CHIP);
    if (!g_chip) {
        log_msg(LOG_ERROR, "gpio: no se pudo abrir %s", GPIO_CHIP);
        return -1;
    }

    /* Salida Frente */
    g_line_frente = gpiod_chip_get_line(g_chip, HW_GPIO_FRENTE_PIN);
    if (!g_line_frente ||
        gpiod_line_request_output(g_line_frente, "alarm_frente", 0) < 0) {
        log_msg(LOG_ERROR, "gpio: error configurando pin Frente %d", HW_GPIO_FRENTE_PIN);
        return -1;
    }

    /* Salida Fondo */
    g_line_fondo = gpiod_chip_get_line(g_chip, HW_GPIO_FONDO_PIN);
    if (!g_line_fondo ||
        gpiod_line_request_output(g_line_fondo, "alarm_fondo", 0) < 0) {
        log_msg(LOG_ERROR, "gpio: error configurando pin Fondo %d", HW_GPIO_FONDO_PIN);
        return -1;
    }

    log_msg(LOG_INFO, "gpio: Frente=pin%d  Fondo=pin%d  inicializados OK",
            HW_GPIO_FRENTE_PIN, HW_GPIO_FONDO_PIN);
    return 0;
}

void gpio_trigger_frente(AppContext *ctx) {
    if (!g_line_frente) { log_msg(LOG_ERROR, "gpio: línea Frente no inicializada"); return; }
    int pulse_ms = ctx->config.gpio_pulse_seconds * 1000;
    int delay_ms = ctx->config.gpio_demora_frente_seg * 1000;
    fire_pulse(g_line_frente, pulse_ms, delay_ms, &ctx->gpio_lock, "Frente");
}

void gpio_trigger_fondo(AppContext *ctx) {
    if (!g_line_fondo) { log_msg(LOG_ERROR, "gpio: línea Fondo no inicializada"); return; }
    int pulse_ms = ctx->config.gpio_pulse_seconds * 1000;
    fire_pulse(g_line_fondo, pulse_ms, 0, &ctx->gpio_lock, "Fondo");
}

void gpio_cleanup(AppContext *ctx) {
    (void)ctx;
    if (g_line_frente) { gpiod_line_set_value(g_line_frente, 0); gpiod_line_release(g_line_frente); g_line_frente = NULL; }
    if (g_line_fondo)  { gpiod_line_set_value(g_line_fondo,  0); gpiod_line_release(g_line_fondo);  g_line_fondo  = NULL; }
    if (g_chip)        { gpiod_chip_close(g_chip); g_chip = NULL; }
    log_msg(LOG_INFO, "gpio: recursos liberados");
}
