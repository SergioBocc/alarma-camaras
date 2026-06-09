/*
 * detlog.c — Log de detecciones positivas en formato TSV
 *
 * Formato de cada línea:
 *   fecha_mail\thora_mail\tarea\tcamara\tuid\tconfianza\timagen
 *
 * La imagen de mayor confianza se guarda en dir_imagenes con
 * nombre det_NNNNNN.jpg donde NNNNNN es un contador correlativo
 * persistido en dir_imagenes/contador.txt
 */
#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "alarm_processor.h"

/* ------------------------------------------------------------------ */
/* Contador correlativo persistente                                     */
/* ------------------------------------------------------------------ */

static long read_counter(const char *dir) {
    char path[MAX_PATH_LEN + 32];
    snprintf(path, sizeof(path), "%s/contador.txt", dir);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    long n = 0;
    fscanf(f, "%ld", &n);
    fclose(f);
    return n;
}

static void write_counter(const char *dir, long n) {
    char path[MAX_PATH_LEN + 32];
    snprintf(path, sizeof(path), "%s/contador.txt", dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%ld\n", n);
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* Guardar imagen JPEG                                                  */
/* ------------------------------------------------------------------ */

static int save_image(const char *dir, long counter,
                       const uint8_t *data, size_t size,
                       char *filename_out, int filename_max) {
    snprintf(filename_out, filename_max, "det_%06ld.jpg", counter);
    char path[MAX_PATH_LEN + 32];
    snprintf(path, sizeof(path), "%s/%s", dir, filename_out);

    FILE *f = fopen(path, "wb");
    if (!f) {
        log_msg(LOG_ERROR, "detlog: no se pudo crear imagen '%s'", path);
        return -1;
    }
    fwrite(data, 1, size, f);
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Parsear fecha y hora del campo date del email                        */
/* ------------------------------------------------------------------ */

/*
 * El campo date viene en formato RFC 2822, por ejemplo:
 *   Mon, 08 Jun 2026 20:00:32 -0300
 * Extraemos solo fecha (YYYY-MM-DD) y hora (HH:MM:SS)
 */
static void parse_mail_date(const char *date_str,
                              char *date_out, int date_max,
                              char *time_out, int time_max) {
    /* Intentar parsear con strptime */
    struct tm tm = {0};
    char *ret = strptime(date_str, "%a, %d %b %Y %H:%M:%S", &tm);
    if (ret) {
        strftime(date_out, date_max, "%Y-%m-%d", &tm);
        strftime(time_out, time_max, "%H:%M:%S", &tm);
    } else {
        /* Si falla, usar el string original */
        strncpy(date_out, date_str, date_max - 1);
        strncpy(time_out, "??:??:??", time_max - 1);
    }
}

/* ------------------------------------------------------------------ */
/* API pública                                                          */
/* ------------------------------------------------------------------ */

void detlog_write(AppContext *ctx, const Email *email,
                  const char *area, const char *camara,
                  float confianza,
                  const uint8_t *img_data, size_t img_size) {

    const char *dir = ctx->config.dir_imagenes;
    const char *tsv = ctx->config.log_detecciones;

    /* Obtener y actualizar contador */
    long counter = read_counter(dir) + 1;
    write_counter(dir, counter);

    /* Guardar imagen */
    char filename[64] = "sin_imagen";
    if (img_data && img_size > 0)
        save_image(dir, counter, img_data, img_size, filename, sizeof(filename));

    /* Parsear fecha del mail */
    char mail_date[32] = "sin-fecha";
    char mail_time[16] = "??:??:??";
    if (email->date[0] != '\0')
        parse_mail_date(email->date, mail_date, sizeof(mail_date),
                        mail_time, sizeof(mail_time));

    /* Escribir línea TSV */
    FILE *f = fopen(tsv, "a");
    if (!f) {
        log_msg(LOG_ERROR, "detlog: no se pudo abrir '%s'", tsv);
        return;
    }

    /* Cabecera si el archivo está vacío */
    fseek(f, 0, SEEK_END);
    if (ftell(f) == 0)
        fprintf(f, "fecha_mail\thora_mail\tarea\tcamara\tuid\tconfianza\timagen\n");

    fprintf(f, "%s\t%s\t%s\t%s\t%s\t%.3f\t%s\n",
            mail_date, mail_time, area, camara,
            email->uid, confianza, filename);

    fclose(f);

    log_msg(LOG_INFO, "detlog: %s %s  area=%s  cam=%s  uid=%s  conf=%.2f  img=%s",
            mail_date, mail_time, area, camara,
            email->uid, confianza, filename);
}
