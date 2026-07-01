#ifndef HARDWARE_H
#define HARDWARE_H

/* ============================================================
 * Definiciones de hardware fijas — NO modificar sin revisar
 * el circuito físico. Un error aquí puede dañar la RPi.
 * ============================================================ */

/* GPIO salida C — Frente (D3, D4) con demora configurable */
#define HW_GPIO_FRENTE_PIN   17

/* GPIO salida F — Fondo (D1, D2) sin demora */
#define HW_GPIO_FONDO_PIN    23

/* GPIO entrada — pulsador físico (pull-up externo 10K a 3.3V) */
#define HW_GPIO_BUTTON_PIN   27

/* GPIO salida — LED indicador de estado armado */
#define HW_GPIO_LED_PIN      25

/* GPIO entrada — boton de prueba (pull-up externo 10K a 3.3V) */
#define HW_GPIO_TEST_PIN     24

#endif /* HARDWARE_H */
