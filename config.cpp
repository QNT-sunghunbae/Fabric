#include "config.h"
#include <QSettings>
#include <QFile>
#include <QCoreApplication>
#include <QDir>
#include <QDebug>

namespace Config {
    float REAL_WIDTH_MM     = 71.0f;
    float REAL_HEIGHT_MM    = 52.0f;
    float FABRIC_WIDTH_MM   = 65.0f;

    int NUM_CAMERAS   = 1;
    int CAMERA_WIDTH  = 640;
    int CAMERA_HEIGHT = 480;
    int CAMERA_SPEED_MODE = 2;
    int CAMERA_FPS    = 30;

    int DISPLAY_WIDTH    = 640;
    int DISPLAY_HEIGHT   = 480;
    int DISPLAY_FPS      = -1;
    int UI_INTERVAL_MS   = 1;
    int STATS_UPDATE_MS  = 500;

    int TARGET_CLASS_ID  = -1;
    int YOLO_INTERVAL    = 0;
    float CONFIDENCE_THRESHOLD = 0.5f;

    int ALERT_HOLD_MS = 300;
    QString DEFAULT_SAVE_PATH = "../logs";
    QString YOLO_CONFIG_PATH = "assets/config_infer_primary_yoloV11.txt";

    void load() {
        QString appPath = QCoreApplication::applicationDirPath();
        QString configPath = QDir::cleanPath(appPath + "/config.ini");

        if (!QFile::exists(configPath)) {
            QSettings settings(configPath, QSettings::IniFormat);
            settings.setValue("Dimension/RealWidthMM", REAL_WIDTH_MM);
            settings.setValue("Dimension/RealHeightMM", REAL_HEIGHT_MM);
            settings.setValue("Dimension/FabricWidthMM", FABRIC_WIDTH_MM);

            settings.setValue("Camera/NumCameras", NUM_CAMERAS);
            settings.setValue("Camera/Width", CAMERA_WIDTH);
            settings.setValue("Camera/Height", CAMERA_HEIGHT);
            settings.setValue("Camera/SpeedMode", CAMERA_SPEED_MODE);
            settings.setValue("Camera/FPS", CAMERA_FPS);

            // ⭐ [핵심 변경] DISPLAY_FPS 기본값을 30으로 설정
            settings.setValue("Display/DisplayFPS", 30);  // -1 → 30
            settings.setValue("Display/StatsUpdateMS", STATS_UPDATE_MS);

            settings.setValue("YOLO/TargetClassID", TARGET_CLASS_ID);
            settings.setValue("YOLO/ConfidenceThreshold", CONFIDENCE_THRESHOLD);
            settings.setValue("YOLO/ConfigPath", YOLO_CONFIG_PATH);

            settings.setValue("System/AlertHoldMS", ALERT_HOLD_MS);
            settings.setValue("System/SavePath", DEFAULT_SAVE_PATH);
        }

        QSettings settings(configPath, QSettings::IniFormat);
        
        REAL_WIDTH_MM = settings.value("Dimension/RealWidthMM", 71.0f).toFloat();
        REAL_HEIGHT_MM = settings.value("Dimension/RealHeightMM", 52.0f).toFloat();
        FABRIC_WIDTH_MM = settings.value("Dimension/FabricWidthMM", 65.0f).toFloat();

        NUM_CAMERAS = settings.value("Camera/NumCameras", 1).toInt();
        CAMERA_WIDTH = settings.value("Camera/Width", 640).toInt();
        CAMERA_HEIGHT = settings.value("Camera/Height", 480).toInt();
        CAMERA_SPEED_MODE = settings.value("Camera/SpeedMode", 2).toInt();
        CAMERA_FPS = settings.value("Camera/FPS", 30).toInt();

        // ⭐ [핵심 변경] DISPLAY_FPS 기본값을 30으로
        DISPLAY_FPS = settings.value("Display/DisplayFPS", 30).toInt();
        
        // ⭐ UI 간격 계산 (최소 16ms = 60 FPS, 최대 33ms = 30 FPS)
        if (DISPLAY_FPS > 0) {
            UI_INTERVAL_MS = qMax(16, 1000 / DISPLAY_FPS);  // 최소 16ms 보장
        } else {
            UI_INTERVAL_MS = 33;  // 기본 30 FPS
        }
        
        STATS_UPDATE_MS = settings.value("Display/StatsUpdateMS", 500).toInt();

        TARGET_CLASS_ID = settings.value("YOLO/TargetClassID", 74).toInt();
        CONFIDENCE_THRESHOLD = settings.value("YOLO/ConfidenceThreshold", 0.5f).toFloat();
        YOLO_CONFIG_PATH = settings.value("YOLO/ConfigPath", "assets/config_infer_primary_yoloV11.txt").toString();

        ALERT_HOLD_MS = settings.value("System/AlertHoldMS", 300).toInt();
        DEFAULT_SAVE_PATH = settings.value("System/SavePath", "../logs").toString();
        
        qDebug() << "Config loaded from:" << configPath;
        qDebug() << "[CONFIG] UI_INTERVAL_MS:" << UI_INTERVAL_MS << "ms (" << (1000.0 / UI_INTERVAL_MS) << "FPS)";
    }
}