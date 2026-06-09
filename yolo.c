/*
 * yolo.c — Inferencia YOLOv8 usando ONNX Runtime C API
 *
 * El modelo YOLOv8n.onnx espera:
 *   Input:  float32 [1, 3, 640, 640]  (RGB normalizado 0..1)
 *   Output: float32 [1, 84, 8400]
 *           84 = 4 coords (cx,cy,w,h) + 80 clases COCO
 *           clase 0 = "person"
 *
 * Dependencias:
 *   - onnxruntime (descargado de github.com/microsoft/onnxruntime/releases)
 *   - stb_image (header-only, incluido en third_party/stb_image.h)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "alarm_processor.h"

/* stb_image — header-only, incluir UNA sola vez con la implementación */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_THREAD_LOCALS
#include "third_party/stb_image.h"

/* ONNX Runtime C API */
#include <onnxruntime_c_api.h>

/* ------------------------------------------------------------------ */
/* Constantes YOLOv8                                                    */
/* ------------------------------------------------------------------ */

#define YOLO_INPUT_W      640
#define YOLO_INPUT_H      640
#define YOLO_NUM_CLASSES  80
#define YOLO_PERSON_CLASS 0
#define YOLO_BOXES        8400  /* número de propuestas */
#define YOLO_ROWS         84    /* 4 coords + 80 clases */

/* ------------------------------------------------------------------ */
/* Helpers ORT                                                          */
/* ------------------------------------------------------------------ */

static const OrtApi *g_ort = NULL;

#define ORT_CHECK(expr)                                             \
    do {                                                            \
        OrtStatus *_s = (expr);                                     \
        if (_s) {                                                   \
            const char *_msg = g_ort->GetErrorMessage(_s);         \
            log_msg(LOG_ERROR, "ORT: %s", _msg);                   \
            g_ort->ReleaseStatus(_s);                               \
            return -1;                                              \
        }                                                           \
    } while (0)

/* ------------------------------------------------------------------ */
/* Preproceso: JPEG/PNG → float32 [1,3,640,640]                        */
/* ------------------------------------------------------------------ */

/*
 * Carga imagen desde memoria, la escala a 640×640 con letterboxing
 * y la convierte a float RGB normalizado [0..1] en formato CHW.
 * Retorna buffer asignado con malloc (caller libera).
 */
static float *preprocess_image(const uint8_t *img_bytes, size_t img_size,
                                 int *ok) {
    *ok = 0;

    int w, h, ch;
    uint8_t *img = stbi_load_from_memory(img_bytes, (int)img_size,
                                          &w, &h, &ch, 3);
    if (!img) {
        log_msg(LOG_WARNING, "yolo: stbi_load falló: %s", stbi_failure_reason());
        return NULL;
    }

    /* Allocar buffer destino CHW */
    size_t tensor_size = 3 * YOLO_INPUT_H * YOLO_INPUT_W;
    float *tensor = (float *)calloc(tensor_size, sizeof(float));
    if (!tensor) { stbi_image_free(img); return NULL; }

    /* Letterbox: escalar manteniendo aspect ratio, rellenar con gris (114) */
    float scale = fminf((float)YOLO_INPUT_W / w, (float)YOLO_INPUT_H / h);
    int new_w   = (int)(w * scale);
    int new_h   = (int)(h * scale);
    int pad_x   = (YOLO_INPUT_W - new_w) / 2;
    int pad_y   = (YOLO_INPUT_H - new_h) / 2;

    /* Rellenar con gris */
    for (size_t i = 0; i < tensor_size; i++)
        tensor[i] = 114.0f / 255.0f;

    /* Escalar bilineal simple y copiar */
    for (int y = 0; y < new_h; y++) {
        for (int x = 0; x < new_w; x++) {
            /* coordenada en imagen original */
            int src_x = (int)(x / scale);
            int src_y = (int)(y / scale);
            src_x = src_x < w ? src_x : w - 1;
            src_y = src_y < h ? src_y : h - 1;

            uint8_t *px = img + (src_y * w + src_x) * 3;

            int dst_y = y + pad_y;
            int dst_x = x + pad_x;
            if (dst_y >= YOLO_INPUT_H || dst_x >= YOLO_INPUT_W) continue;

            /* CHW: canal, fila, col */
            tensor[0 * YOLO_INPUT_H * YOLO_INPUT_W + dst_y * YOLO_INPUT_W + dst_x]
                = px[0] / 255.0f;
            tensor[1 * YOLO_INPUT_H * YOLO_INPUT_W + dst_y * YOLO_INPUT_W + dst_x]
                = px[1] / 255.0f;
            tensor[2 * YOLO_INPUT_H * YOLO_INPUT_W + dst_y * YOLO_INPUT_W + dst_x]
                = px[2] / 255.0f;
        }
    }

    stbi_image_free(img);
    *ok = 1;
    return tensor;
}

/* ------------------------------------------------------------------ */
/* Postproceso: output [1,84,8400] → YoloResult                        */
/* ------------------------------------------------------------------ */

static void postprocess(const float *output, float conf_threshold,
                         YoloResult *result) {
    result->person_detected = 0;
    result->person_count    = 0;
    result->max_confidence  = 0.0f;

    /*
     * output shape: [1, 84, 8400]
     * Para acceder a la clase c de la propuesta b:
     *   output[ (4 + c) * YOLO_BOXES + b ]
     * La confianza de "person" (clase 0) está en columna 4.
     */
    for (int b = 0; b < YOLO_BOXES; b++) {
        float person_score = output[(4 + YOLO_PERSON_CLASS) * YOLO_BOXES + b];

        if (person_score >= conf_threshold) {
            result->person_detected = 1;
            result->person_count++;
            if (person_score > result->max_confidence)
                result->max_confidence = person_score;
        }
    }
}

/* ------------------------------------------------------------------ */
/* API pública                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    OrtEnv      *env;
    OrtSession  *session;
    OrtMemoryInfo *memory_info;
} OrtCtx;

int yolo_init(AppContext *ctx) {
    g_ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!g_ort) {
        log_msg(LOG_ERROR, "yolo: no se pudo obtener ORT API");
        return -1;
    }

    OrtCtx *ortctx = (OrtCtx *)calloc(1, sizeof(OrtCtx));
    if (!ortctx) return -1;

    /* Entorno */
    OrtStatus *status;
    status = g_ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "alarm", &ortctx->env);
    if (status) {
        log_msg(LOG_ERROR, "yolo: CreateEnv: %s", g_ort->GetErrorMessage(status));
        g_ort->ReleaseStatus(status);
        free(ortctx);
        return -1;
    }

    /* Opciones de sesión */
    OrtSessionOptions *opts;
    ORT_CHECK(g_ort->CreateSessionOptions(&opts));
    /* Número de threads = 2 para no saturar la Raspberry Pi */
    g_ort->SetIntraOpNumThreads(opts, 2);
    g_ort->SetInterOpNumThreads(opts, 1);
    g_ort->SetSessionGraphOptimizationLevel(opts, ORT_ENABLE_ALL);

    /* Cargar modelo */
    status = g_ort->CreateSession(ortctx->env,
                                   ctx->config.yolo_model,
                                   opts,
                                   &ortctx->session);
    g_ort->ReleaseSessionOptions(opts);
    if (status) {
        log_msg(LOG_ERROR, "yolo: CreateSession '%s': %s",
                ctx->config.yolo_model, g_ort->GetErrorMessage(status));
        g_ort->ReleaseStatus(status);
        g_ort->ReleaseEnv(ortctx->env);
        free(ortctx);
        return -1;
    }

    /* MemoryInfo para CPU */
    ORT_CHECK(g_ort->CreateCpuMemoryInfo(OrtArenaAllocator,
                                          OrtMemTypeDefault,
                                          &ortctx->memory_info));

    ctx->onnx_session = ortctx->session;
    ctx->onnx_env     = ortctx->env;

    /* Guardamos el puntero completo en onnx_env (reutilizamos el campo) */
    ctx->onnx_env     = ortctx; /* cast void* */

    log_msg(LOG_INFO, "yolo: modelo '%s' cargado OK", ctx->config.yolo_model);
    return 0;
}

int yolo_analyze(AppContext *ctx, const ImageBuffer *img, YoloResult *result) {
    OrtCtx *ortctx = (OrtCtx *)ctx->onnx_env;

    /* Preprocesar imagen */
    int ok = 0;
    float *input_tensor = preprocess_image(img->data, img->size, &ok);
    if (!ok || !input_tensor) {
        log_msg(LOG_WARNING, "yolo: no se pudo preprocesar imagen");
        result->person_detected = 0;
        result->person_count    = 0;
        result->max_confidence  = 0.0f;
        return -1;
    }

    /* Crear tensor de entrada */
    int64_t input_shape[] = { 1, 3, YOLO_INPUT_H, YOLO_INPUT_W };
    size_t  input_elems   = 1 * 3 * YOLO_INPUT_H * YOLO_INPUT_W;

    OrtValue *input_val = NULL;
    OrtStatus *status = g_ort->CreateTensorWithDataAsOrtValue(
        ortctx->memory_info,
        input_tensor,
        input_elems * sizeof(float),
        input_shape, 4,
        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
        &input_val);

    if (status) {
        log_msg(LOG_ERROR, "yolo: CreateTensor: %s",
                g_ort->GetErrorMessage(status));
        g_ort->ReleaseStatus(status);
        free(input_tensor);
        return -1;
    }

    /* Nombres de entrada/salida (YOLOv8 exportado con ultralytics) */
    const char *input_names[]  = { "images" };
    const char *output_names[] = { "output0" };

    OrtValue *output_val = NULL;
    status = g_ort->Run(ortctx->session,
                         NULL,
                         input_names,  &input_val,  1,
                         output_names, 1, &output_val);

    g_ort->ReleaseValue(input_val);
    free(input_tensor);

    if (status) {
        log_msg(LOG_ERROR, "yolo: Run: %s", g_ort->GetErrorMessage(status));
        g_ort->ReleaseStatus(status);
        return -1;
    }

    /* Obtener puntero a datos de salida */
    float *output_data = NULL;
    ORT_CHECK(g_ort->GetTensorMutableData(output_val, (void **)&output_data));

    postprocess(output_data, ctx->config.yolo_confidence, result);

    g_ort->ReleaseValue(output_val);

    log_msg(LOG_DEBUG, "yolo: persona=%d  count=%d  conf=%.3f",
            result->person_detected, result->person_count,
            result->max_confidence);

    return 0;
}

void yolo_cleanup(AppContext *ctx) {
    if (!ctx->onnx_env) return;
    OrtCtx *ortctx = (OrtCtx *)ctx->onnx_env;

    if (ortctx->memory_info) g_ort->ReleaseMemoryInfo(ortctx->memory_info);
    if (ortctx->session)     g_ort->ReleaseSession(ortctx->session);
    if (ortctx->env)         g_ort->ReleaseEnv(ortctx->env);

    free(ortctx);
    ctx->onnx_env = NULL;
    ctx->onnx_session = NULL;
    log_msg(LOG_INFO, "yolo: recursos liberados");
}
