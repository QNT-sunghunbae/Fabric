#include "DefectController.h"
#include "config.h"
#include <QDebug>
#include <QDateTime>
#include <QApplication>
#include <QDir>
#include <QThread>

DefectController::DefectController(QObject* parent) : QObject(parent) {
    m_blinkTimer = new QTimer(this);
    connect(m_blinkTimer, &QTimer::timeout, this, &DefectController::updateAlarmTimerEffect);
    m_blinkTimer->start(50); // 50ms 간격
    m_yoloFpsTimer.start();
}

DefectController::~DefectController() {
    stopSession();
    for(auto* saver : m_savers) {
        saver->stop();
        delete saver;
    }
    m_savers.clear();
}

void DefectController::initializeSavers(const std::map<int, QString>& classNames) {
    for(int i = 0; i < NUM_SAVER_THREADS; ++i) {
        ImageSaverThread* saver = new ImageSaverThread(i, this);
        saver->setClassMap(classNames);
        connect(saver, &ImageSaverThread::imageSaved, this, &DefectController::onSaverImageSaved);
        connect(saver, &ImageSaverThread::queueEmpty, this, &DefectController::onSaverQueueEmpty);
        saver->start();
        m_savers.push_back(saver);
    }
}

void DefectController::startSession(const QString& baseSavePath) {
    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    m_currentSessionPath = QString("%1/%2").arg(baseSavePath).arg(timestamp);
    QDir().mkpath(m_currentSessionPath + "/images");
    
    QFile multiFile(QString("%1/log_multi.csv").arg(m_currentSessionPath));
    if (multiFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&multiFile);
        out << "Time,FileName";
        for(int i=1; i<=20; i++) out << QString(",Class_%1,Position_%1,StartX_%1,EndX_%1").arg(i);
        out << "\n";
        multiFile.close();
    }
    
    m_maxDefectsInSession = 0;
    m_isLogFinalized = false;
    m_stats.reset();
    m_currentOnScreenState = STATE_NONE;
    m_isAlarmDoneForCurrentState = false;
    m_saveStartTime = 0;
    m_isSaveLimitReached = false;
    m_alarmTimer = 0;
    
    for (auto* saver : m_savers) {
        saver->setSessionInfo(m_currentSessionPath, &m_logMutex, &m_maxDefectsInSession);
    }
    
    m_systemRunning = true;
    emit alarmStateChanged("NORMAL", "정상 상태", QColor("#2b2b2b"));
}

void DefectController::stopSession() {
    m_systemRunning = false;
    m_alarmTimer = 0;
    emit blinkStateUpdated(false, Qt::black);
}

void DefectController::setSystemRunning(bool running) {
    m_systemRunning = running;
    if (!running) stopSession();
}

void DefectController::setExceptionRatio(float ratio) {
    m_exceptionRatio = ratio;
}

void DefectController::updateCameraFps(int fps) {
    m_cameraFps = fps;
}

// ==========================================================================
// [CORE LOGIC] 프레임 처리 및 알람 판정
// ==========================================================================
void DefectController::processFrame(int idx, QSharedPointer<QImage> img, const std::vector<Detection>& dets) {
    if (!m_systemRunning) return;

    // 1. 프레임 카운트 및 FPS 계산
    m_frameCount++;
    m_totalFrameCount++;
    if (m_yoloFpsTimer.elapsed() >= 1000) {
        m_yoloFps = m_frameCount;
        m_frameCount = 0;
        m_yoloFpsTimer.restart();
        emit statsUpdated(m_cameraFps, m_yoloFps, m_stats.totalFabric + m_stats.totalFilm, m_stats.totalFabric, m_stats.totalFilm);
    }
    
    // UI에 그리기 요청
    emit renderFrame(img, dets);

    int h = img->height();
    float saveTop = h * 0.30f;
    float saveBottom = h * 0.70f;
    float logTop = h * 0.40f;
    float logBottom = h * 0.60f;

    bool inLogZone = false;
    for (const auto& d : dets) {
        float cy = d.bbox.center().y();
        if (cy >= logTop && cy <= logBottom) inLogZone = true;
    }

    // ============================================================
    // 2. 알람 판정 로직 (Refactored)
    // ============================================================
    int objCount = dets.size();
    DefectState frameState = STATE_NONE;
    if (objCount >= 2) frameState = STATE_MULTI;
    else if (objCount == 1) frameState = STATE_SINGLE;

    bool isStateChanged = (frameState != m_currentOnScreenState);
    bool isDefect = (frameState != STATE_NONE);

    if (isStateChanged) {
        m_currentOnScreenState = frameState;
        m_isAlarmDoneForCurrentState = false; 
        m_blinkToggle = true; 
    }

    if (isDefect) {
        bool needTrigger = isStateChanged || (!m_isAlarmDoneForCurrentState && m_alarmTimer == 0);

        if (needTrigger) {
            QApplication::beep(); // 즉시음

            if (frameState == STATE_MULTI) {
                // 원본: #28a745 (Green)
                startAlarm(QColor("#28a745"), 20); 
                emit alarmStateChanged("MULTI", "다중 불량", QColor("#28a745"));
            } 
            else if (frameState == STATE_SINGLE) {
                if (!dets.empty()) {
                    if (dets[0].label == "Film") {
                        // 원본: #007bff (Blue)
                        startAlarm(QColor("#007bff"), 10);
                        emit alarmStateChanged("FILM", "필름 불량", QColor("#007bff"));
                    } else {
                        // 원본: #dc3545 (Red)
                        startAlarm(QColor("#dc3545"), 10);
                        emit alarmStateChanged("FABRIC", "원단 불량", QColor("#dc3545"));
                    }
                }
            }
            m_isAlarmDoneForCurrentState = true; 
        }
    } 
    // 불량 해제 시 처리 (필요시 구현)

    // 3. 저장 로직 실행
    handleSaveLogic(img, dets, inLogZone, saveTop, saveBottom);
}

void DefectController::handleSaveLogic(QSharedPointer<QImage> img, const std::vector<Detection>& dets, bool inLogZone, float saveTop, float saveBottom) {
    bool inSaveZone = false;
    int currentSaveZoneCount = 0;
    
    for (const auto& d : dets) {
        float cy = d.bbox.center().y();
        if (cy >= saveTop && cy <= saveBottom) {
            inSaveZone = true;
            currentSaveZoneCount++;
        }
    }

    if (inSaveZone) {
        m_resetTimer = 0; 
        if (!m_isSaveLimitReached) {
            qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (m_saveStartTime == 0) m_saveStartTime = now;
            
            if (now - m_saveStartTime > 5000) {
                m_isSaveLimitReached = true; 
            } else {
                if (currentSaveZoneCount > m_lastSaveZoneCount) {
                    int newDefects = currentSaveZoneCount - m_lastSaveZoneCount;
                    for (int i = 0; i < newDefects && i < (int)dets.size(); ++i) {
                        if (dets[i].label == "Film") m_stats.totalFilm++;
                        else m_stats.totalFabric++;
                    }
                    m_lastSaveZoneCount = currentSaveZoneCount;
                }
                
                if (m_currentOnScreenState == STATE_MULTI) {
                    m_savers[m_currentSaverIdx]->enqueue(img, dets, m_totalFrameCount, true, inLogZone, m_exceptionRatio);
                    m_currentSaverIdx = (m_currentSaverIdx + 1) % NUM_SAVER_THREADS;
                } else if (m_currentOnScreenState == STATE_SINGLE) {
                    m_savers[m_currentSaverIdx]->enqueue(img, dets, m_totalFrameCount, false, inLogZone, m_exceptionRatio);
                    m_currentSaverIdx = (m_currentSaverIdx + 1) % NUM_SAVER_THREADS;
                }
            }
        }
    } else {
        m_resetTimer++;
        if (m_resetTimer > 100) {
            m_lastSaveZoneCount = 0;
            m_saveStartTime = 0;
            m_isSaveLimitReached = false;
            m_resetTimer = 0; 
        }
    }
}

void DefectController::startAlarm(QColor color, int durationTick) {
    m_currentAlarmColor = color;
    m_alarmTimer = durationTick;
}

void DefectController::updateAlarmTimerEffect() {
    if (m_alarmTimer > 0) {
        m_alarmTimer--;
        m_blinkToggle = !m_blinkToggle; // 토글

        int beepInterval = (m_currentOnScreenState == STATE_MULTI) ? 4 : 8;
        if (m_alarmTimer > 0 && (m_alarmTimer % beepInterval == 0)) {
             QThread::create([]() { QApplication::beep(); })->start();
        }
        
        emit blinkStateUpdated(m_blinkToggle, m_currentAlarmColor);
    } else {
        m_blinkToggle = false;
        emit blinkStateUpdated(false, Qt::black);
        
        if (m_currentOnScreenState == STATE_NONE) {
            emit alarmStateChanged("NORMAL", "정상 상태", QColor("#2b2b2b"));
        }
    }
}

void DefectController::onSaverImageSaved(DefectItem item, int defectCount) {
    if (item.showInLog) {
        emit newLogItem(item);
    }
}

void DefectController::onSaverQueueEmpty() {
    bool allEmpty = areSaversEmpty();
    emit saverStatusChanged(allEmpty);
}

bool DefectController::areSaversEmpty() {
    for(auto* s : m_savers) {
        if (!s->isQueueEmpty()) return false;
    }
    return true;
}

void DefectController::finalizeLogFile() {
    bool expected = false;
    if (!m_isLogFinalized.compare_exchange_strong(expected, true)) return;

    QString bodyPath = QString("%1/log_body.tmp").arg(m_currentSessionPath);
    QString headerPath = QString("%1/log_header.tmp").arg(m_currentSessionPath);
    QString finalPath = QString("%1/log_single.csv").arg(m_currentSessionPath);

    // [Fix] system("cat ...") 제거 및 Qt 파일 API 사용
    QFile finalFile(finalPath);
    if (finalFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        // 1. 헤더 복사
        QFile headerFile(headerPath);
        if (headerFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            finalFile.write(headerFile.readAll());
            headerFile.close();
            headerFile.remove(); // 임시 파일 삭제
        } else {
            // 헤더 파일이 없을 경우 기본 헤더 작성 (안전 장치)
            QTextStream out(&finalFile);
            out << "Time,Frame,Class,Position,StartX,EndX\n"; 
        }

        // 2. 내용(Body) 복사
        QFile bodyFile(bodyPath);
        if (bodyFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            // 파일 내용을 읽어서 최종 파일에 씀
            finalFile.write(bodyFile.readAll());
            bodyFile.close();
            bodyFile.remove(); // 임시 파일 삭제
        }
        finalFile.close();
        qDebug() << "[LOG] 로그 파일 병합 완료:" << finalPath;
    }
}