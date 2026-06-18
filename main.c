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
        /* Chequear período de gracia en cada email */
       log_msg(LOG_INFO, "DBG: state=%d ts=%ld diff=%.0f demora=%d",
                alarm_get_state(ctx), ctx->arm_timestamp,
                difftime(time(NULL), ctx->arm_timestamp),
                ctx->config.demora_armado_seg);
        if (alarm_get_state(ctx) == ALARM_ARMED &&
            ctx->arm_timestamp > 0 &&
            difftime(time(NULL), ctx->arm_timestamp) < ctx->config.demora_armado_seg) {
            log_msg(LOG_INFO, "main: periodo de gracia → marcando todo como leído");
            imap_mark_all_read(ctx);
            for (int j = i; j < count; j++)
                email_free_images(&emails[j]);
            for (int j = 0; j < ctx->num_areas; j++)
                burst_reset_area(ctx, j);
            return;
        }
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
    Email *emails = (Email *)malloc(MAX_EMAILS_BATCH * sizeof(Email));
    if (!emails) {
        log_msg(LOG_ERROR, "main: sin memoria para emails");
        return;
    }

    log_msg(LOG_INFO, "main: iniciando ciclo  (intervalo minimo=%ds)",
            ctx->config.intervalo_minimo_seg);

    while (g_running) {
        struct timespec t_inicio, t_fin;
        clock_gettime(CLOCK_MONOTONIC, &t_inicio);
        /* 1. Período de gracia post-armado */
        if (ctx->gracia_activa) {
            log_msg(LOG_INFO, "main: periodo de gracia — suspendiendo %ds",
                    ctx->config.demora_armado_seg);
            sleep(ctx->config.demora_armado_seg);
            log_msg(LOG_INFO, "main: periodo de gracia — reseteando buffers");
            for (int i = 0; i < ctx->num_areas; i++)
                burst_reset_area(ctx, i);
            log_msg(LOG_INFO, "main: periodo de gracia — marcando todo como leído");
            imap_mark_all_read(ctx);
            ctx->gracia_activa = 0;
            log_msg(LOG_INFO, "main: periodo de gracia completado — retomando operación");
        } else {
            /* 2. Consultar buzón IMAP y procesar */
            int count = imap_fetch_unread(ctx, emails, MAX_EMAILS_BATCH);

            if (count < 0) {
                log_msg(LOG_ERROR, "main: error consultando IMAP");
            } else if (count > 0) {
                log_msg(LOG_INFO, "main: %d correos clasificados", count);
                process_emails(ctx, emails, count);
            }
        }
        /* 3. Verificar timeouts de ventanas */
        burst_check_timeouts(ctx);

        /* 4. Calcular tiempo transcurrido y dormir el resto */
        clock_gettime(CLOCK_MONOTONIC, &t_fin);
        long elapsed_ms = (t_fin.tv_sec - t_inicio.tv_sec) * 1000 +
                          (t_fin.tv_nsec - t_inicio.tv_nsec) / 1000000;
        long intervalo_ms = ctx->config.intervalo_minimo_seg * 1000L;

        if (elapsed_ms < intervalo_ms && g_running) {
            long sleep_ms = intervalo_ms - elapsed_ms;
            log_msg(LOG_DEBUG, "main: procesado en %ldms, durmiendo %ldms",
                    elapsed_ms, sleep_ms);
            usleep((useconds_t)(sleep_ms * 1000));
        } else {
            log_msg(LOG_DEBUG, "main: procesado en %ldms, consulta inmediata",
                    elapsed_ms);
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
