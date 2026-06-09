/*
 * log.c — Logger simple con nivel y archivo de salida
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include "alarm_processor.h"

static FILE *log_fp   = NULL;
static int   log_level_min = LOG_INFO;

static const char *level_str[] = { "DEBUG", "INFO ", "WARN ", "ERROR" };

void log_init(const Config *cfg) {
    /* Nivel */
    if      (strcmp(cfg->log_level, "DEBUG")   == 0) log_level_min = LOG_DEBUG;
    else if (strcmp(cfg->log_level, "WARNING") == 0) log_level_min = LOG_WARNING;
    else if (strcmp(cfg->log_level, "ERROR")   == 0) log_level_min = LOG_ERROR;
    else                                              log_level_min = LOG_INFO;

    /* Archivo */
    if (cfg->log_file[0] != '\0') {
        log_fp = fopen(cfg->log_file, "a");
        if (!log_fp) {
            fprintf(stderr, "log: no se pudo abrir '%s', usando stderr\n",
                    cfg->log_file);
        }
    }
}

void log_msg(int level, const char *fmt, ...) {
    if (level < log_level_min) return;

    /* Timestamp */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm_info);

    va_list args;

    /* Consola */
    va_start(args, fmt);
    fprintf(stderr, "[%s] %s  ", ts, level_str[level]);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);

    /* Archivo */
    if (log_fp) {
        va_start(args, fmt);
        fprintf(log_fp, "[%s] %s  ", ts, level_str[level]);
        vfprintf(log_fp, fmt, args);
        fprintf(log_fp, "\n");
        fflush(log_fp);
        va_end(args);
    }
}

void log_close(void) {
    if (log_fp) {
        fclose(log_fp);
        log_fp = NULL;
    }
}
