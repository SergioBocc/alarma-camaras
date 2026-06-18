#ifndef ALARM_PROCESSOR_H
#define ALARM_PROCESSOR_H

#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include "hardware.h"

/* ============================================================
 * Límites del sistema
 * ============================================================ */
#define MAX_CAMERAS         16
#define MAX_AREAS           16
#define MAX_EMAILS_BATCH    20
#define MAX_IMAGES_PER_EMAIL 8
#define MAX_IMAGES_BUFFER   64
#define MAX_SUBJECT_LEN     256
#define MAX_PATH_LEN        512
#define MAX_ALARM_SUBJECTS  16
#define MAX_STR             512

/* ============================================================
 * Estado de armado de la alarma de cámaras
 * ============================================================ */
typedef enum {
    ALARM_DISARMED = 0,
    ALARM_ARMED    = 1
} AlarmState;

/* ============================================================
 * Configuración (leída de config.ini)
 * ============================================================ */
typedef struct {
    /* IMAP */
    char imap_server[MAX_STR];
    int  imap_port;
    char imap_user[MAX_STR];
    char imap_password[MAX_STR];
    char imap_folder[MAX_STR];

    /* Detección */
    char detection_subject[MAX_STR];

    /* Cámaras y áreas */
    int  num_cameras;
    char camera_name[MAX_CAMERAS][MAX_STR];
    char camera_area[MAX_CAMERAS][MAX_STR];

    /* Alarmas inmediatas */
    int  num_alarm_subjects;
    char alarm_subjects[MAX_ALARM_SUBJECTS][MAX_STR];
    
    /* GPIO */
    int  gpio_pulse_seconds;
    int  gpio_demora_frente_seg;   /* demora antes de activar GPIO frente */
    
    /* YOLO */
    char yolo_model[MAX_PATH_LEN];
    float yolo_confidence;

    /* Ventana de ráfaga */
    int  window_minutes;
    int  detection_threshold;

    /* MQTT alarma */
    char mqtt_host[MAX_STR];
    int  mqtt_port;
    char mqtt_user[MAX_STR];
    char mqtt_password[MAX_STR];
    char mqtt_aes_key_hex[65];  /* 32 bytes = 64 hex chars + \0 */

    /* Sistema */
    int  intervalo_minimo_seg;
    int  demora_armado_seg;
    char log_level[16];
    char log_file[MAX_PATH_LEN];
    char log_detecciones[MAX_PATH_LEN];
    char dir_imagenes[MAX_PATH_LEN];
} Config;

/* ============================================================
 * Imagen adjunta
 * ============================================================ */
typedef struct {
    uint8_t *data;
    size_t   size;
} ImageBuffer;

/* ============================================================
 * Correo clasificado
 * ============================================================ */
typedef enum {
    EMAIL_DISCARD        = 0,
    EMAIL_DETECTION      = 1,
    EMAIL_ALARM_IMMEDIATE = 2
} EmailType;

typedef struct {
    char       uid[64];
    char       subject[MAX_SUBJECT_LEN];
    char       date[128];
    EmailType  type;
    int        camera_idx;
    ImageBuffer images[MAX_IMAGES_PER_EMAIL];
    int        num_images;
} Email;

/* ============================================================
 * Buffer de ráfaga por área
 * ============================================================ */
typedef struct {
    int         area_idx;
    int         active;
    time_t      window_open;
    int         positive_count;
    int         total_images;
    pthread_mutex_t lock;
} BurstBuffer;

/* ============================================================
 * Resultado de análisis YOLO
 * ============================================================ */
typedef struct {
    int   person_detected;
    int   person_count;
    float max_confidence;
} YoloResult;

/* ============================================================
 * Contexto global del sistema
 * ============================================================ */
typedef struct {
    Config       config;
    BurstBuffer  buffers[MAX_AREAS];
    int          num_areas;
    char         area_names[MAX_AREAS][MAX_STR];
    void        *onnx_session;
    void        *onnx_env;
    int          running;
    time_t      arm_timestamp;   /* momento en que se armó el sistema */
    volatile int    gracia_activa;   /* 1 durante periodo post-armado */
    pthread_mutex_t gpio_lock;

    /* Estado de armado — compartido entre hilo MQTT y loop principal */
    AlarmState      alarm_state;
    pthread_mutex_t alarm_state_lock;
} AppContext;

/* ============================================================
 * Prototipos — config
 * ============================================================ */
int  config_load(const char *path, Config *cfg);
void config_print(const Config *cfg);

/* ============================================================
 * Prototipos — IMAP
 * ============================================================ */
int  imap_fetch_unread(AppContext *ctx, Email *emails, int max_emails);
int  imap_mark_read(AppContext *ctx, const char *uid);
int imap_mark_all_read(AppContext *ctx);

/* ============================================================
 * Prototipos — clasificador
 * ============================================================ */
EmailType classify_email(const Email *email, const Config *cfg, int *camera_idx_out);

/* ============================================================
 * Prototipos — YOLO
 * ============================================================ */
int  yolo_init(AppContext *ctx);
int  yolo_analyze(AppContext *ctx, const ImageBuffer *img, YoloResult *result);
void yolo_cleanup(AppContext *ctx);

/* ============================================================
 * Prototipos — buffer de ráfaga
 * ============================================================ */
int  burst_get_or_create_area(AppContext *ctx, const char *area_name);
int  burst_process_email(AppContext *ctx, const Email *email);
void burst_check_timeouts(AppContext *ctx);

/* ============================================================
 * Prototipos — GPIO
 * ============================================================ */
int  gpio_init(AppContext *ctx);
void gpio_trigger_frente(AppContext *ctx);   /* con demora, no bloqueante */
void gpio_trigger_fondo(AppContext *ctx);    /* inmediato, no bloqueante */
void gpio_trigger_frente_test(AppContext *ctx);  /* sin demora, solo para prueba */
void gpio_cleanup(AppContext *ctx);

/* ============================================================
 * Prototipos — módulo MQTT alarma
 * ============================================================ */
int  mqtt_alarm_init(AppContext *ctx);
void mqtt_alarm_cleanup(AppContext *ctx);


/* ============================================================
 * Prototipos — burst_reset_area
 * ============================================================ */
void burst_reset_area(AppContext *ctx, int area_idx);


/* Helper para cambiar estado de armado desde cualquier hilo */
void alarm_set_state(AppContext *ctx, AlarmState new_state, const char *source);
AlarmState alarm_get_state(AppContext *ctx);

/* ============================================================
 * Prototipos — log
 * ============================================================ */
void log_init(const Config *cfg);
void log_msg(int level, const char *fmt, ...);
void log_close(void);

#define LOG_DEBUG   0
#define LOG_INFO    1
#define LOG_WARNING 2
#define LOG_ERROR   3

/* ============================================================
 * Prototipos — log de detecciones
 * ============================================================ */
void detlog_write(AppContext *ctx, const Email *email,
                  const char *area, const char *camara,
                  float confianza,
                  const uint8_t *img_data, size_t img_size);

#endif /* ALARM_PROCESSOR_H */
