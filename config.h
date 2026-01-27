#ifndef CONFIG_H
#define CONFIG_H

#include <QString>

namespace Config {
    extern float REAL_WIDTH_MM;
    extern float REAL_HEIGHT_MM;
    extern float FABRIC_WIDTH_MM;

    extern int NUM_CAMERAS;
    extern int CAMERA_WIDTH;
    extern int CAMERA_HEIGHT;
    extern int CAMERA_SPEED_MODE;
    extern int CAMERA_FPS;

    extern int DISPLAY_WIDTH;
    extern int DISPLAY_HEIGHT;
    extern int DISPLAY_FPS;
    extern int UI_INTERVAL_MS;
    extern int STATS_UPDATE_MS;

    extern int TARGET_CLASS_ID;
    extern int YOLO_INTERVAL;
    extern float CONFIDENCE_THRESHOLD;

    extern int ALERT_HOLD_MS;
    extern QString DEFAULT_SAVE_PATH;
    extern QString YOLO_CONFIG_PATH;

    void load();
}

#endif // CONFIG_H