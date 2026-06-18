/*
 * mqtt_alarm.c — Módulo MQTT para control remoto de armado/desarmado
 *
 * Funciones:
 *   - Conexión SSL a EMQX con usuario cam_rpi
 *   - Publicación de estado en alarma/camaras/estado (retained)
 *   - Suscripción a alarma/camaras/cmd/# para recibir comandos
 *   - Descifrado AES-256-GCM de comandos entrantes
 *   - Cifrado AES-256-GCM del estado publicado
 *   - Hilo dedicado para pulsador físico + LED
 *   - Integración con AppContext.alarm_state
 *
 * Dependencias:
 *   - libmosquitto (sudo apt install libmosquitto-dev)
 *   - OpenSSL (libssl-dev)
 *   - libgpiod (ya instalado)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#include <mosquitto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <gpiod.h>

#include "alarm_processor.h"

/* ------------------------------------------------------------------ */
/* Constantes                                                           */
/* ------------------------------------------------------------------ */

#define TOPIC_STATE        "alarma/camaras/estado"
#define TOPIC_CMD_PREFIX   "alarma/camaras/cmd/"
#define TOPIC_CMD_SUB      "alarma/camaras/cmd/#"

#define AES_KEY_LEN   32   /* 256 bits */
#define AES_IV_LEN    12   /* 96 bits GCM */
#define AES_TAG_LEN   16   /* 128 bits GCM */

/* Intervalo de republicación de estado (segundos) */
#define STATE_REPUBLISH_S  60

/* Debounce del pulsador (ms) */
#define BUTTON_DEBOUNCE_MS 200

/* ------------------------------------------------------------------ */
/* Estado interno del módulo                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    AppContext         *ctx;
    struct mosquitto   *mosq;
    uint8_t             aes_key[AES_KEY_LEN];
    pthread_t           button_thread;
    pthread_t           mqtt_thread;
    int                 running;

    /* GPIO */
    struct gpiod_chip  *chip;
    struct gpiod_line  *button_line;
    struct gpiod_line  *test_line;
    pthread_t           test_thread;
    int                 test_sequence;   /* 0=Frente, 1=Fondo */
    struct gpiod_line  *led_line;
} MqttAlarmCtx;

static MqttAlarmCtx g_mctx = {0};

/* ------------------------------------------------------------------ */
/* Helpers de conversión hex                                            */
/* ------------------------------------------------------------------ */

static int hex_to_bytes(const char *hex, uint8_t *out, int out_len) {
    int len = (int)strlen(hex);
    if (len != out_len * 2) return -1;
    for (int i = 0; i < out_len; i++) {
        unsigned int byte;
        if (sscanf(hex + 2*i, "%02x", &byte) != 1) return -1;
        out[i] = (uint8_t)byte;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Cifrado AES-256-GCM                                                  */
/* ------------------------------------------------------------------ */

/*
 * Formato del payload cifrado (binario):
 *   [12 bytes IV][16 bytes TAG][N bytes ciphertext]
 * Total: 28 + N bytes
 * Se envía en Base64 para compatibilidad con JSON/texto
 */

/* Base64 encode simple */
static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int base64_encode(const uint8_t *in, int in_len, char *out, int out_max) {
    int i = 0, j = 0;
    while (i < in_len) {
        uint32_t octet_a = i < in_len ? in[i++] : 0;
        uint32_t octet_b = i < in_len ? in[i++] : 0;
        uint32_t octet_c = i < in_len ? in[i++] : 0;
        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;
        if (j + 4 >= out_max) return -1;
        out[j++] = b64_table[(triple >> 18) & 0x3F];
        out[j++] = b64_table[(triple >> 12) & 0x3F];
        out[j++] = b64_table[(triple >>  6) & 0x3F];
        out[j++] = b64_table[(triple >>  0) & 0x3F];
    }
    int pad = in_len % 3;
    if (pad == 1) { out[j-2] = '='; out[j-1] = '='; }
    if (pad == 2) { out[j-1] = '='; }
    out[j] = '\0';
    return j;
}

static int base64_decode(const char *in, uint8_t *out, int out_max) {
    static const int8_t T[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    int len = (int)strlen(in);
    int j = 0;
    for (int i = 0; i < len; ) {
        uint32_t sextet_a = (in[i] == '=') ? 0 : T[(uint8_t)in[i]]; i++;
        uint32_t sextet_b = (in[i] == '=') ? 0 : T[(uint8_t)in[i]]; i++;
        uint32_t sextet_c = (in[i] == '=') ? 0 : T[(uint8_t)in[i]]; i++;
        uint32_t sextet_d = (in[i] == '=') ? 0 : T[(uint8_t)in[i]]; i++;
        uint32_t triple = (sextet_a << 18) | (sextet_b << 12) | (sextet_c << 6) | sextet_d;
        if (j < out_max) out[j++] = (triple >> 16) & 0xFF;
        if (in[i-2] != '=' && j < out_max) out[j++] = (triple >> 8) & 0xFF;
        if (in[i-1] != '=' && j < out_max) out[j++] = triple & 0xFF;
    }
    return j;
}

/*
 * Cifra plaintext con AES-256-GCM.
 * Salida: Base64([IV(12)][TAG(16)][ciphertext])
 * Retorna longitud del string Base64 o -1 si error.
 */
static int aes_encrypt(const uint8_t *key, const char *plaintext,
                        char *out_b64, int out_b64_max) {
    uint8_t iv[AES_IV_LEN];
    if (RAND_bytes(iv, AES_IV_LEN) != 1) return -1;

    int pt_len = (int)strlen(plaintext);
    uint8_t *ciphertext = (uint8_t *)malloc(pt_len + 32);
    if (!ciphertext) return -1;

    uint8_t tag[AES_TAG_LEN];
    int ct_len = 0, len = 0;

    EVP_CIPHER_CTX *evp = EVP_CIPHER_CTX_new();
    EVP_EncryptInit_ex(evp, EVP_aes_256_gcm(), NULL, NULL, NULL);
    EVP_CIPHER_CTX_ctrl(evp, EVP_CTRL_GCM_SET_IVLEN, AES_IV_LEN, NULL);
    EVP_EncryptInit_ex(evp, NULL, NULL, key, iv);
    EVP_EncryptUpdate(evp, ciphertext, &len, (uint8_t *)plaintext, pt_len);
    ct_len = len;
    EVP_EncryptFinal_ex(evp, ciphertext + len, &len);
    ct_len += len;
    EVP_CIPHER_CTX_ctrl(evp, EVP_CTRL_GCM_GET_TAG, AES_TAG_LEN, tag);
    EVP_CIPHER_CTX_free(evp);

    /* Armar buffer: [IV][TAG][ciphertext] */
    int total = AES_IV_LEN + AES_TAG_LEN + ct_len;
    uint8_t *combined = (uint8_t *)malloc(total);
    if (!combined) { free(ciphertext); return -1; }
    memcpy(combined,                          iv,         AES_IV_LEN);
    memcpy(combined + AES_IV_LEN,             tag,        AES_TAG_LEN);
    memcpy(combined + AES_IV_LEN + AES_TAG_LEN, ciphertext, ct_len);

    int r = base64_encode(combined, total, out_b64, out_b64_max);
    free(combined);
    free(ciphertext);
    return r;
}

/*
 * Descifra payload Base64 con AES-256-GCM.
 * Retorna 0 si OK, -1 si error o autenticación falla.
 */
static int aes_decrypt(const uint8_t *key, const char *b64_payload,
                        char *plaintext_out, int plaintext_max) {
    uint8_t buf[4096];
    int buf_len = base64_decode(b64_payload, buf, sizeof(buf));
    if (buf_len < AES_IV_LEN + AES_TAG_LEN + 1) return -1;

    uint8_t *iv         = buf;
    uint8_t *tag        = buf + AES_IV_LEN;
    uint8_t *ciphertext = buf + AES_IV_LEN + AES_TAG_LEN;
    int      ct_len     = buf_len - AES_IV_LEN - AES_TAG_LEN;

    uint8_t *plaintext = (uint8_t *)malloc(ct_len + 1);
    if (!plaintext) return -1;

    int len = 0, pt_len = 0;
    EVP_CIPHER_CTX *evp = EVP_CIPHER_CTX_new();
    EVP_DecryptInit_ex(evp, EVP_aes_256_gcm(), NULL, NULL, NULL);
    EVP_CIPHER_CTX_ctrl(evp, EVP_CTRL_GCM_SET_IVLEN, AES_IV_LEN, NULL);
    EVP_DecryptInit_ex(evp, NULL, NULL, key, iv);
    EVP_DecryptUpdate(evp, plaintext, &len, ciphertext, ct_len);
    pt_len = len;
    EVP_CIPHER_CTX_ctrl(evp, EVP_CTRL_GCM_SET_TAG, AES_TAG_LEN, tag);
    int ok = EVP_DecryptFinal_ex(evp, plaintext + len, &len);
    EVP_CIPHER_CTX_free(evp);

    if (ok <= 0) { free(plaintext); return -1; }
    pt_len += len;
    plaintext[pt_len] = '\0';

    strncpy(plaintext_out, (char *)plaintext, plaintext_max - 1);
    plaintext_out[plaintext_max - 1] = '\0';
    free(plaintext);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Publicar estado                                                       */
/* ------------------------------------------------------------------ */

static void publish_state(MqttAlarmCtx *m) {
    AlarmState state = alarm_get_state(m->ctx);
    const char *plaintext = (state == ALARM_ARMED) ? "ARMADA" : "DESARMADA";

    char payload[512];
    if (aes_encrypt(m->aes_key, plaintext, payload, sizeof(payload)) < 0) {
        log_msg(LOG_ERROR, "mqtt_alarm: error cifrando estado");
        return;
    }

    int rc = mosquitto_publish(m->mosq, NULL, TOPIC_STATE,
                               (int)strlen(payload), payload,
                               1,     /* QoS 1 */
                               true); /* retained */
    if (rc != MOSQ_ERR_SUCCESS)
        log_msg(LOG_ERROR, "mqtt_alarm: error publicando estado: %s",
                mosquitto_strerror(rc));
    else
        log_msg(LOG_INFO, "mqtt_alarm: estado publicado → %s", plaintext);
}

/* ------------------------------------------------------------------ */
/* Callback de mensaje recibido                                          */
/* ------------------------------------------------------------------ */

static void on_message(struct mosquitto *mosq, void *userdata,
                        const struct mosquitto_message *msg) {
    (void)mosq;
    MqttAlarmCtx *m = (MqttAlarmCtx *)userdata;

    /* Extraer nombre de usuario del topic: alarma/camaras/cmd/{usuario} */
    const char *prefix = TOPIC_CMD_PREFIX;
    const char *user = msg->topic + strlen(prefix);

    /* Descifrar payload */
    char plaintext[256];
    if (aes_decrypt(m->aes_key, (char *)msg->payload,
                    plaintext, sizeof(plaintext)) != 0) {
        log_msg(LOG_WARNING, "mqtt_alarm: descifrado fallido en cmd de '%s'", user);
        return;
    }

    log_msg(LOG_INFO, "mqtt_alarm: cmd de '%s' → '%s'", user, plaintext);

    if (strcmp(plaintext, "ARMAR") == 0) {
        alarm_set_state(m->ctx, ALARM_ARMED, user);
        publish_state(m);
    } else if (strcmp(plaintext, "DESARMAR") == 0) {
        alarm_set_state(m->ctx, ALARM_DISARMED, user);
        publish_state(m);
    } else {
        log_msg(LOG_WARNING, "mqtt_alarm: comando desconocido '%s'", plaintext);
    }
}

static void on_connect(struct mosquitto *mosq, void *userdata, int rc) {
    MqttAlarmCtx *m = (MqttAlarmCtx *)userdata;
    if (rc != 0) {
        log_msg(LOG_ERROR, "mqtt_alarm: conexión rechazada: %d", rc);
        return;
    }
    log_msg(LOG_INFO, "mqtt_alarm: conectado a EMQX");
    mosquitto_subscribe(mosq, NULL, TOPIC_CMD_SUB, 1);
    publish_state(m);
}

static void on_disconnect(struct mosquitto *mosq, void *userdata, int rc) {
    (void)mosq; (void)userdata;
    log_msg(LOG_WARNING, "mqtt_alarm: desconectado (rc=%d), reconectando...", rc);
}

/* ------------------------------------------------------------------ */
/* Hilo MQTT                                                            */
/* ------------------------------------------------------------------ */

static void *mqtt_thread_fn(void *arg) {
    MqttAlarmCtx *m = (MqttAlarmCtx *)arg;
    time_t last_publish = 0;

    while (m->running) {
        mosquitto_loop(m->mosq, 1000, 1);

        /* Republicar estado periódicamente */
        time_t now = time(NULL);
        if (difftime(now, last_publish) >= STATE_REPUBLISH_S) {
            publish_state(m);
            last_publish = now;
        }
    }
    return NULL;
}

static void *test_button_thread_fn(void *arg) {
    MqttAlarmCtx *m = (MqttAlarmCtx *)arg;
    int last_value = 1;
    struct timespec last_press = {0};

    while (m->running) {
        int value = gpiod_line_get_value(m->test_line);
        if (value < 0) { usleep(10000); continue; }

        if (value == 0 && last_value == 1) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long elapsed_ms = (now.tv_sec - last_press.tv_sec) * 1000 +
                              (now.tv_nsec - last_press.tv_nsec) / 1000000;

            if (elapsed_ms > BUTTON_DEBOUNCE_MS) {
                last_press = now;

                if (alarm_get_state(m->ctx) == ALARM_ARMED) {
                    log_msg(LOG_INFO, "test_btn: sistema ARMADO, boton ignorado");
                } else {
                    if (m->test_sequence == 0) {
                        log_msg(LOG_WARNING, "test_btn: simulando deteccion FRENTE");
                        gpio_trigger_frente_test(m->ctx);
                        m->test_sequence = 1;
                    } else {
                        log_msg(LOG_WARNING, "test_btn: simulando deteccion FONDO");
                        gpio_trigger_fondo(m->ctx);
                        m->test_sequence = 0;
                    }
                }
            }
        }
        last_value = value;
        usleep(10000);
    }
    return NULL;
}






/* ------------------------------------------------------------------ */
/* Hilo pulsador físico + LED                                           */
/* ------------------------------------------------------------------ */

static void update_led(MqttAlarmCtx *m) {
    AlarmState state = alarm_get_state(m->ctx);
    gpiod_line_set_value(m->led_line, (state == ALARM_ARMED) ? 1 : 0);
}

static void *button_thread_fn(void *arg) {
    MqttAlarmCtx *m = (MqttAlarmCtx *)arg;
    int last_value = 1; /* pull-up: reposo = 1 */
    struct timespec last_press = {0};

    while (m->running) {
        int value = gpiod_line_get_value(m->button_line);
        if (value < 0) { usleep(10000); continue; }

        /* Detectar flanco descendente (presión del botón) */
        if (value == 0 && last_value == 1) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long elapsed_ms = (now.tv_sec - last_press.tv_sec) * 1000 +
                              (now.tv_nsec - last_press.tv_nsec) / 1000000;

            if (elapsed_ms > BUTTON_DEBOUNCE_MS) {
                last_press = now;
                /* Toggle estado */
                AlarmState current = alarm_get_state(m->ctx);
                AlarmState new_state = (current == ALARM_ARMED) ?
                                        ALARM_DISARMED : ALARM_ARMED;
                alarm_set_state(m->ctx, new_state, "boton_fisico");
                update_led(m);
                publish_state(m);
            }
        }
        last_value = value;
        usleep(10000); /* 10ms polling */
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Funciones de estado compartido                                        */
/* ------------------------------------------------------------------ */

void alarm_set_state(AppContext *ctx, AlarmState new_state, const char *source) {
    pthread_mutex_lock(&ctx->alarm_state_lock);
    AlarmState old = ctx->alarm_state;
    ctx->alarm_state = new_state;
    if (new_state == ALARM_ARMED) {
            ctx->arm_timestamp = time(NULL);
            ctx->gracia_activa = 1;
            g_mctx.test_sequence = 0;
    }
    pthread_mutex_unlock(&ctx->alarm_state_lock);
    if (old != new_state) {
        log_msg(LOG_INFO, "mqtt_alarm: estado cambiado → %s  (por: %s)",
                (new_state == ALARM_ARMED) ? "ARMADA" : "DESARMADA", source);
        if (g_mctx.led_line)
            gpiod_line_set_value(g_mctx.led_line,
                                 (new_state == ALARM_ARMED) ? 1 : 0);
        /* Registrar timestamp de armado */
        if (new_state == ALARM_ARMED)
            ctx->arm_timestamp = time(NULL);
        /* Reset secuencia de prueba al armar */
        if (new_state == ALARM_ARMED)
            g_mctx.test_sequence = 0;
    }
}

AlarmState alarm_get_state(AppContext *ctx) {
    pthread_mutex_lock(&ctx->alarm_state_lock);
    AlarmState s = ctx->alarm_state;
    pthread_mutex_unlock(&ctx->alarm_state_lock);
    return s;
}

/* ------------------------------------------------------------------ */
/* Inicialización y limpieza                                            */
/* ------------------------------------------------------------------ */

int mqtt_alarm_init(AppContext *ctx) {
    MqttAlarmCtx *m = &g_mctx;
    memset(m, 0, sizeof(*m));
    m->ctx     = ctx;
    m->running = 1;

    /* Convertir clave AES hex → bytes */
    if (hex_to_bytes(ctx->config.mqtt_aes_key_hex,
                     m->aes_key, AES_KEY_LEN) != 0) {
        log_msg(LOG_ERROR, "mqtt_alarm: clave AES inválida en config");
        return -1;
    }

    /* ── GPIO pulsador y LED ────────────────────────────────────── */
    m->chip = gpiod_chip_open("/dev/gpiochip0");
    if (!m->chip) {
        log_msg(LOG_ERROR, "mqtt_alarm: no se pudo abrir gpiochip0");
        return -1;
    }

    m->button_line = gpiod_chip_get_line(m->chip,
                                          HW_GPIO_BUTTON_PIN);
    if (!m->button_line ||
        gpiod_line_request_input_flags(m->button_line,
                                        "alarm_button",
                                        GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP) < 0) {
        log_msg(LOG_ERROR, "mqtt_alarm: error configurando pulsador GPIO %d",
                HW_GPIO_BUTTON_PIN);
        return -1;
    }

    m->led_line = gpiod_chip_get_line(m->chip, HW_GPIO_LED_PIN);
    if (!m->led_line ||
        gpiod_line_request_output(m->led_line, "alarm_led", 0) < 0) {
        log_msg(LOG_ERROR, "mqtt_alarm: error configurando LED GPIO %d",
                HW_GPIO_LED_PIN);
        return -1;
    }
    
    /* Boton de prueba */
    m->test_line = gpiod_chip_get_line(m->chip, HW_GPIO_TEST_PIN);
    if (!m->test_line ||
        gpiod_line_request_input_flags(m->test_line,
                                        "test_button",
                                        GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP) < 0) {
        log_msg(LOG_WARNING, "mqtt_alarm: boton prueba GPIO %d no disponible",
                HW_GPIO_TEST_PIN);
        m->test_line = NULL;
    }
    m->test_sequence = 0;
    
    /* Estado inicial → DESARMADA, LED apagado */
    alarm_set_state(ctx, ALARM_DISARMED, "inicio");
    log_msg(LOG_INFO, "mqtt_alarm: GPIO pulsador=%d  LED=%d",
            HW_GPIO_BUTTON_PIN, HW_GPIO_LED_PIN);

    /* ── Mosquitto ──────────────────────────────────────────────── */
    mosquitto_lib_init();

    char client_id[64];
    snprintf(client_id, sizeof(client_id), "cam_rpi_%ld", (long)getpid());

    m->mosq = mosquitto_new(client_id, true, m);
    if (!m->mosq) {
        log_msg(LOG_ERROR, "mqtt_alarm: mosquitto_new falló");
        return -1;
    }

    mosquitto_username_pw_set(m->mosq,
                               ctx->config.mqtt_user,
                               ctx->config.mqtt_password);
    //mosquitto_tls_set(m->mosq, NULL, NULL, NULL, NULL, NULL);
    mosquitto_tls_set(m->mosq, "/home/pi/alarma_camaras/emqxsl-ca.crt", NULL, NULL, NULL, NULL);
    mosquitto_tls_opts_set(m->mosq, 1, NULL, NULL);

    mosquitto_connect_callback_set(m->mosq, on_connect);
    mosquitto_disconnect_callback_set(m->mosq, on_disconnect);
    mosquitto_message_callback_set(m->mosq, on_message);

    int rc = mosquitto_connect(m->mosq,
                               ctx->config.mqtt_host,
                               ctx->config.mqtt_port,
                               60);
    if (rc != MOSQ_ERR_SUCCESS) {
        log_msg(LOG_ERROR, "mqtt_alarm: connect falló: %s",
                mosquitto_strerror(rc));
        return -1;
    }

    /* ── Hilos ──────────────────────────────────────────────────── */
    pthread_create(&m->mqtt_thread,   NULL, mqtt_thread_fn,   m);
    pthread_create(&m->button_thread, NULL, button_thread_fn, m);
    if (m->test_line)
        pthread_create(&m->test_thread, NULL, test_button_thread_fn, m);
    log_msg(LOG_INFO, "mqtt_alarm: módulo iniciado OK");
    return 0;
}

void mqtt_alarm_cleanup(AppContext *ctx) {
    (void)ctx;
    MqttAlarmCtx *m = &g_mctx;
    m->running = 0;

    pthread_join(m->mqtt_thread,   NULL);
    pthread_join(m->button_thread, NULL);
    
    if (m->test_line)
        pthread_join(m->test_thread, NULL);
    if (m->mosq) {
        mosquitto_disconnect(m->mosq);
        mosquitto_destroy(m->mosq);
    }
    mosquitto_lib_cleanup();

    if (m->led_line)    { gpiod_line_set_value(m->led_line, 0);
                          gpiod_line_release(m->led_line); }
    if (m->test_line) {
        gpiod_line_release(m->test_line);
        m->test_line = NULL;
    }
    if (m->button_line)   gpiod_line_release(m->button_line);
    if (m->chip)          gpiod_chip_close(m->chip);

    log_msg(LOG_INFO, "mqtt_alarm: módulo detenido");
}
