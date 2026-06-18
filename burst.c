/*
 * burst.c — Lógica del buffer de ráfaga por área
 *
 * Reglas:
 *   - Primer correo de detección de un área abre la ventana y arranca timer T
 *   - Cada imagen del correo se analiza con YOLO inmediatamente
 *   - Si persona detectada → incrementar contador positivo del área
 *   - Si contador >= umbral_detecciones → GPIO + cerrar ventana
 *   - Si T vence sin llegar al umbral → descartar + reset
 *   - burst_check_timeouts() se llama en cada ciclo del loop principal
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "alarm_processor.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

/* Obtiene o crea el índice de área para un nombre dado */
int burst_get_or_create_area(AppContext *ctx, const char *area_name) {
    /* Buscar existente */
    for (int i = 0; i < ctx->num_areas; i++) {
        if (strcmp(ctx->area_names[i], area_name) == 0)
            return i;
    }

    /* Crear nuevo */
    if (ctx->num_areas >= MAX_AREAS) {
        log_msg(LOG_ERROR, "burst: máximo de áreas alcanzado (%d)", MAX_AREAS);
        return -1;
    }

    int idx = ctx->num_areas++;
    strncpy(ctx->area_names[idx], area_name, MAX_STR - 1);
    BurstBuffer *buf = &ctx->buffers[idx];
    memset(buf, 0, sizeof(BurstBuffer));
    buf->area_idx = idx;
    buf->active   = 0;
    pthread_mutex_init(&buf->lock, NULL);

    log_msg(LOG_DEBUG, "burst: nueva área '%s' → idx=%d", area_name, idx);
    return idx;
}

static void burst_reset(BurstBuffer *buf, const char *area_name) {
    pthread_mutex_lock(&buf->lock);
    buf->active          = 0;
    buf->window_open     = 0;
    buf->positive_count  = 0;
    buf->total_images    = 0;
    pthread_mutex_unlock(&buf->lock);
    log_msg(LOG_DEBUG, "burst: reset área '%s'", area_name);
}

void burst_reset_area(AppContext *ctx, int area_idx) {
    if (area_idx < 0 || area_idx >= ctx->num_areas) return;
    BurstBuffer *buf = &ctx->buffers[area_idx];
    burst_reset(buf, ctx->area_names[area_idx]);
}

static void burst_open_window(BurstBuffer *buf, const char *area_name) {
    pthread_mutex_lock(&buf->lock);
    buf->active         = 1;
    buf->window_open    = time(NULL);
    buf->positive_count = 0;
    buf->total_images   = 0;
    pthread_mutex_unlock(&buf->lock);
    log_msg(LOG_INFO, "burst: ventana ABIERTA para área '%s'", area_name);
}

/* ------------------------------------------------------------------ */
/* Procesamiento de un correo de detección                              */
/* ------------------------------------------------------------------ */

int burst_process_email(AppContext *ctx, const Email *email) {
    if (email->type != EMAIL_DETECTION) return 0;

    /* Determinar área */
    const char *area_name = "Desconocida";
    if (email->camera_idx >= 0 && email->camera_idx < ctx->config.num_cameras)
        area_name = ctx->config.camera_area[email->camera_idx];

    int area_idx = burst_get_or_create_area(ctx, area_name);
    if (area_idx < 0) return -1;

    BurstBuffer *buf = &ctx->buffers[area_idx];
    
    /* Imagen de mayor confianza para el log de detecciones */
    const uint8_t *best_img_data = NULL;
    size_t         best_img_size = 0;
    float          best_conf     = 0.0f;
    const char    *best_camara   = (email->camera_idx >= 0) ?
                                    ctx->config.camera_name[email->camera_idx] :
                                    "desconocida";
    
    /* Si la ventana no está abierta, abrirla */
    if (!buf->active) {
        burst_open_window(buf, area_name);
    }

    /* Verificar timeout antes de procesar */
    time_t now     = time(NULL);
    double elapsed = difftime(now, buf->window_open);
    double timeout = ctx->config.window_minutes * 60.0;

    if (elapsed > timeout) {
        log_msg(LOG_INFO,
                "burst: ventana expiró para área '%s' (%.0fs > %.0fs) → descarte",
                area_name, elapsed, timeout);
        burst_reset(buf, area_name);
        /* Re-abrir para este correo */
        burst_open_window(buf, area_name);
    }

    /* Analizar cada imagen con YOLO */
    for (int i = 0; i < email->num_images; i++) {
        YoloResult result;
        int ret = yolo_analyze(ctx, &email->images[i], &result);

        pthread_mutex_lock(&buf->lock);
        buf->total_images++;

        if (ret == 0 && result.person_detected) {
            buf->positive_count++;
            log_msg(LOG_INFO,
                    "burst: área='%s'  imagen %d/%d  ✓ persona  conf=%.2f  "
                    "positivas=%d/%d",
                    area_name,
                    buf->total_images, email->num_images,
                    result.max_confidence,
                    buf->positive_count,
                    ctx->config.detection_threshold);
                    if (result.max_confidence > best_conf) {
                        best_conf     = result.max_confidence;
                        best_img_data = email->images[i].data;
                        best_img_size = email->images[i].size;
                    }
        } else {
            log_msg(LOG_DEBUG,
                    "burst: área='%s'  imagen %d/%d  sin persona",
                    area_name,
                    buf->total_images, email->num_images);
        }

        int should_trigger = (buf->positive_count >= ctx->config.detection_threshold);
        pthread_mutex_unlock(&buf->lock);

        if (should_trigger) {
            if (alarm_get_state(ctx) == ALARM_ARMED) {
                if (strcmp(area_name, "Frente") == 0) {
                    log_msg(LOG_WARNING,
                            "burst: ¡DETECCIÓN FRENTE! %d positivas → GPIO Frente (demora %ds)",
                            ctx->config.detection_threshold,
                            ctx->config.gpio_demora_frente_seg);
                    gpio_trigger_frente(ctx);
                } else {
                    log_msg(LOG_WARNING,
                            "burst: ¡DETECCIÓN FONDO! %d positivas → GPIO Fondo inmediato",
                            ctx->config.detection_threshold);
                    gpio_trigger_fondo(ctx);
                }
            } else {
                log_msg(LOG_INFO,
                        "burst: detección en área='%s' pero alarma DESARMADA → GPIO inhibido",
                        area_name);
            }
            detlog_write(ctx, email, area_name, best_camara,
                         best_conf, best_img_data, best_img_size);
            burst_reset(buf, area_name);
            return 1;
        }

    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Verificación de timeouts en cada ciclo                               */
/* ------------------------------------------------------------------ */

void burst_check_timeouts(AppContext *ctx) {
    time_t now = time(NULL);
    double timeout = ctx->config.window_minutes * 60.0;

    for (int i = 0; i < ctx->num_areas; i++) {
        BurstBuffer *buf = &ctx->buffers[i];
        if (!buf->active) continue;

        double elapsed = difftime(now, buf->window_open);
        if (elapsed >= timeout) {
            log_msg(LOG_INFO,
                    "burst: timeout área='%s'  "
                    "(%d positivas de %d requeridas) → descarte",
                    ctx->area_names[i],
                    buf->positive_count,
                    ctx->config.detection_threshold);
            burst_reset(buf, ctx->area_names[i]);
        }
    }
}
