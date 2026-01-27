#include "MainWindow.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QPainter>
#include <QFont>
#include <QCoreApplication>
#include <QDesktopWidget>
#include <QApplication>
#include <QDebug>
#include <QtConcurrent>
#include <QFuture>
#include <QFutureWatcher>
#include <unistd.h>
#include <malloc.h>

// ==========================================================================
// AspectRatioLabel Implementation (변경 없음)
// ==========================================================================
AspectRatioLabel::AspectRatioLabel(const QString& text, QWidget* parent)
    : QLabel(text, parent) {
    setAlignment(Qt::AlignCenter);
    setStyleSheet("background-color: transparent; color: white;");
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    setScaledContents(false); 
}

void AspectRatioLabel::updateImage(const QImage& img) {
    if (img.isNull()) return;
    m_originalImage = img;
    resizeEvent(nullptr); 
}

void AspectRatioLabel::resizeEvent(QResizeEvent* event) {
    QLabel::resizeEvent(event);
    if (m_originalImage.isNull()) return;
    
    QSize labelSize = size();
    QSize imageSize = m_originalImage.size();
    float scaleW = (float)labelSize.width() / imageSize.width();
    float scaleH = (float)labelSize.height() / imageSize.height();
    float scale = qMin(scaleW, scaleH);
    int newWidth = (int)(imageSize.width() * scale);
    int newHeight = (int)(imageSize.height() * scale);
    
    QPixmap scaledPixmap = QPixmap::fromImage(m_originalImage).scaled(
        newWidth, newHeight, Qt::KeepAspectRatio, Qt::SmoothTransformation
    );
    setPixmap(scaledPixmap);
}

// ==========================================================================
// CameraWorker Implementation (변경 없음)
// ==========================================================================
CameraWorker::CameraWorker(int cameraIndex, QObject* parent)
    : QObject(parent), m_cameraIndex(cameraIndex) {}

CameraWorker::~CameraWorker() { stop(); }

bool CameraWorker::initialize() {
    CameraSdkInit(1);
    tSdkCameraDevInfo tCameraEnumList[4];
    int iCameraCounts = 4;
    CameraEnumerateDevice(tCameraEnumList, &iCameraCounts);
    if (iCameraCounts == 0) return false;
    if (CameraInit(&tCameraEnumList[0], -1, -1, &m_hCamera) != CAMERA_STATUS_SUCCESS) return false;
    
    CameraGetCapability(m_hCamera, &m_tCapability);
    if (Config::CAMERA_SPEED_MODE < m_tCapability.iFrameSpeedDesc) {
        CameraSetFrameSpeed(m_hCamera, Config::CAMERA_SPEED_MODE);
    }
    CameraSetAeState(m_hCamera, TRUE);
    
    int bufferSize = m_tCapability.sResolutionRange.iWidthMax * m_tCapability.sResolutionRange.iHeightMax * 3;

    try {
        m_rgbBuffer.resize(bufferSize); // [수정] 벡터 크기 설정 (메모리 자동 할당)
    } catch (...) {
        return false; // 메모리 부족 시 예외 처리
    }
    
    if (m_tCapability.sIspCapacity.bMonoSensor) CameraSetIspOutFormat(m_hCamera, CAMERA_MEDIA_TYPE_MONO8);
    else CameraSetIspOutFormat(m_hCamera, CAMERA_MEDIA_TYPE_RGB8);
    
    CameraSetTriggerMode(m_hCamera, 0);
    CameraPlay(m_hCamera);
    return true;
}

void CameraWorker::start() {
    if (m_running.load()) return;
    m_running.store(true);
    m_frameCounter = 0;
    m_fpsTimer.start();
    m_thread = QThread::create([this]() { captureLoop(); });
    m_thread->start();
}

void CameraWorker::stop() {
    m_running.store(false);
    if (m_thread) { m_thread->quit(); m_thread->wait(); delete m_thread; m_thread = nullptr; }
    if (m_hCamera > 0) { CameraUnInit(m_hCamera); m_hCamera = -1; }
    // if (m_pRgbBuffer) { free(m_pRgbBuffer); m_pRgbBuffer = nullptr; }
}

void CameraWorker::captureLoop() {
    tSdkFrameHead s;
    BYTE* b;
    while (m_running.load()) {
        if (CameraGetImageBuffer(m_hCamera, &s, &b, 200) == CAMERA_STATUS_SUCCESS) {
            m_frameCounter++;
            if (m_fpsTimer.elapsed() >= 1000) {
                int fps = m_frameCounter;
                m_currentFps.store(fps);
                emit fpsUpdated(fps); // FPS 신호 발생
                m_frameCounter = 0;
                m_fpsTimer.restart();
            }
            CameraImageProcess(m_hCamera, b, m_rgbBuffer.data(), &s);
            CameraReleaseImageBuffer(m_hCamera, b);
            if (m_yolo) m_yolo->pushFrame(m_cameraIndex, m_rgbBuffer.data(), s.iWidth, s.iHeight, QDateTime::currentMSecsSinceEpoch());
        } else {
            usleep(1000);
        }
    }
}

// ==========================================================================
// MainWindow Implementation (Refactored)
// ==========================================================================
MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    qRegisterMetaType<DefectItem>("DefectItem");
    qRegisterMetaType<QSharedPointer<QImage>>("QSharedPointer<QImage>");
    
    QString appDir = QCoreApplication::applicationDirPath();
    m_baseSavePath = QDir::cleanPath(appDir + QDir::separator() + Config::DEFAULT_SAVE_PATH);
    
    loadClassList();
    
    // 1. 컨트롤러 생성 및 초기화
    m_controller = new DefectController(this);
    m_controller->initializeSavers(m_classNames);
    
    // 2. 컨트롤러 -> UI 신호 연결
    connect(m_controller, &DefectController::renderFrame, this, &MainWindow::onRenderFrame);
    connect(m_controller, &DefectController::alarmStateChanged, this, &MainWindow::onAlarmStateChanged);
    connect(m_controller, &DefectController::blinkStateUpdated, this, &MainWindow::onBlinkStateUpdated);
    connect(m_controller, &DefectController::statsUpdated, this, &MainWindow::onStatsUpdated);
    connect(m_controller, &DefectController::newLogItem, this, &MainWindow::onNewLogItem);
    connect(m_controller, &DefectController::saverStatusChanged, this, &MainWindow::onSaverStatusChanged);
    
    setupUI();
    setupCameras();
    
    // 시간 표시용 타이머
    m_uiTimer = new QTimer(this);
    connect(m_uiTimer, &QTimer::timeout, this, [this]() {
        if (m_timeLabel) m_timeLabel->setText(QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss"));
    });
    m_uiTimer->start(1000);
    
    m_calibrationTimer = new QTimer(this);
    connect(m_calibrationTimer, &QTimer::timeout, this, &MainWindow::onCalibrationTimeout);
    
    showFullScreen();
}

MainWindow::~MainWindow() {
    if (m_cameraWorker) m_cameraWorker->stop();
    if (m_yolo) m_yolo->stop();
    // Controller는 QObject parent로 인해 자동 삭제됨
}

void MainWindow::loadClassList() {
    QString appDir = QCoreApplication::applicationDirPath();
    QString labelPath = QDir::cleanPath(appDir + QDir::separator() + "assets/labels2.txt");
    QFile file(labelPath);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&file);
        int id = 0;
        while (!in.atEnd()) {
            QString line = in.readLine().trimmed();
            if (!line.isEmpty()) m_classNames[id] = line;
            id++;
        }
        file.close();
    }
}

void MainWindow::setupCameras() {
    m_cameraWorker = std::make_unique<CameraWorker>(0);
    m_yolo = std::make_unique<DeepStreamYOLO>(this);
    
    m_cameraWorker->setYOLO(m_yolo.get());
    
    // [핵심] DeepStream -> Controller 연결
    connect(m_yolo.get(), &DeepStreamYOLO::frameProcessed, m_controller, &DefectController::processFrame, Qt::QueuedConnection);
    
    // Camera FPS -> Controller 연결
    connect(m_cameraWorker.get(), &CameraWorker::fpsUpdated, m_controller, &DefectController::updateCameraFps);
}

// ==========================================================================
// [View Slots]
// ==========================================================================

void MainWindow::onRenderFrame(QSharedPointer<QImage> img, const std::vector<Detection>& dets) {
    if (!img || img->isNull()) return;
    
    // 오버레이 그리기
    QImage composedImg = drawOverlay(*img, dets);
    
    if (m_cameraLabel) m_cameraLabel->updateImage(composedImg);
    
    // 버튼 상태 업데이트 (View에서 계산하여 업데이트)
    bool hasDefect = false, hasSide = false, hasStaple = false;
    for (const auto& d : dets) {
        QString className = m_classNames[d.classId].toLower();
        if (className.contains("defect")) hasDefect = true;
        if (className.contains("side")) hasSide = true;
        if (className.contains("staple")) hasStaple = true;
    }
    onButtonStateUpdated(hasDefect, hasSide, hasStaple);
}

// ⭐ [수정] 원본 디자인 복원: stateCode에 따른 HEX 컬러 및 테두리 스타일 적용
void MainWindow::onAlarmStateChanged(QString stateCode, QString message, QColor color) {
    QString style;
    
    if (stateCode == "NORMAL") {
        style = "QFrame { background-color: #2b2b2b; border: 3px solid #444; border-radius: 10px; outline: none; }";
        m_alertLabel->setStyleSheet("color: #888; font-weight: bold; font-size: 24px; font-family: 'Malgun Gothic';");
    } 
    else if (stateCode == "FABRIC") {
        // 원본: 빨강 배경(#dc3545), 노랑 테두리(#ffeb3b)
        style = "QFrame { background-color: #dc3545; border: 3px solid #ffeb3b; border-radius: 10px; outline: none; }";
        m_alertLabel->setStyleSheet("color: white; font-weight: bold; font-size: 24px; font-family: 'Malgun Gothic';");
    } 
    else if (stateCode == "FILM") {
        // 원본: 파랑 배경(#007bff), 흰색 테두리
        style = "QFrame { background-color: #007bff; border: 3px solid white; border-radius: 10px; outline: none; }";
        m_alertLabel->setStyleSheet("color: white; font-weight: bold; font-size: 24px; font-family: 'Malgun Gothic';");
    } 
    else if (stateCode == "MULTI") {
        // 원본: 초록 배경(#28a745), 흰색 테두리
        style = "QFrame { background-color: #28a745; border: 3px solid white; border-radius: 10px; outline: none; }";
        m_alertLabel->setStyleSheet("color: white; font-weight: bold; font-size: 24px; font-family: 'Malgun Gothic';");
    }
    
    m_alertFrame->setStyleSheet(style);
    m_alertLabel->setText(message);
}

void MainWindow::onBlinkStateUpdated(bool isVisible, QColor color) {
    // Controller에서 계산된 깜빡임 상태를 View의 멤버 변수에 저장 (drawOverlay에서 사용)
    m_blinkToggle = isVisible; 
    
    // 깜빡임 색상 업데이트 (FILM/MULTI 등 상태에 따라 Controller가 보내준 색상 사용)
    m_currentAlarmColor = color; 
}

void MainWindow::onStatsUpdated(int fps, int yoloFps, int total, int fabric, int film) {
    if (m_lblTotalValue) m_lblTotalValue->setText(QString::number(total));
    if (m_lblFabricValue) m_lblFabricValue->setText(QString::number(fabric));
    if (m_lblFilmValue) m_lblFilmValue->setText(QString::number(film));
    
    qDebug() << "[STATS] CamFPS:" << fps << "YoloFPS:" << yoloFps << "Total:" << total;
}

void MainWindow::onNewLogItem(DefectItem item) {
    addLogItemToWidget(item);
    m_history.insert(m_history.begin(), item);
    if ((int)m_history.size() > MAX_LOG_SIZE) m_history.pop_back();
    m_currentIndex = 0;
    updateLogViewer();
}

void MainWindow::onSaverStatusChanged(bool allEmpty) {
    if (!m_controller->isSystemRunning() && allEmpty) {
        m_controller->finalizeLogFile();
        m_btnExit->setEnabled(true);
        m_btnExit->setStyleSheet(m_styleExitActive);
        m_cameraLabel->setText("모든 파일 저장 완료.\n안전하게 종료 가능.");
    }
}

// ⭐ [수정] 원본 디자인 복원: 버튼별 고유 HEX 컬러 적용
void MainWindow::onButtonStateUpdated(bool hasDefect, bool hasSide, bool hasStaple) {
    auto setStyle = [](QPushButton* btn, bool active, QString activeColor) {
        if (active) {
            btn->setStyleSheet(QString(
                "QPushButton { background-color: %1; color: white; border: 3px solid white; border-radius: 8px; font-weight: bold; font-size: 20px; font-family: 'Malgun Gothic'; }"
            ).arg(activeColor));
        } else {
            btn->setStyleSheet(
                "QPushButton { background-color: #2d2d2d; color: #888; border: 2px solid #444; border-radius: 8px; font-weight: bold; font-size: 20px; font-family: 'Malgun Gothic'; }"
            );
        }
    };
    
    // 안감 불량: #FF6B6B
    setStyle(m_btnLining, hasDefect, "#FF6B6B");
    // 사이드 불량: #4ECDC4
    setStyle(m_btnSide, hasSide, "#4ECDC4");
    // 스테이플러: #FFA500
    setStyle(m_btnStapler, hasStaple, "#FFA500");
}

// ==========================================================================
// 사용자 입력 처리
// ==========================================================================

void MainWindow::onBtnStartStop() {
    if (m_controller->isSystemRunning()) {
        // 정지
        m_controller->setSystemRunning(false);
        m_cameraWorker->stop();
        if (m_yolo) m_yolo->stop();
        m_calibrationTimer->stop();
        m_calibrationStep = 0;
        
        m_btnStartStop->setText("재시작");
        m_btnStartStop->setStyleSheet("QPushButton { background-color: #28a745; color: white; font-weight: bold; font-size: 20px; border: none; border-radius: 8px; font-family: 'Malgun Gothic'; }");
        
        if (!m_controller->areSaversEmpty()) {
            m_btnExit->setEnabled(false);
            m_btnExit->setStyleSheet(m_styleExitInactive);
            m_cameraLabel->setText("이미지 저장 중...");
        } else {
            m_controller->finalizeLogFile();
            m_btnExit->setEnabled(true);
            m_btnExit->setStyleSheet(m_styleExitActive);
        }
    } else {
        // 시작
        m_controller->startSession(m_baseSavePath);
        
        if (!m_cameraWorker->initialize()) {
            m_cameraLabel->setText("카메라 초기화 실패");
            return;
        }
        
        m_cameraLabel->setText("TensorRT 엔진 로딩 중...");
        
        QFuture<bool> future = QtConcurrent::run([this]() -> bool {
            return m_yolo->initialize(Config::NUM_CAMERAS, 30);
        });
        
        QFutureWatcher<bool>* watcher = new QFutureWatcher<bool>(this);
        connect(watcher, &QFutureWatcher<bool>::finished, this, [this, watcher]() {
            bool success = watcher->result();
            watcher->deleteLater();
            
            if (!success) {
                m_cameraLabel->setText("YOLO 초기화 실패");
                m_cameraWorker->stop();
                return;
            }
            
            m_yolo->start();
            m_cameraWorker->start();
            m_controller->setSystemRunning(true);
            
            m_btnStartStop->setText("정지");
            m_btnStartStop->setStyleSheet("QPushButton { background-color: #dc3545; color: white; font-weight: bold; font-size: 20px; border: none; border-radius: 8px; font-family: 'Malgun Gothic'; }");
            m_btnExit->setEnabled(false);
            m_btnExit->setStyleSheet(m_styleExitInactive);
            
            startFPSCalibration();
        });
        watcher->setFuture(future);
    }
}

void MainWindow::onRatioChanged(int value) {
    if (m_ratioLabel) m_ratioLabel->setText(QString("%1%").arg(value));
    
    float ratio = value / 100.0f;
    // Controller와 YOLO 둘 다 업데이트 필요
    m_controller->setExceptionRatio(ratio);
    if (m_yolo) m_yolo->setExceptionRatio(ratio);
}

void MainWindow::onBtnPlus() { if (m_ratioSlider) m_ratioSlider->setValue(m_ratioSlider->value() + 1); }
void MainWindow::onBtnMinus() { if (m_ratioSlider) m_ratioSlider->setValue(m_ratioSlider->value() - 1); }

void MainWindow::onBtnExit() {
    m_btnStartStop->setEnabled(false);
    m_btnExit->setEnabled(false);
    
    QThread* cleanupThread = QThread::create([this](){
        m_controller->setSystemRunning(false);
        if (m_cameraWorker) m_cameraWorker->stop();
        if (m_yolo) m_yolo->stop();
    });
    
    connect(cleanupThread, &QThread::finished, this, [](){ QApplication::quit(); });
    connect(cleanupThread, &QThread::finished, cleanupThread, &QThread::deleteLater);
    cleanupThread->start();
}

void MainWindow::startFPSCalibration() {
    m_calibrationStep = 0;
    m_calibrationTimer->start(1000);
}

void MainWindow::onCalibrationTimeout() {
    if (m_calibrationStep == 0) { 
        m_cameraLabel->setText("FPS 측정 준비...");
        m_calibrationStep = 1; 
    } else if (m_calibrationStep == 1) { 
        m_measuredFrameCount = 0; 
        m_fpsTimer.restart();
        m_calibrationStep = 2; 
    } else if (m_calibrationStep == 2) { 
        m_calibrationTimer->stop();
        int elapsed = m_fpsTimer.elapsed();
        int measuredFPS = (elapsed > 0) ? (m_measuredFrameCount * 1000 / elapsed) : 30;
        if (m_yolo) m_yolo->setFPS(measuredFPS);
        m_calibrationStep = 0;
    }
}

// ==========================================================================
// Draw Overlay (순수 뷰 로직)
// ==========================================================================
QImage MainWindow::drawOverlay(const QImage& source, const std::vector<Detection>& dets) {
    if (source.isNull()) return QImage();
    QImage canvas = source.copy();
    QPainter painter(&canvas);
    
    int w = canvas.width();
    int h = canvas.height();

    // 깜빡임 효과 적용 (Controller가 준 색상 사용)
    if (m_blinkToggle) {
         QColor blinkColor = m_currentAlarmColor;
         blinkColor.setAlpha(80); 
         painter.fillRect(0, 0, w, h, blinkColor); 
         painter.setPen(QPen(m_currentAlarmColor, 10));
         painter.drawRect(0, 0, w, h); 
    }

    // ROI 및 영역 그리기 (기존 코드 유지)
    float ratio = m_ratioSlider ? (m_ratioSlider->value() / 100.0f) : 0.2f;
    float fabricRatio = Config::FABRIC_WIDTH_MM / Config::REAL_WIDTH_MM;
    int fabricPixelW = static_cast<int>(w * fabricRatio);
    int fabricX1 = (w - fabricPixelW) / 2;
    int roiW = static_cast<int>(fabricPixelW * ratio);
    int roiX1 = fabricX1 + (fabricPixelW - roiW) / 2;
    QRect roiRect(roiX1, 0, roiW, h);
    
    painter.fillRect(roiRect, QColor(135, 206, 235, 60));
    painter.setPen(QPen(QColor(0, 150, 255, 180), 2, Qt::DashLine));
    painter.drawRect(roiRect);
    
    float saveTop = h * 0.30f;
    float saveBottom = h * 0.70f;
    float logTop = h * 0.40f;
    float logBottom = h * 0.60f;
    
    painter.fillRect(QRect(0, saveTop, w, saveBottom - saveTop), QColor(0, 255, 0, 30));
    painter.setPen(QPen(QColor(0, 255, 0), 2, Qt::DashLine));
    painter.drawLine(0, saveTop, w, saveTop);
    painter.drawLine(0, saveBottom, w, saveBottom);
    
    painter.fillRect(QRect(0, logTop, w, logBottom - logTop), QColor(255, 255, 0, 40));
    painter.setPen(QPen(QColor(255, 255, 0), 2, Qt::SolidLine));
    painter.drawLine(0, logTop, w, logTop);
    painter.drawLine(0, logBottom, w, logBottom);
    
    for (const auto& d : dets) {
        QColor color = (d.label == "Film") ? Qt::blue : Qt::red;
        painter.setPen(QPen(color, 3));
        painter.drawRect(d.bbox);
        
        QString labelText = QString("%1: %2").arg(getLabelName(d.classId)).arg(d.label);
        QRect textBgRect(d.bbox.x(), d.bbox.y() - 20, 150, 20);
        painter.fillRect(textBgRect, color);
        painter.setPen(Qt::white);
        painter.drawText(d.bbox.x() + 5, d.bbox.y() - 5, labelText);
    }
    
    painter.end();
    return canvas;
}

// Log & Navigation Helpers (기존 코드 유지)
void MainWindow::onLogClear() { m_logWidget->clear(); m_history.clear(); m_currentIndex = -1; updateLogViewer(); }
void MainWindow::onNavPrev() { if (m_currentIndex > 0) { m_currentIndex--; updateLogViewer(); } }
void MainWindow::onNavNext() { if (m_currentIndex < (int)m_history.size() - 1) { m_currentIndex++; updateLogViewer(); } }

void MainWindow::addLogItemToWidget(const DefectItem& item) {
    QListWidgetItem* listItem = new QListWidgetItem();
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(12, 10, 12, 10);
    
    QLabel* lblTime = new QLabel(item.timeString);
    lblTime->setStyleSheet("color: rgba(255, 255, 255, 0.6); font-size: 16px;");
    
    QString logText;
    if (item.isMulti) logText = "다중 불량";
    else if (!item.detections.empty()) logText = QString("%1 : %2").arg(item.detections[0].label == "Film" ? "필름" : "원단").arg(getLabelName(item.detections[0].classId));
    else logText = "알 수 없음";
    
    QLabel* lblMsg = new QLabel(logText);
    lblMsg->setStyleSheet("color: white; font-weight: bold; font-size: 18px;");
    layout->addWidget(lblTime);
    layout->addWidget(lblMsg);
    
    QString bgColor = item.isMulti ? "#66BB6A" : (item.detections.empty() ? "#888" : (item.detections[0].label == "Film" ? "#42A5F5" : "#EF5350"));
    widget->setStyleSheet(QString("background-color: %1; border-radius: 6px;").arg(bgColor));
    listItem->setSizeHint(QSize(widget->sizeHint().width(), 80));
    m_logWidget->insertItem(0, listItem);
    m_logWidget->setItemWidget(listItem, widget);
}

void MainWindow::updateLogViewer() {
    if (m_history.empty()) {
        m_latestImage->updateImage(QImage());
        m_navCountLabel->setText("0 / 0");
        return;
    }
    if (m_currentIndex < 0) m_currentIndex = 0;
    const DefectItem& item = m_history[m_currentIndex];
    m_latestImage->updateImage(QImage(item.filePath));
    m_navCountLabel->setText(QString("%1 / %2").arg(m_currentIndex + 1).arg(m_history.size()));
    
    auto updateHist = [&](int offset, AspectRatioLabel* lbl) {
        int idx = m_currentIndex + offset;
        if (idx < (int)m_history.size()) lbl->updateImage(QImage(m_history[idx].filePath));
        else lbl->updateImage(QImage());
    };
    updateHist(1, m_historyImg1);
    updateHist(2, m_historyImg2);
    updateHist(3, m_historyImg3);
}

QString MainWindow::getLabelName(int classId) {
    if (m_classNames.count(classId)) {
        QString n = m_classNames[classId].toLower();
        if (n == "defect") return "안감 불량";
        if (n == "side") return "사이드 불량";
        if (n.contains("staple")) return "스테이플러";
        return m_classNames[classId];
    }
    return QString::number(classId);
}

void MainWindow::setupUI() {
    // 화면 해상도 자동 계산 (10:16 비율)
    QDesktopWidget* desktop = QApplication::desktop();
    QRect screenGeometry = desktop->screenGeometry();
    int screenWidth = screenGeometry.width();
    int screenHeight = screenGeometry.height();
    
    int maxHeight = static_cast<int>(screenHeight * 0.95);
    int maxWidth = static_cast<int>(maxHeight * 10.0 / 16.0);
    
    if (maxWidth > screenWidth * 0.95) {
        maxWidth = static_cast<int>(screenWidth * 0.95);
        maxHeight = static_cast<int>(maxWidth * 16.0 / 10.0);
    }
    
    setFixedSize(maxWidth, maxHeight);
    setStyleSheet("background-color: #121212;");
    
    QWidget* cw = new QWidget(this);
    setCentralWidget(cw);
    
    QVBoxLayout* mainLayout = new QVBoxLayout(cw);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);
    
    // ==========================================================================
    // ZONE 1 (5%): 시계
    // ==========================================================================
    QFrame* zone1 = new QFrame();
    zone1->setStyleSheet("background-color: #1e1e1e; border-bottom: 1px solid #333;");
    QVBoxLayout* layout1 = new QVBoxLayout(zone1);
    layout1->setContentsMargins(0, 8, 0, 8);
    
    m_timeLabel = new QLabel("로딩 중...");
    m_timeLabel->setAlignment(Qt::AlignCenter);
    m_timeLabel->setStyleSheet("color: white; font-size: 20px; font-weight: bold; font-family: 'Malgun Gothic';");
    layout1->addWidget(m_timeLabel);
    
    // ==========================================================================
    // ZONE 2 (30%): 카메라(70%) + 통계/알람(30%)
    // ==========================================================================
    QWidget* zone2 = new QWidget();
    zone2->setStyleSheet("background-color: #1e1e1e;");
    QHBoxLayout* layout2 = new QHBoxLayout(zone2);
    layout2->setContentsMargins(8, 8, 8, 8);
    layout2->setSpacing(8);
    
    // 좌측: 카메라 + 슬라이더
    QWidget* z2Left = new QWidget();
    QVBoxLayout* z2LeftLayout = new QVBoxLayout(z2Left);
    z2LeftLayout->setContentsMargins(0, 0, 0, 0);
    z2LeftLayout->setSpacing(8);
    
    m_cameraLabel = new AspectRatioLabel("카메라 영상", this);
    m_cameraLabel->setStyleSheet("background-color: #000000; border: 1px solid #333; border-radius: 8px; color: #888; font-weight: bold; font-size: 22px;");
    
    QFrame* sliderContainer = new QFrame();
    sliderContainer->setStyleSheet("background-color: #2d2d2d; border: none; border-radius: 8px; padding: 4px;");
    QHBoxLayout* sliderLayout = new QHBoxLayout(sliderContainer);
    sliderLayout->setContentsMargins(12, 8, 12, 8);
    sliderLayout->setSpacing(8);
    
    QLabel* lblSlider = new QLabel("필름 영역");
    lblSlider->setStyleSheet("color: white; font-weight: bold; font-size: 14px; font-family: 'Malgun Gothic';");
    
    m_btnMinus = new QPushButton("−");
    m_btnMinus->setFixedSize(36, 36);
    m_btnMinus->setStyleSheet(
        "QPushButton { background-color: #616161; color: white; border: none; border-radius: 18px; font-weight: bold; font-size: 20px; }"
        "QPushButton:hover { background-color: #757575; }"
        "QPushButton:pressed { background-color: #9E9E9E; }"
    );
    connect(m_btnMinus, &QPushButton::clicked, this, &MainWindow::onBtnMinus);
    
    m_ratioSlider = new QSlider(Qt::Horizontal);
    m_ratioSlider->setRange(0, 100);
    m_ratioSlider->setValue(20);  // ⭐ 원본대로 20%
    m_ratioSlider->setStyleSheet(
        "QSlider::groove:horizontal { border: none; height: 8px; background: #424242; border-radius: 4px; }"
        "QSlider::handle:horizontal { background: #42A5F5; border: 2px solid #1e1e1e; width: 24px; height: 24px; margin: -10px 0; border-radius: 12px; }"
        "QSlider::handle:horizontal:hover { background: #2196F3; }"
    );
    connect(m_ratioSlider, &QSlider::valueChanged, this, &MainWindow::onRatioChanged);
    
    m_btnPlus = new QPushButton("+");
    m_btnPlus->setFixedSize(36, 36);
    m_btnPlus->setStyleSheet(m_btnMinus->styleSheet());
    connect(m_btnPlus, &QPushButton::clicked, this, &MainWindow::onBtnPlus);
    
    m_ratioLabel = new QLabel("20%");  // ⭐ 원본대로
    m_ratioLabel->setFixedWidth(50);
    m_ratioLabel->setAlignment(Qt::AlignCenter);
    m_ratioLabel->setStyleSheet("color: white; font-weight: bold; font-size: 16px; font-family: 'Malgun Gothic';");
    
    sliderLayout->addWidget(lblSlider);
    sliderLayout->addWidget(m_btnMinus);
    sliderLayout->addWidget(m_ratioSlider);
    sliderLayout->addWidget(m_btnPlus);
    sliderLayout->addWidget(m_ratioLabel);
    
    z2LeftLayout->addWidget(m_cameraLabel, 90);
    z2LeftLayout->addWidget(sliderContainer, 10);
    
    // 우측: 통계 + 알람
    QWidget* z2Right = new QWidget();
    QVBoxLayout* z2RightLayout = new QVBoxLayout(z2Right);
    z2RightLayout->setContentsMargins(0, 0, 0, 0);
    z2RightLayout->setSpacing(8);
    
    QFrame* statsCard = new QFrame();
    statsCard->setStyleSheet("background-color: #2d2d2d; border: none; border-radius: 8px;");
    QVBoxLayout* statsLayout = new QVBoxLayout(statsCard);
    statsLayout->setContentsMargins(12, 12, 12, 12);
    statsLayout->setSpacing(8);
    
    m_lblTotalTitle = new QLabel("전체 불량");
    m_lblTotalTitle->setAlignment(Qt::AlignCenter);
    m_lblTotalTitle->setStyleSheet("color: #999; font-size: 15px; font-weight: bold; font-family: 'Malgun Gothic';");
    
    m_lblTotalValue = new QLabel("0");
    m_lblTotalValue->setAlignment(Qt::AlignCenter);
    m_lblTotalValue->setStyleSheet("color: #EF5350; font-size: 28px; font-weight: bold; font-family: 'Malgun Gothic';");
    
    m_lblFabricTitle = new QLabel("원단 불량");
    m_lblFabricTitle->setAlignment(Qt::AlignCenter);
    m_lblFabricTitle->setStyleSheet("color: #999; font-size: 15px; font-weight: bold; font-family: 'Malgun Gothic';");
    
    m_lblFabricValue = new QLabel("0");
    m_lblFabricValue->setAlignment(Qt::AlignCenter);
    m_lblFabricValue->setStyleSheet("color: white; font-size: 26px; font-weight: bold; font-family: 'Malgun Gothic';");
    
    m_lblFilmTitle = new QLabel("필름 불량");
    m_lblFilmTitle->setAlignment(Qt::AlignCenter);
    m_lblFilmTitle->setStyleSheet("color: #999; font-size: 15px; font-weight: bold; font-family: 'Malgun Gothic';");
    
    m_lblFilmValue = new QLabel("0");
    m_lblFilmValue->setAlignment(Qt::AlignCenter);
    m_lblFilmValue->setStyleSheet("color: white; font-size: 26px; font-weight: bold; font-family: 'Malgun Gothic';");
    
    statsLayout->addStretch();
    statsLayout->addWidget(m_lblTotalTitle);
    statsLayout->addWidget(m_lblTotalValue);
    statsLayout->addStretch();
    statsLayout->addWidget(m_lblFabricTitle);
    statsLayout->addWidget(m_lblFabricValue);
    statsLayout->addStretch();
    statsLayout->addWidget(m_lblFilmTitle);
    statsLayout->addWidget(m_lblFilmValue);
    statsLayout->addStretch();
    
    m_alertFrame = new QFrame();
    m_alertFrame->setStyleSheet("background-color: #2d2d2d; border: 2px solid #444; border-radius: 8px;");
    QVBoxLayout* vboxAlarm = new QVBoxLayout(m_alertFrame);
    vboxAlarm->setContentsMargins(0, 0, 0, 0);
    m_alertLabel = new QLabel("정상 상태");
    m_alertLabel->setAlignment(Qt::AlignCenter);
    m_alertLabel->setStyleSheet("color: #888; font-weight: bold; font-size: 24px; font-family: 'Malgun Gothic';");
    vboxAlarm->addWidget(m_alertLabel);
    
    z2RightLayout->addWidget(statsCard, 1);
    z2RightLayout->addWidget(m_alertFrame, 1);
    
    // ⭐ 추가: 알람과 로그 사이 공간에 3개 아이콘 (세로 정렬)
    QFrame* extraIconsFrame = new QFrame();
    extraIconsFrame->setStyleSheet("background-color: transparent; border: none;");
    QVBoxLayout* extraIconsLayout = new QVBoxLayout(extraIconsFrame);
    extraIconsLayout->setContentsMargins(0, 8, 0, 8);
    extraIconsLayout->setSpacing(8);
    
    // 안감 불량 버튼
    m_btnLining = new QPushButton("안감 불량");
    m_btnLining->setFixedHeight(60);
    m_btnLining->setStyleSheet(
        "QPushButton { "
        "background-color: #2d2d2d; "
        "color: #888; "
        "border: 2px solid #444; "
        "border-radius: 8px; "
        "font-weight: bold; "
        "font-size: 20px; "
        "font-family: 'Malgun Gothic'; "
        "}"
    );
    
    // 사이드 버튼
    m_btnSide = new QPushButton("사이드 불량");
    m_btnSide->setFixedHeight(60);
    m_btnSide->setStyleSheet(m_btnLining->styleSheet());
    
    // 스테이플러 버튼
    m_btnStapler = new QPushButton("스테이플러");
    m_btnStapler->setFixedHeight(60);
    m_btnStapler->setStyleSheet(m_btnLining->styleSheet());
    
    extraIconsLayout->addWidget(m_btnLining);
    extraIconsLayout->addWidget(m_btnSide);
    extraIconsLayout->addWidget(m_btnStapler);
    
    z2RightLayout->addWidget(extraIconsFrame, 1);
    
    layout2->addWidget(z2Left, 70);
    layout2->addWidget(z2Right, 30);
    
    // ==========================================================================
    // ZONE 3 (60%): 이미지 히스토리 + 로그
    // ==========================================================================
    QWidget* zone3 = new QWidget();
    zone3->setStyleSheet("background-color: #1e1e1e;");
    QHBoxLayout* layout3 = new QHBoxLayout(zone3);
    layout3->setContentsMargins(8, 0, 8, 8);
    layout3->setSpacing(8);
    
    QWidget* z3Left = new QWidget();
    QVBoxLayout* z3LeftLayout = new QVBoxLayout(z3Left);
    z3LeftLayout->setContentsMargins(0, 0, 0, 0);
    z3LeftLayout->setSpacing(8);
    
    QWidget* histTop = new QWidget();
    QHBoxLayout* histLayout = new QHBoxLayout(histTop);
    histLayout->setContentsMargins(0, 0, 0, 0);
    histLayout->setSpacing(8);
    
    m_historyImg1 = new AspectRatioLabel("이전 -3", this);
    m_historyImg1->setStyleSheet("background-color: #000000; border: 1px solid #333; border-radius: 8px; color: #555; font-weight: bold; font-size: 14px;");
    
    m_historyImg2 = new AspectRatioLabel("이전 -2", this);
    m_historyImg2->setStyleSheet("background-color: #000000; border: 1px solid #333; border-radius: 8px; color: #555; font-weight: bold; font-size: 14px;");
    
    m_historyImg3 = new AspectRatioLabel("이전 -1", this);
    m_historyImg3->setStyleSheet("background-color: #000000; border: 1px solid #333; border-radius: 8px; color: #555; font-weight: bold; font-size: 14px;");
    
    histLayout->addWidget(m_historyImg1);
    histLayout->addWidget(m_historyImg2);
    histLayout->addWidget(m_historyImg3);
    
    m_latestImage = new AspectRatioLabel("최신 검출 이미지", this);
    m_latestImage->setStyleSheet("background-color: #000000; border: 1px solid #333; border-radius: 8px; color: #888; font-size: 24px; font-weight: bold;");
    
    z3LeftLayout->addWidget(histTop, 30);
    z3LeftLayout->addWidget(m_latestImage, 70);
    
    QWidget* z3Right = new QWidget();
    z3Right->setStyleSheet("background-color: #2d2d2d; border: none; border-radius: 8px;");
    QVBoxLayout* z3RightLayout = new QVBoxLayout(z3Right);
    z3RightLayout->setContentsMargins(0, 0, 0, 0);
    z3RightLayout->setSpacing(0);
    
    QFrame* logHeader = new QFrame();
    logHeader->setStyleSheet("background-color: #2d2d2d; border: none; border-top-left-radius: 8px; border-top-right-radius: 8px;");
    QHBoxLayout* headerLayout = new QHBoxLayout(logHeader);
    headerLayout->setContentsMargins(16, 12, 16, 12);
    
    QLabel* lblLogTitle = new QLabel("불량 기록");
    lblLogTitle->setStyleSheet("color: white; font-weight: bold; font-size: 18px; font-family: 'Malgun Gothic';");
    
    m_btnLogClear = new QPushButton("지우기");
    m_btnLogClear->setFixedSize(70, 32);
    m_btnLogClear->setStyleSheet(
        "QPushButton { background-color: #616161; color: white; border: none; border-radius: 8px; font-weight: bold; font-size: 14px; font-family: 'Malgun Gothic'; }"
        "QPushButton:hover { background-color: #757575; }"
        "QPushButton:pressed { background-color: #9E9E9E; }"
    );
    connect(m_btnLogClear, &QPushButton::clicked, this, &MainWindow::onLogClear);
    
    headerLayout->addWidget(lblLogTitle);
    headerLayout->addStretch();
    headerLayout->addWidget(m_btnLogClear);
    
    m_logWidget = new QListWidget();
    m_logWidget->setStyleSheet(
        "QListWidget { background-color: #1e1e1e; border: none; padding: 8px 4px 8px 8px; border-bottom-left-radius: 8px; border-bottom-right-radius: 8px; }"
        "QListWidget::item { margin: 4px 8px 4px 0px; border: none; background: transparent; }"
        "QListWidget::item:selected { background: transparent; }"
    );
    
    // ⭐ 로그 아이템 클릭 시 이미지 표시
    connect(m_logWidget, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        int row = m_logWidget->row(item);
        // row 0이 가장 최신, m_history[0]도 가장 최신
        if (row >= 0 && row < (int)m_history.size()) {
            m_currentIndex = row;
            updateLogViewer();
        }
    });
    
    z3RightLayout->addWidget(logHeader);
    z3RightLayout->addWidget(m_logWidget);
    
    layout3->addWidget(z3Left, 70);
    layout3->addWidget(z3Right, 30);
    
    // ==========================================================================
    // ZONE 4 (5%): 제어 버튼
    // ==========================================================================
    QWidget* zone4 = new QWidget();
    zone4->setStyleSheet("background-color: #1e1e1e; border-top: 1px solid #333;");
    QHBoxLayout* layout4 = new QHBoxLayout(zone4);
    layout4->setContentsMargins(8, 4, 8, 4);
    layout4->setSpacing(8);
    
    m_btnStartStop = new QPushButton("시작");
    m_btnStartStop->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_btnStartStop->setStyleSheet(
        "QPushButton { background-color: #28a745; color: white; font-weight: bold; font-size: 20px; border: none; border-radius: 8px; font-family: 'Malgun Gothic'; }"
        "QPushButton:hover { background-color: #34ce57; }"
        "QPushButton:pressed { background-color: #218838; }"
    );
    connect(m_btnStartStop, &QPushButton::clicked, this, &MainWindow::onBtnStartStop);
    
    QFrame* navFrame = new QFrame();
    navFrame->setStyleSheet("background-color: transparent; border: none;");
    QHBoxLayout* navLayout = new QHBoxLayout(navFrame);
    navLayout->setContentsMargins(0, 0, 0, 0);
    navLayout->setSpacing(12);
    
    m_btnNavPrev = new QPushButton("◀");
    m_btnNavPrev->setFixedSize(50, 44);
    m_btnNavPrev->setStyleSheet(
        "QPushButton { background-color: #424242; color: white; font-weight: bold; border: none; border-radius: 8px; font-size: 18px; }"
        "QPushButton:hover { background-color: #616161; }"
        "QPushButton:pressed { background-color: #757575; }"
        "QPushButton:disabled { color: #333; }"
    );
    connect(m_btnNavPrev, &QPushButton::clicked, this, &MainWindow::onNavPrev);
    
    m_navCountLabel = new QLabel("0 / 0");
    m_navCountLabel->setAlignment(Qt::AlignCenter);
    m_navCountLabel->setStyleSheet("color: white; font-weight: bold; font-size: 18px; font-family: 'Malgun Gothic';");
    
    m_btnNavNext = new QPushButton("▶");
    m_btnNavNext->setFixedSize(50, 44);
    m_btnNavNext->setStyleSheet(m_btnNavPrev->styleSheet());
    connect(m_btnNavNext, &QPushButton::clicked, this, &MainWindow::onNavNext);
    
    navLayout->addWidget(m_btnNavPrev);
    navLayout->addWidget(m_navCountLabel, 1);
    navLayout->addWidget(m_btnNavNext);
    
    QFrame* exitFrame = new QFrame();
    exitFrame->setStyleSheet("background-color: transparent; border: none;");
    QVBoxLayout* exitLayout = new QVBoxLayout(exitFrame);
    exitLayout->setContentsMargins(0, 0, 0, 0);
    
    m_btnExit = new QPushButton("종료");
    m_btnExit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_styleExitActive = 
        "QPushButton { background-color: #EF5350; color: white; border: none; font-weight: bold; font-size: 20px; border-radius: 8px; font-family: 'Malgun Gothic'; }"
        "QPushButton:hover { background-color: #F44336; }"
        "QPushButton:pressed { background-color: #E53935; }";
    m_styleExitInactive = 
        "QPushButton { background-color: #424242; color: #757575; border: none; font-weight: bold; font-size: 16px; border-radius: 8px; font-family: 'Malgun Gothic'; }";
    m_btnExit->setStyleSheet(m_styleExitActive);
    connect(m_btnExit, &QPushButton::clicked, this, &MainWindow::onBtnExit);
    exitLayout->addWidget(m_btnExit);
    
    layout4->addWidget(m_btnStartStop, 20);
    layout4->addWidget(navFrame, 60);
    layout4->addWidget(exitFrame, 20);
    
    // 메인 어셈블리
    mainLayout->addWidget(zone1, 5);
    mainLayout->addWidget(zone2, 35);  // ⭐ 30% -> 35% (버튼 공간 확보)
    mainLayout->addWidget(zone3, 55);  // ⭐ 60% -> 55% (로그 영역 축소)
    mainLayout->addWidget(zone4, 5);
    
    setAlarmState("NORMAL");
}

// ==========================================================================
// 슬롯 함수들
// ==========================================================================
