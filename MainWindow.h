#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QListWidget>
#include <QTimer>
#include <QImage>
#include <QSharedPointer>
#include <memory>
#include <vector>
#include <map>

#include "Types.h"
#include "config.h"
#include "CameraApi.h"
#include "DeepStreamYOLO.h"
#include "DefectController.h" // [New] 컨트롤러 헤더 포함

// ==========================================================================
// UI 관련 보조 클래스 (기존 유지)
// ==========================================================================
class AspectRatioLabel : public QLabel {
    Q_OBJECT
public:
    explicit AspectRatioLabel(const QString& text, QWidget* parent = nullptr);
    void updateImage(const QImage& img);
protected:
    void resizeEvent(QResizeEvent* event) override;
private:
    QImage m_originalImage;
};

// ==========================================================================
// CameraWorker 클래스 (기존 유지)
// ==========================================================================
class CameraWorker : public QObject {
    Q_OBJECT
public:
    explicit CameraWorker(int cameraIndex, QObject* parent = nullptr);
    ~CameraWorker();
    bool initialize();
    void start();
    void stop();
    int getFPS() const { return m_currentFps.load(); }
    void setYOLO(DeepStreamYOLO* yolo) { m_yolo = yolo; }

signals:
    void fpsUpdated(int fps); // [Added] FPS 변경 시 신호 발송

private:
    void captureLoop();

    int m_cameraIndex;
    int m_hCamera = -1;
    tSdkCameraCapbility m_tCapability; 
    std::vector<unsigned char> m_rgbBuffer;
    std::atomic<bool> m_running{false};
    QThread* m_thread = nullptr;
    DeepStreamYOLO* m_yolo = nullptr;
    
    std::atomic<int> m_currentFps{0};
    int m_frameCounter = 0;
    QElapsedTimer m_fpsTimer;
};

// ==========================================================================
// MainWindow 클래스 (View 전담)
// ==========================================================================
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

private slots:
    // 사용자 입력 핸들러
    void onBtnStartStop();
    void onBtnExit();
    void onBtnPlus();
    void onBtnMinus();
    void onRatioChanged(int value);
    void onLogClear();
    void onNavPrev();
    void onNavNext();
    
    // [New] Controller로부터 받는 신호 처리 (View 업데이트용)
    void onRenderFrame(QSharedPointer<QImage> img, const std::vector<Detection>& dets);
    void onAlarmStateChanged(QString stateCode, QString message, QColor color);
    void onBlinkStateUpdated(bool isVisible, QColor color);
    void onStatsUpdated(int fps, int yoloFps, int total, int fabric, int film);
    void onNewLogItem(DefectItem item);
    void onSaverStatusChanged(bool allEmpty);
    void onButtonStateUpdated(bool hasDefect, bool hasSide, bool hasStaple); // (선택 구현)
    
    // FPS 보정 (기존 유지)
    void onCalibrationTimeout();

private:
    void setupUI();
    void setupCameras();
    void loadClassList();
    void startFPSCalibration();
    
    // UI 헬퍼
    void updateLogViewer();
    void addLogItemToWidget(const DefectItem& item);
    QImage drawOverlay(const QImage& source, const std::vector<Detection>& dets);
    QString getLabelName(int classId);

    // --------------------------------------------------------
    // [Refactored] 핵심 멤버: 컨트롤러
    // --------------------------------------------------------
    DefectController* m_controller = nullptr;
    
    // 하드웨어 관리
    std::unique_ptr<CameraWorker> m_cameraWorker;
    std::unique_ptr<DeepStreamYOLO> m_yolo;

    // UI 위젯들
    QLabel* m_timeLabel = nullptr;
    AspectRatioLabel* m_cameraLabel = nullptr;
    QPushButton* m_btnStartStop = nullptr;
    QPushButton* m_btnExit = nullptr;
    QFrame* m_alertFrame = nullptr;
    QLabel* m_alertLabel = nullptr;
    QSlider* m_ratioSlider = nullptr;
    QLabel* m_ratioLabel = nullptr;
    QPushButton* m_btnMinus = nullptr;
    QPushButton* m_btnPlus = nullptr;
    QListWidget* m_logWidget = nullptr;
    QPushButton* m_btnLogClear = nullptr;
    AspectRatioLabel* m_latestImage = nullptr;
    AspectRatioLabel* m_historyImg1 = nullptr;
    AspectRatioLabel* m_historyImg2 = nullptr;
    AspectRatioLabel* m_historyImg3 = nullptr;
    QPushButton* m_btnNavPrev = nullptr;
    QPushButton* m_btnNavNext = nullptr;
    QLabel* m_navCountLabel = nullptr;
    
    QLabel* m_lblTotalValue = nullptr;
    QLabel* m_lblFabricValue = nullptr;
    QLabel* m_lblFilmValue = nullptr;
    QLabel* m_lblTotalTitle = nullptr;
    QLabel* m_lblFabricTitle = nullptr;
    QLabel* m_lblFilmTitle = nullptr;

    QPushButton* m_btnLining = nullptr;
    QPushButton* m_btnSide = nullptr;
    QPushButton* m_btnStapler = nullptr;

    QString m_styleExitActive;
    QString m_styleExitInactive;
    
    // 히스토리 관리 (UI 표시용)
    static constexpr int MAX_LOG_SIZE = 1000;
    std::vector<DefectItem> m_history;
    int m_currentIndex = -1;

    // 설정값
    QString m_baseSavePath;
    std::map<int, QString> m_classNames;
    
    // 타이머
    QTimer* m_uiTimer = nullptr;
    QTimer* m_calibrationTimer = nullptr;
    QTimer* m_exitTimer = nullptr;

    // 교정용 변수
    int m_calibrationStep = 0;
    int m_measuredFrameCount = 0;
    QElapsedTimer m_fpsTimer;
};

#endif // MAINWINDOW_H