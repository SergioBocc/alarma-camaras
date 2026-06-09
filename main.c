/*
 * main.c — Ciclo principal del procesador de alarmas
 *
 * Uso:
 *   ./alarm_processor [config.ini]
 *   (por defecto busca config.ini en el directorio actual)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <curl/curl.h>
#include "alarm_processor.h"

/* ------------------------------------------------------------------ */
/* Señales                                                              */
/* ------------------------------------------------------------------ */

static volatile int g_running = 1;

static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* ------------------------------------------------------------------ */
/* Liberación de imágenes de un Email                                   */
/* ------------------------------------------------------------------ */

static void email_free_images(Email *e) {
    for (int i = 0; i < e->num_images; i++) {
        free(e->images[i].data);
        e->images[i].data = NULL;
        e->images[i].size = 0;
    }
    e->num_images = 0;
}

/* ------------------------------------------------------------------ */
/* Procesamiento de un lote de correos                                  */
/* ------------------------------------------------------------------ */

static void process_emails(AppContext *ctx, Email *emails, int count) {
    for (int i = 0; i < count; i++) {
        Email *e = &emails[i];

        switch (e->type) {

        case EMAIL_ALARM_IMMEDIATE:
            log_msg(LOG_WARNING,
                    "main: ALARMA INMEDIATA  asunto='%s'  uid=%s → GPIO",
                    e->subject, e->uid);
            gpio_trigger_frente(ctx);
            gpio_trigger_fondo(ctx);
            /* Las alarmas inmediatas NO se marcan como leídas
               (no son del sistema de cámaras de detección) */
            break;

        case EMAIL_DETECTION:
            log_msg(LOG_INFO,
                    "main: detección  camara='%s'  imágenes=%d  uid=%s",
                    e->camera_idx >= 0
                        ? ctx->config.camera_name[e->camera_idx]
                        : "desconocida",
                    e->num_images,
                    e->uid,
                    e->date);

            burst_process_email(ctx, e);

            /* Marcar como leído — solo correos del sistema de cámaras */
            imap_mark_read(ctx, e->uid);
            break;

        case EMAIL_DISCARD:
        default:
            /* No hacer nada */
            break;
        }

        email_free_images(e);
    }
}

/* ------------------------------------------------------------------ */
/* Ciclo principal                                                       */
/* ------------------------------------------------------------------ */

static void run_loop(AppContext *ctx) {
    //Email emails[MAX_EMAILS_BATCH];
    Email *emails = (Email *)malloc(MAX_EMAILS_BATCH * sizeof(Email));
    if (!emails) {
        log_msg(LOG_ERROR, "main: sin memoria para emails");
        return;
    }
    log_msg(LOG_INFO, "main: iniciando ciclo  (intervalo=%ds)",
            ctx->config.cycle_seconds);

    while (g_running) {
        /* 1. Consultar buzón IMAP */
        int count = imap_fetch_unread(ctx, emails, MAX_EMAILS_BATCH);

        if (count < 0) {
            log_msg(LOG_ERROR, "main: error consultando IMAP, reintentando en %ds",
                    ctx->config.cycle_seconds);
        } else if (count > 0) {
            log_msg(LOG_INFO, "main: %d correos clasificados", count);
            process_emails(ctx, emails, count);
        }

        /* 2. Verificar timeouts de ventanas de ráfaga */
        burst_check_timeouts(ctx);

        /* 3. Esperar hasta el próximo ciclo */
        if (g_running) {
            sleep((unsigned int)ctx->config.cycle_seconds);
        }
    }
    free(emails);
    log_msg(LOG_INFO, "main: ciclo detenido");
}

/* ------------------------------------------------------------------ */
/* Inicialización del contexto                                          */
/* ------------------------------------------------------------------ */

static int app_init(AppContext *ctx, const char *config_path) {
    memset(ctx, 0, sizeof(AppContext));

    /* Cargar configuración */
    if (config_load(config_path, &ctx->config) < 0)
        return -1;

    /* Inicializar log */
    log_init(&ctx->config);
    config_print(&ctx->config);

    /* Mutex GPIO */
    pthread_mutex_init(&ctx->gpio_lock, NULL);
    pthread_mutex_init(&ctx->alarm_state_lock, NULL);
    /* libcurl global */
    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* GPIO */
    if (gpio_init(ctx) < 0) {
        log_msg(LOG_ERROR, "main: fallo al inicializar GPIO");
        return -1;
    }

    /* YOLO */
    if (yolo_init(ctx) < 0) {
        log_msg(LOG_ERROR, "main: fallo al inicializar YOLO");
        gpio_cleanup(ctx);
        return -1;
    }
    
    /* MQTT alarma */
    if (mqtt_alarm_init(ctx) < 0) {
        log_msg(LOG_ERROR, "main: fallo al inicializar MQTT alarma");
        yolo_cleanup(ctx);
        gpio_cleanup(ctx);
        return -1;
    }
    ctx->running = 1;
    return 0;
}

static void app_cleanup(AppContext *ctx) {
    mqtt_alarm_cleanup(ctx);
    yolo_cleanup(ctx);
    gpio_cleanup(ctx);
    curl_global_cleanup();

    /* Destruir mutexes de buffers */
    for (int i = 0; i < ctx->num_areas; i++)
        pthread_mutex_destroy(&ctx->buffers[i].lock);
    pthread_mutex_destroy(&ctx->gpio_lock);
    pthread_mutex_destroy(&ctx->alarm_state_lock);
    log_msg(LOG_INFO, "main: limpieza completa");
    log_close();
}

/* ------------------------------------------------------------------ */
/* Entrada                                                              */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
    const char *config_path = (argc > 1) ? argv[1] : "config.ini";

    /* Señales de terminación */
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    AppContext ctx;
    if (app_init(&ctx, config_path) < 0) {
        fprintf(stderr, "Error al inicializar el sistema\n");
        return EXIT_FAILURE;
    }

    run_loop(&ctx);

    app_cleanup(&ctx);
    return EXIT_SUCCESS;
}
