/*
 * config.c — Carga y valida la configuración desde config.ini
 */
 
/*  Las credenciales para los usuarios de EMQX son:
    cam_agustina     R8amv5AL20h@
    cam_sergio       nnxy7h5vM717
    cam_rpi          5JajQ102$nvp
    cam_pedro	     695bqtTawn%2
    cam_maxi         ru4HH8tji&1s
    cam_malva	     CkZ733#W1pgf
    cam_android      dzci376knv?N
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "alarm_processor.h"
#include "inih/ini.h"

static void str_trim(char *s) {
    int len = (int)strlen(s);
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' ||
                        s[len-1] == '\r' || s[len-1] == '\n'))
        s[--len] = '\0';
    int start = 0;
    while (s[start] == ' ' || s[start] == '\t') start++;
    if (start > 0) memmove(s, s + start, len - start + 1);
}

static int split_pipe(const char *src, char dest[][MAX_STR], int max_items) {
    char buf[MAX_STR * MAX_ALARM_SUBJECTS];
    strncpy(buf, src, sizeof(buf) - 1);
    buf[sizeof(buf)-1] = '\0';
    int count = 0;
    char *token = strtok(buf, "|");
    while (token && count < max_items) {
        strncpy(dest[count], token, MAX_STR - 1);
        str_trim(dest[count]);
        count++;
        token = strtok(NULL, "|");
    }
    return count;
}

static int ini_callback(void *user, const char *section,
                         const char *name, const char *value) {
    Config *cfg = (Config *)user;

#define MATCH(s, n) (strcmp(section, s) == 0 && strcmp(name, n) == 0)
#define STARTS(s, n) (strcmp(section, s) == 0 && strncmp(name, n, strlen(n)) == 0)

    /* ---- IMAP ---- */
    if      (MATCH("imap", "servidor"))   strncpy(cfg->imap_server,   value, MAX_STR-1);
    else if (MATCH("imap", "puerto"))     cfg->imap_port = atoi(value);
    else if (MATCH("imap", "usuario"))    strncpy(cfg->imap_user,     value, MAX_STR-1);
    else if (MATCH("imap", "password"))   strncpy(cfg->imap_password, value, MAX_STR-1);
    else if (MATCH("imap", "carpeta"))    strncpy(cfg->imap_folder,   value, MAX_STR-1);

    /* ---- Detección ---- */
    else if (MATCH("deteccion", "asunto_deteccion"))
        strncpy(cfg->detection_subject, value, MAX_STR-1);
    else if (STARTS("deteccion", "camara_") && strstr(name, "_nombre")) {
        int idx = atoi(name + 7) - 1;
        if (idx >= 0 && idx < MAX_CAMERAS) {
            strncpy(cfg->camera_name[idx], value, MAX_STR-1);
            if (idx >= cfg->num_cameras) cfg->num_cameras = idx + 1;
        }
    }
    else if (STARTS("deteccion", "camara_") && strstr(name, "_area")) {
        int idx = atoi(name + 7) - 1;
        if (idx >= 0 && idx < MAX_CAMERAS) {
            strncpy(cfg->camera_area[idx], value, MAX_STR-1);
            if (idx >= cfg->num_cameras) cfg->num_cameras = idx + 1;
        }
    }

    /* ---- Alarma inmediata ---- */
    else if (MATCH("alarma_inmediata", "asuntos"))
        cfg->num_alarm_subjects = split_pipe(value, cfg->alarm_subjects,
                                             MAX_ALARM_SUBJECTS);

    /* ---- YOLO ---- */
    else if (MATCH("yolo", "modelo"))    strncpy(cfg->yolo_model, value, MAX_PATH_LEN-1);
    else if (MATCH("yolo", "confianza")) cfg->yolo_confidence = (float)atof(value);

    /* ---- Ventana ---- */
    else if (MATCH("ventana", "tiempo_minutos"))     cfg->window_minutes = atoi(value);
    else if (MATCH("ventana", "umbral_detecciones")) cfg->detection_threshold = atoi(value);

    /* ---- GPIO ---- */
    else if (MATCH("gpio", "duracion_pulso")) cfg->gpio_pulse_seconds = atoi(value);
    else if (MATCH("gpio", "demora_frente_seg")) cfg->gpio_demora_frente_seg = atoi(value);
   
    /* ---- MQTT ---- */
    else if (MATCH("mqtt", "host"))     strncpy(cfg->mqtt_host,     value, MAX_STR-1);
    else if (MATCH("mqtt", "puerto"))   cfg->mqtt_port = atoi(value);
    else if (MATCH("mqtt", "usuario"))  strncpy(cfg->mqtt_user,     value, MAX_STR-1);
    else if (MATCH("mqtt", "password")) strncpy(cfg->mqtt_password, value, MAX_STR-1);
    else if (MATCH("mqtt", "aes_key"))  strncpy(cfg->mqtt_aes_key_hex, value, 64);

    /* ---- Sistema ---- */
    else if (MATCH("sistema", "intervalo_minimo_seg")) cfg->intervalo_minimo_seg = atoi(value);
    else if (MATCH("sistema", "log_nivel"))      strncpy(cfg->log_level, value, 15);
    else if (MATCH("sistema", "log_archivo"))    strncpy(cfg->log_file,  value, MAX_PATH_LEN-1);
    else if (MATCH("sistema", "log_detecciones")) strncpy(cfg->log_detecciones, value, MAX_PATH_LEN-1);
    else if (MATCH("sistema", "dir_imagenes"))    strncpy(cfg->dir_imagenes,    value, MAX_PATH_LEN-1);
    else if (MATCH("sistema", "demora_armado_seg")) cfg->demora_armado_seg = atoi(value);

#undef MATCH
#undef STARTS
    return 1;
}

static void config_defaults(Config *cfg) {
    memset(cfg, 0, sizeof(Config));
    cfg->imap_port           = 993;
    cfg->yolo_confidence     = 0.20f;
    cfg->window_minutes      = 2;
    cfg->detection_threshold = 3;
    cfg->gpio_pulse_seconds  = 1;
    cfg->mqtt_port           = 8883;
    cfg->intervalo_minimo_seg = 15;
    cfg->gpio_demora_frente_seg = 30;
    cfg->demora_armado_seg = 60;
    strncpy(cfg->imap_folder, "INBOX",                MAX_STR-1);
    strncpy(cfg->log_level,   "INFO",                 15);
    strncpy(cfg->log_file,    "alarm_processor.log",  MAX_PATH_LEN-1);
    strncpy(cfg->yolo_model,  "yolov8n.onnx",         MAX_PATH_LEN-1);
    strncpy(cfg->log_detecciones, "/var/log/alarma_detecciones.tsv", MAX_PATH_LEN-1);
    strncpy(cfg->dir_imagenes,    "/var/log/alarma_imagenes",        MAX_PATH_LEN-1);
}

int config_load(const char *path, Config *cfg) {
    config_defaults(cfg);
    int ret = ini_parse(path, ini_callback, cfg);
    if (ret < 0) { fprintf(stderr, "config: no se pudo abrir '%s'\n", path); return -1; }
    if (ret > 0) { fprintf(stderr, "config: error en línea %d de '%s'\n", ret, path); return -1; }

    if (cfg->imap_server[0] == '\0') { fprintf(stderr, "config: falta [imap] servidor\n"); return -1; }
    if (cfg->imap_user[0]   == '\0') { fprintf(stderr, "config: falta [imap] usuario\n");  return -1; }
    if (cfg->num_cameras    == 0)    { fprintf(stderr, "config: no se definió ninguna cámara\n"); return -1; }
    if (cfg->mqtt_host[0]   == '\0') { fprintf(stderr, "config: falta [mqtt] host\n");      return -1; }
    if (cfg->mqtt_aes_key_hex[0] == '\0') { fprintf(stderr, "config: falta [mqtt] aes_key\n"); return -1; }

    return 0;
}

void config_print(const Config *cfg) {
    printf("=== Configuración cargada ===\n");
    printf("IMAP:       %s:%d  usuario=%s\n", cfg->imap_server, cfg->imap_port, cfg->imap_user);
    printf("Cámaras:    %d definidas\n", cfg->num_cameras);
    for (int i = 0; i < cfg->num_cameras; i++)
        printf("  [%d] '%s' → área '%s'\n", i, cfg->camera_name[i], cfg->camera_area[i]);
    printf("YOLO:       modelo=%s  confianza=%.2f\n", cfg->yolo_model, cfg->yolo_confidence);
    printf("Ventana:    %d min  umbral=%d detecciones\n", cfg->window_minutes, cfg->detection_threshold);
    printf("GPIO:       frente=pin%d  fondo=pin%d  boton=pin%d  led=pin%d  pulso=%ds  demora_frente=%ds\n",
           HW_GPIO_FRENTE_PIN, HW_GPIO_FONDO_PIN,
           HW_GPIO_BUTTON_PIN, HW_GPIO_LED_PIN,
           cfg->gpio_pulse_seconds, cfg->gpio_demora_frente_seg);
    printf("MQTT:       %s:%d  usuario=%s\n", cfg->mqtt_host, cfg->mqtt_port, cfg->mqtt_user);
    printf("Ciclo:      intervalo minimo=%ds  demora_armado=%ds\n", cfg->intervalo_minimo_seg, cfg->demora_armado_seg);
    printf("=============================\n");
}
