/*
 * imap.c — Consulta IMAP con libcurl, descarga adjuntos de imágenes
 *
 * Flujo:
 *   1. Busca mensajes no leídos (UNSEEN) → lista de UIDs
 *   2. Para cada UID: descarga cabecera Subject + Date
 *   3. Decodifica el Subject si viene en formato MIME encoded-word
 *   4. Clasifica el correo
 *   5. Si es EMAIL_DETECTION: descarga el cuerpo completo y extrae adjuntos
 *   6. Marca como leído si corresponde
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <curl/curl.h>
#include "alarm_processor.h"

/* ------------------------------------------------------------------ */
/* Buffer dinámico para respuestas de curl                              */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *data;
    size_t size;
    size_t capacity;
} CurlBuf;

static void curlbuf_init(CurlBuf *b) {
    b->data     = (char *)malloc(4096);
    b->size     = 0;
    b->capacity = 4096;
    if (b->data) b->data[0] = '\0';
}

static void curlbuf_free(CurlBuf *b) {
    free(b->data);
    b->data = NULL;
    b->size = b->capacity = 0;
}

static size_t curlbuf_write(void *ptr, size_t sz, size_t nmemb, void *userdata) {
    CurlBuf *b   = (CurlBuf *)userdata;
    size_t   len = sz * nmemb;

    while (b->size + len + 1 > b->capacity) {
        b->capacity *= 2;
        b->data = (char *)realloc(b->data, b->capacity);
        if (!b->data) return 0;
    }
    memcpy(b->data + b->size, ptr, len);
    b->size += len;
    b->data[b->size] = '\0';
    return len;
}

/* ------------------------------------------------------------------ */
/* Base64                                                               */
/* ------------------------------------------------------------------ */

static const char b64chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_val(char c) {
    const char *p = strchr(b64chars, c);
    return p ? (int)(p - b64chars) : -1;
}

static uint8_t *base64_decode(const char *src, size_t src_len, size_t *out_len) {
    char *clean = (char *)malloc(src_len + 1);
    if (!clean) return NULL;
    size_t clen = 0;
    for (size_t i = 0; i < src_len; i++) {
        if (!isspace((unsigned char)src[i]))
            clean[clen++] = src[i];
    }
    clean[clen] = '\0';

    size_t decoded_len = (clen / 4) * 3;
    uint8_t *out = (uint8_t *)malloc(decoded_len + 4);
    if (!out) { free(clean); return NULL; }

    size_t j = 0;
    for (size_t i = 0; i + 3 < clen; i += 4) {
        int v0 = b64_val(clean[i]);
        int v1 = b64_val(clean[i+1]);
        int v2 = b64_val(clean[i+2]);
        int v3 = b64_val(clean[i+3]);
        if (v0 < 0 || v1 < 0) break;
        out[j++] = (uint8_t)((v0 << 2) | (v1 >> 4));
        if (clean[i+2] != '=' && v2 >= 0)
            out[j++] = (uint8_t)((v1 << 4) | (v2 >> 2));
        if (clean[i+3] != '=' && v3 >= 0)
            out[j++] = (uint8_t)((v2 << 6) | v3);
    }

    free(clean);
    *out_len = j;
    return out;
}

/* ------------------------------------------------------------------ */
/* Decodificación MIME encoded-word  =?charset?B?base64?=              */
/* ------------------------------------------------------------------ */

static void decode_mime_subject(const char *encoded, char *out, int out_size) {
    out[0] = '\0';
    int out_pos = 0;
    const char *p = encoded;

    while (*p && out_pos < out_size - 1) {
        if (p[0] == '=' && p[1] == '?') {
            const char *start = p + 2;
            const char *q1 = strchr(start, '?');
            if (!q1) { out[out_pos++] = *p++; continue; }

            char enc_type = toupper((unsigned char)*(q1 + 1));
            if (*(q1 + 2) != '?') { out[out_pos++] = *p++; continue; }

            const char *data_start = q1 + 3;
            const char *data_end   = strstr(data_start, "?=");
            if (!data_end) { out[out_pos++] = *p++; continue; }

            size_t data_len = (size_t)(data_end - data_start);

            if (enc_type == 'B') {
                size_t decoded_len = 0;
                uint8_t *decoded = base64_decode(data_start, data_len, &decoded_len);
                if (decoded) {
                    size_t copy = decoded_len;
                    if (copy > (size_t)(out_size - out_pos - 1))
                        copy = (size_t)(out_size - out_pos - 1);
                    memcpy(out + out_pos, decoded, copy);
                    out_pos += (int)copy;
                    free(decoded);
                }
            } else if (enc_type == 'Q') {
                for (size_t i = 0; i < data_len && out_pos < out_size - 1; i++) {
                    if (data_start[i] == '_') {
                        out[out_pos++] = ' ';
                    } else if (data_start[i] == '=' && i + 2 < data_len) {
                        char hex[3] = { data_start[i+1], data_start[i+2], '\0' };
                        out[out_pos++] = (char)strtol(hex, NULL, 16);
                        i += 2;
                    } else {
                        out[out_pos++] = data_start[i];
                    }
                }
            }

            p = data_end + 2;
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        } else {
            out[out_pos++] = *p++;
        }
    }
    out[out_pos] = '\0';
}

/* ------------------------------------------------------------------ */
/* Helpers MIME                                                         */
/* ------------------------------------------------------------------ */

static void parse_header_value(const char *line, const char *header,
                                char *out, int max_len) {
    size_t hlen = strlen(header);
    const char *p = line;
    while (*p) {
        if (strncasecmp(p, header, hlen) == 0 && p[hlen] == ':') {
            const char *v = p + hlen + 1;
            while (*v == ' ' || *v == '\t') v++;
            int i = 0;
            while (*v && *v != '\r' && *v != '\n' && i < max_len - 1)
                out[i++] = *v++;
            out[i] = '\0';
            return;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    out[0] = '\0';
}

/* ------------------------------------------------------------------ */
/* Extracción de adjuntos de imagen desde cuerpo MIME multipart         */
/* ------------------------------------------------------------------ */

static void extract_image_attachments(const char *body, size_t body_len,
                                       Email *email) {
    (void)body_len;

    const char *bstart = strstr(body, "boundary=");
    if (!bstart) bstart = strstr(body, "Boundary=");
    if (!bstart) bstart = strstr(body, "BOUNDARY=");

    if (!bstart) {
        log_msg(LOG_DEBUG, "imap: no se encontró boundary uid=%s", email->uid);
        return;
    }
    bstart += strlen("boundary=");

    char boundary[256] = {0};
    int bi = 0;
    int quoted = (*bstart == '"');
    if (quoted) bstart++;
    while (*bstart && *bstart != '"' && *bstart != '\r' &&
           *bstart != '\n' && *bstart != ';' && bi < 254) {
        boundary[bi++] = *bstart++;
    }
    boundary[bi] = '\0';

    if (boundary[0] == '\0') {
        log_msg(LOG_WARNING, "imap: boundary vacío uid=%s", email->uid);
        return;
    }

    char delim[260];
    snprintf(delim, sizeof(delim), "--%s", boundary);

    const char *pos = body;
    while (email->num_images < MAX_IMAGES_PER_EMAIL) {
        const char *part_start = strstr(pos, delim);
        if (!part_start) break;
        part_start += strlen(delim);
        while (*part_start == '\r' || *part_start == '\n') part_start++;
        if (*part_start == '-') break;

        const char *part_end = strstr(part_start, delim);
        if (!part_end) break;

        const char *header_end = strstr(part_start, "\r\n\r\n");
        if (!header_end) {
            header_end = strstr(part_start, "\n\n");
            if (!header_end) { pos = part_end; continue; }
        }

        size_t hdr_len = (size_t)(header_end - part_start);
        char *hdrs = (char *)malloc(hdr_len + 1);
        if (!hdrs) break;
        memcpy(hdrs, part_start, hdr_len);
        hdrs[hdr_len] = '\0';

        char content_type[128] = {0};
        parse_header_value(hdrs, "Content-Type", content_type, sizeof(content_type));

        char encoding[64] = {0};
        parse_header_value(hdrs, "Content-Transfer-Encoding", encoding, sizeof(encoding));

        free(hdrs);

        if (strncasecmp(content_type, "image/", 6) == 0 &&
            strncasecmp(encoding, "base64", 6) == 0) {

            const char *b64_start = header_end;
            while (*b64_start == '\r' || *b64_start == '\n') b64_start++;
            size_t b64_len = (size_t)(part_end - b64_start);
            while (b64_len > 0 && (b64_start[b64_len-1] == '\r' ||
                                    b64_start[b64_len-1] == '\n'))
                b64_len--;

            size_t img_len = 0;
            uint8_t *img_data = base64_decode(b64_start, b64_len, &img_len);
            if (img_data && img_len > 0) {
                email->images[email->num_images].data = img_data;
                email->images[email->num_images].size = img_len;
                email->num_images++;
                log_msg(LOG_DEBUG, "imap: imagen %d  bytes=%zu  uid=%s",
                        email->num_images, img_len, email->uid);
            } else {
                free(img_data);
            }
        }

        pos = part_end;
    }
}

/* ------------------------------------------------------------------ */
/* CURL helpers                                                         */
/* ------------------------------------------------------------------ */

static CURL *curl_create(const Config *cfg) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    curl_easy_setopt(curl, CURLOPT_USERNAME,       cfg->imap_user);
    curl_easy_setopt(curl, CURLOPT_PASSWORD,       cfg->imap_password);
    curl_easy_setopt(curl, CURLOPT_USE_SSL,        CURLUSESSL_ALL);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        30L);

    return curl;
}

/* ------------------------------------------------------------------ */
/* Búsqueda de UIDs no leídos                                           */
/* ------------------------------------------------------------------ */

static int imap_search_unseen(const Config *cfg,
                               char uids_out[][64], int max_uids) {
    CURL *curl = curl_create(cfg);
    if (!curl) return -1;

    CurlBuf buf;
    curlbuf_init(&buf);

    char url[MAX_STR * 2];
    snprintf(url, sizeof(url), "imaps://%s:%d/%s",
             cfg->imap_server, cfg->imap_port, cfg->imap_folder);

    curl_easy_setopt(curl, CURLOPT_URL,           url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "UID SEARCH UNSEEN");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlbuf_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &buf);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        log_msg(LOG_ERROR, "imap: SEARCH UNSEEN falló: %s",
                curl_easy_strerror(res));
        curlbuf_free(&buf);
        return -1;
    }

    int count = 0;
    const char *p = strstr(buf.data, "* SEARCH");
    if (p) {
        p += 8;
        while (*p && count < max_uids) {
            while (*p == ' ') p++;
            if (!isdigit((unsigned char)*p)) break;
            int i = 0;
            while (isdigit((unsigned char)*p) && i < 63)
                uids_out[count][i++] = *p++;
            uids_out[count][i] = '\0';
            if (i > 0) count++;
        }
    }

    curlbuf_free(&buf);
    return count;
}

/* ------------------------------------------------------------------ */
/* Descarga de Subject y Date de un UID                                 */
/* ------------------------------------------------------------------ */

static int imap_fetch_subject(const Config *cfg, const char *uid,
                               char *subject_out, int subject_max,
                               char *date_out,    int date_max) {
    CURL *curl = curl_create(cfg);
    if (!curl) return -1;

    CurlBuf buf;
    curlbuf_init(&buf);

    char fetch_url[MAX_STR * 2];
    snprintf(fetch_url, sizeof(fetch_url),
             "imaps://%s:%d/%s/;UID=%s/;SECTION=HEADER",
             cfg->imap_server, cfg->imap_port, cfg->imap_folder, uid);

    curl_easy_setopt(curl, CURLOPT_URL,           fetch_url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlbuf_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &buf);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        log_msg(LOG_ERROR, "imap: FETCH header uid=%s falló: %s",
                uid, curl_easy_strerror(res));
        curlbuf_free(&buf);
        return -1;
    }

    /* Subject */
    char raw_subject[MAX_SUBJECT_LEN] = {0};
    parse_header_value(buf.data, "Subject", raw_subject, sizeof(raw_subject));
    decode_mime_subject(raw_subject, subject_out, subject_max);

    /* Date — extraer del mismo header ya descargado */
    char raw_date[128] = {0};
    parse_header_value(buf.data, "Date", raw_date, sizeof(raw_date));
    strncpy(date_out, raw_date, date_max - 1);
    date_out[date_max - 1] = '\0';

    curlbuf_free(&buf);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Descarga de cuerpo completo de un UID                                */
/* ------------------------------------------------------------------ */

static int imap_fetch_body(const Config *cfg, const char *uid,
                            CurlBuf *body_out) {
    CURL *curl = curl_create(cfg);
    if (!curl) return -1;

    char fetch_url[MAX_STR * 2];
    snprintf(fetch_url, sizeof(fetch_url),
             "imaps://%s:%d/%s/;UID=%s",
             cfg->imap_server, cfg->imap_port, cfg->imap_folder, uid);

    curl_easy_setopt(curl, CURLOPT_URL,           fetch_url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlbuf_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     body_out);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        log_msg(LOG_ERROR, "imap: FETCH body uid=%s falló: %s",
                uid, curl_easy_strerror(res));
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Clasificación de correos                                             */
/* ------------------------------------------------------------------ */

static int extract_camera_idx(const char *subject, const Config *cfg) {
    const char *marker = "INSTANTANEA CAMARA ID:";
    const char *p = strstr(subject, marker);
    if (!p) return -1;

    p += strlen(marker);

    char cam_id[16] = {0};
    int i = 0;
    while (*p && *p != ',' && *p != ')' && *p != ' ' && i < 15)
        cam_id[i++] = *p++;
    cam_id[i] = '\0';

    if (cam_id[0] == '\0') return -1;

    for (int c = 0; c < cfg->num_cameras; c++) {
        if (strcasecmp(cfg->camera_name[c], cam_id) == 0)
            return c;
    }

    log_msg(LOG_WARNING, "classify: cámara '%s' no encontrada en config", cam_id);
    return -1;
}

EmailType classify_email(const Email *email, const Config *cfg,
                          int *camera_idx_out) {
    *camera_idx_out = -1;

    if (strstr(email->subject, "INSTANTANEA CAMARA ID:")) {
        *camera_idx_out = extract_camera_idx(email->subject, cfg);
        return EMAIL_DETECTION;
    }

    for (int i = 0; i < cfg->num_alarm_subjects; i++) {
        if (strstr(email->subject, cfg->alarm_subjects[i])) {
            return EMAIL_ALARM_IMMEDIATE;
        }
    }

    return EMAIL_DISCARD;
}

/* ------------------------------------------------------------------ */
/* API pública                                                           */
/* ------------------------------------------------------------------ */

int imap_fetch_unread(AppContext *ctx, Email *emails, int max_emails) {
    const Config *cfg = &ctx->config;

    char (*uids)[64] = (char (*)[64])malloc(MAX_EMAILS_BATCH * 64);
    if (!uids) {
        log_msg(LOG_ERROR, "imap: sin memoria para uids");
        return -1;
    }

    int uid_count = imap_search_unseen(cfg, uids, MAX_EMAILS_BATCH);
    if (uid_count <= 0) {
        if (uid_count == 0)
            log_msg(LOG_DEBUG, "imap: no hay mensajes sin leer");
        free(uids);
        return uid_count;
    }

    log_msg(LOG_INFO, "imap: %d mensajes sin leer", uid_count);

    int fetched = 0;
    for (int i = 0; i < uid_count && fetched < max_emails; i++) {
        /* Abortar si se activó período de gracia */
        if (ctx->gracia_activa) {
            log_msg(LOG_INFO, "imap: abortando fetch por periodo de gracia");
            break;
        }
        Email *e = &emails[fetched];
        memset(e, 0, sizeof(Email));
        strncpy(e->uid, uids[i], 63);

        /* Obtener Subject y Date */
        if (imap_fetch_subject(cfg, uids[i],
                               e->subject, MAX_SUBJECT_LEN,
                               e->date, sizeof(e->date)) < 0)
            continue;

        log_msg(LOG_INFO, "imap: uid=%s date='%s' subject='%s'",
                e->uid, e->date, e->subject);
        /**
        char mail_date[128] = {0};
        if (imap_fetch_subject(cfg, uids[i],
                               e->subject, MAX_SUBJECT_LEN,
                               mail_date, sizeof(mail_date)) < 0)
            continue;

        log_msg(LOG_INFO, "imap: uid=%s date='%s' subject='%s'",
                e->uid, mail_date, e->subject); **/

        /* Clasificar */
        e->type = classify_email(e, cfg, &e->camera_idx);

        if (e->type == EMAIL_DISCARD) {
            /* Marcar como leído si es del sistema de cámaras */
            if (strstr(e->subject, "TIPO DE EVENTO"))
                imap_mark_read(ctx, e->uid);
            continue;
        }

        /* Si es detección con imágenes, descargar cuerpo y extraer adjuntos */
        if (e->type == EMAIL_DETECTION) {
            CurlBuf body;
            memset(&body, 0, sizeof(body));
            body.data = (char *)malloc(65536);
            if (!body.data) {
                log_msg(LOG_ERROR, "imap: sin memoria para body uid=%s", e->uid);
                continue;
            }
            body.size     = 0;
            body.capacity = 65536;
            body.data[0]  = '\0';

            if (imap_fetch_body(cfg, uids[i], &body) == 0)
                extract_image_attachments(body.data, body.size, e);
            curlbuf_free(&body);

            if (e->num_images == 0) {
                log_msg(LOG_WARNING,
                        "imap: correo sin imágenes uid=%s — descartado", e->uid);
                imap_mark_read(ctx, e->uid);
                continue;
            }
        }

        fetched++;
    }

    free(uids);
    return fetched;
}

int imap_mark_all_read(AppContext *ctx) {
    const Config *cfg = &ctx->config;
    CURL *curl = curl_create(cfg);
    if (!curl) return -1;

    CurlBuf buf;
    curlbuf_init(&buf);

    char url[MAX_STR * 2];
    snprintf(url, sizeof(url), "imaps://%s:%d/%s",
             cfg->imap_server, cfg->imap_port, cfg->imap_folder);

    curl_easy_setopt(curl, CURLOPT_URL,           url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "UID STORE 1:* +FLAGS (\\Seen)");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlbuf_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &buf);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    curlbuf_free(&buf);

    if (res != CURLE_OK) {
        log_msg(LOG_ERROR, "imap: mark_all_read falló: %s", curl_easy_strerror(res));
        return -1;
    }

    log_msg(LOG_INFO, "imap: todos los mensajes marcados como leídos");
    return 0;
}


int imap_mark_read(AppContext *ctx, const char *uid) {
    const Config *cfg = &ctx->config;
    CURL *curl = curl_create(cfg);
    if (!curl) return -1;

    CurlBuf buf;
    curlbuf_init(&buf);

    char url[MAX_STR * 2];
    snprintf(url, sizeof(url), "imaps://%s:%d/%s",
             cfg->imap_server, cfg->imap_port, cfg->imap_folder);

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "UID STORE %s +FLAGS (\\Seen)", uid);

    curl_easy_setopt(curl, CURLOPT_URL,           url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, cmd);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlbuf_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &buf);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    curlbuf_free(&buf);

    if (res != CURLE_OK) {
        log_msg(LOG_ERROR, "imap: STORE \\Seen uid=%s falló: %s",
                uid, curl_easy_strerror(res));
        return -1;
    }

    log_msg(LOG_DEBUG, "imap: marcado como leído uid=%s", uid);
    return 0;
}
