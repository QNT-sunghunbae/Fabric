#ifndef DEFECTCONTROLLER_H
#define DEFECTCONTROLLER_H

#include <QObject>
#include <QSharedPointer>
#include <QImage>
#include <QTimer>
#include <QElapsedTimer>
#include <QMutex>
#include <vector>
#include <map>
#include <atomic>
#include "Types.h"
#include "ImageSaverThread.h"

// 컨트롤러: 비즈니스 로직(불량 판정, 알람, 저장 관리) 전담
class DefectController : public QObject {
    Q_OBJECT

public:
    explicit DefectController(QObject* parent = nullptr);
    ~DefectController();

    // 초기화 및 설정
    void initializeSavers(const std::map<int, QString>& classNames);
    void startSession(const QString& baseSavePath);
    void stopSession();
    void setExceptionRatio(float ratio); // 슬라이더 값 수신

    // 상태 확인
    bool isSystemRunning() const { return m_systemRunning; }
    void setSystemRunning(bool running);
    
    // 로그 마무리를 위한 함수
    void finalizeLogFile();
    bool areSaversEmpty();
    int getTotalDefects() const { return m_stats.totalFabric + m_stats.totalFilm; }
    int getFabricDefects() const { return m_stats.totalFabric; }
    int getFilmDefects() const { return m_stats.totalFilm; }

public slots:
    // 핵심 입력: YOLO로부터 데이터 수신
    void processFrame(int idx, QSharedPointer<QImage> img, const std::vector<Detection>& dets);
    
    // FPS 업데이트 (CameraWorker로부터)
    void updateCameraFps(int fps);

private slots:
    void onSaverImageSaved(DefectItem item, int defectCount);
    void onSaverQueueEmpty();
    void updateAlarmTimerEffect(); // 깜빡임 타이머 틱

signals:
    // UI 갱신 요청 신호들
    void renderFrame(QSharedPointer<QImage> img, const std::vector<Detection>& dets);
    void alarmStateChanged(QString stateCode, QString message, QColor color); // NORMAL, SINGLE, MULTI
    void blinkStateUpdated(bool isVisible, QColor color); // 깜빡임 효과
    void statsUpdated(int fps, int yoloFps, int total, int fabric, int film);
    void newLogItem(DefectItem item);
    void saverStatusChanged(bool allEmpty);
    void buttonStateUpdated(bool hasDefect, bool hasSide, bool hasStaple);

private:
    void startAlarm(QColor color, int durationTick);
    void handleSaveLogic(QSharedPointer<QImage> img, const std::vector<Detection>& dets, bool inLogZone, float saveTop, float saveBottom);

    // 저장 관련
    static constexpr int NUM_SAVER_THREADS = 4;
    std::vector<ImageSaverThread*> m_savers;
    int m_currentSaverIdx = 0;
    
    // 세션 정보
    QString m_currentSessionPath;
    std::atomic<int> m_maxDefectsInSession{0};
    QMutex m_logMutex;
    std::atomic<bool> m_isLogFinalized{false};

    // 상태 변수
    bool m_systemRunning = false;
    enum DefectState { STATE_NONE, STATE_SINGLE, STATE_MULTI };
    DefectState m_currentOnScreenState = STATE_NONE;
    bool m_isAlarmDoneForCurrentState = false;
    
    // 알람 & 깜빡임
    QTimer* m_blinkTimer = nullptr;
    int m_alarmTimer = 0;
    bool m_blinkToggle = false;
    QColor m_currentAlarmColor;
    
    // 저장 로직 변수
    qint64 m_saveStartTime = 0;
    bool m_isSaveLimitReached = false;
    int m_lastSaveZoneCount = 0;
    int m_resetTimer = 0;
    
    // FPS 및 프레임
    long long m_totalFrameCount = 0;
    int m_frameCount = 0;
    int m_yoloFps = 0;
    int m_cameraFps = 0;
    QElapsedTimer m_yoloFpsTimer;
    
    // 설정값
    float m_exceptionRatio = 0.2f;
    
    // 통계
    struct Stats {
        int totalFabric = 0;
        int totalFilm = 0;
        void reset() { totalFabric = 0; totalFilm = 0; }
    } m_stats;
    
    QMutex m_mutex;
};

#endif // DEFECTCONTROLLER_H