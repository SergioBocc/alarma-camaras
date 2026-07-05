# Alarma Cámaras con IA

Sistema de filtro de alarmas con detección de cuerpo humano usando YOLOv8, instalado en Raspberry Pi 4.

## Descripción

El sistema lee correos del NVR de cámaras, extrae las imágenes adjuntas y las analiza con YOLOv8 para confirmar si hay una figura humana antes de activar el panel de alarma de la casa. Filtra falsos positivos causados por gatos, pájaros y plantas en movimiento.

---

## Ayuda Memoria — Operación del Daemon

### Ver estado del daemon
```bash
sudo systemctl status alarm_processor
```

### Ver logs en tiempo real
```bash
tail -f /var/log/alarm_processor.log
```

### Ver detecciones positivas (tabla TSV)
```bash
cat /var/log/alarma_detecciones.tsv
```

### Detener / iniciar / reiniciar el daemon
```bash
sudo systemctl stop alarm_processor
sudo systemctl start alarm_processor
sudo systemctl restart alarm_processor
```

### Verificar si arranca automáticamente con la RPi
```bash
sudo systemctl is-enabled alarm_processor
```

### Habilitar / deshabilitar arranque automático
```bash
sudo systemctl enable alarm_processor
sudo systemctl disable alarm_processor
```

---

## Ayuda Memoria — Editar Configuración

### Editar parámetros (sin recompilar)
```bash
sudo systemctl stop alarm_processor
nano /home/pi/alarma_camaras/config.ini
sudo systemctl start alarm_processor
```

### Parámetros más importantes en config.ini

| Parámetro | Descripción | Valor actual |
|-----------|-------------|--------------|
| `confianza` | Umbral de confianza YOLO (0.0 a 1.0) | 0.40 |
| `umbral_detecciones` | Detecciones positivas para disparar alarma | 2 |
| `tiempo_minutos` | Duración de la ventana de ráfaga | 2 |
| `demora_frente_seg` | Demora antes de activar GPIO Frente | 30 |
| `demora_armado_seg` | Espera al armar antes de limpiar correos | 60 |
| `intervalo_minimo_seg` | Intervalo mínimo entre consultas IMAP | 15 |
| `duracion_pulso` | Duración del pulso GPIO en segundos | 1 |

### Editar el servicio systemd
```bash
sudo nano /etc/systemd/system/alarm_processor.service
sudo systemctl daemon-reload
```

---

## Ayuda Memoria — Modificar el Código Fuente

### 1. Editar el código fuente
```bash
nano /home/pi/alarma_camaras/nombre_archivo.c
```

### 2. Compilar
```bash
cd /home/pi/alarma_camaras
make clean && make
```

### 3. Instalar y reiniciar el daemon
```bash
sudo make install
sudo systemctl restart alarm_processor
```

### 4. Verificar que está corriendo
```bash
sudo systemctl status alarm_processor
```

---

## Ayuda Memoria — Respaldo en GitHub

```bash
cd /home/pi/alarma_camaras
git add .
git commit -m "descripción del cambio"
git push origin main
```

Si GitHub rechaza el push por cambios remotos:
```bash
git pull --rebase origin main
git push origin main
```

### Respaldo local completo
```bash
/home/pi/backup_alarma.sh
```

---

## Pines GPIO (definidos en hardware.h — NO editar en config.ini)

| Pin BCM | Pin físico | Uso |
|---------|------------|-----|
| 17 | 11 | Salida: alarma Frente (con demora) |
| 23 | 16 | Salida: alarma Fondo (inmediato) |
| 25 | 22 | Salida: LED indicador armado |
| 27 | 13 | Entrada: pulsador físico armado |
| 24 | 18 | Entrada: pulsador de prueba |

---

## Archivos importantes

| Archivo | Ubicación | Descripción |
|---------|-----------|-------------|
| Configuración | `/home/pi/alarma_camaras/config.ini` | Parámetros del sistema |
| Log operacional | `/var/log/alarm_processor.log` | Eventos del sistema |
| Log detecciones | `/var/log/alarma_detecciones.tsv` | Tabla de detecciones positivas |
| Imágenes guardadas | `/var/log/alarma_imagenes/` | Imágenes de cada detección |
| Modelo YOLO | `/home/pi/models/yolov8n.onnx` | Red neuronal de detección |
| Ejecutable daemon | `/usr/local/bin/alarm_processor` | Binario en producción |
| Fuentes | `/home/pi/alarma_camaras/` | Código fuente C |

---

## Usuarios MQTT en EMQX

| Usuario | Dispositivo |
|---------|-------------|
| cam_rpi | Raspberry Pi |
| cam_sergio | iPhone Sergio |
| cam_pedro | iPhone Pedro |
| cam_malva | iPhone Malva |
| cam_agustina | iPhone Agustina |
| cam_maxi | iPhone Maxi |
| cam_android | Android |

Para revocar acceso de un dispositivo perdido: entrar a **cloud.emqx.com → HomeAutomation_broker → Access Control → Authentication** y cambiar la password del usuario correspondiente.

---

## Interfaz web iPhone

URL de acceso por usuario:
```
https://sergiobocc.github.io/alarma-camaras/web/Camaras.sergio.html
https://sergiobocc.github.io/alarma-camaras/web/Camaras.pedro.html
```

Para agregar a pantalla de inicio: Safari → Compartir → Agregar a pantalla de inicio.

---

## Repositorio GitHub

```
https://github.com/SergioBocc/alarma-camaras
```
