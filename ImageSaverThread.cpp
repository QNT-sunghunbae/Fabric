#include "ImageSaverThread.h"
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QPainter>
#include <QDateTime>
#include <QDebug>
#include <malloc.h>

ImageSaverThread::ImageSaverThread(int id, QObject* parent)
    : QThread(parent), m_threadId(id) {}

ImageSaverThread::~ImageSaverThread() {
    stop();
}

void ImageSaverThread::stop() {
    m_running = false;
    m_condition.wakeAll();
    wait();
}

void ImageSaverThread::setSessionInfo(const QString& path, QMutex* logMutex, std::atomic<int>* maxDefects) {
    m_sessionPath = path;
    m_logMutex = logMutex;
    m_maxDefectsPtr = maxDefects;
}

void ImageSaverThread::setClassMap(const std::map<int, QString>& classMap) {
    m_classNames = classMap;
}

bool ImageSaverThread::isQueueEmpty() {
    QMutexLocker locker(&m_mutex);
    return m_queue.isEmpty();
}

void ImageSaverThread::enqueue(QSharedPointer<QImage> img, const std::vector<Detection>& dets, long long frameCount, bool isMulti, bool showInLog, float filmRatio) {
    QMutexLocker locker(&m_mutex);
    if (m_queue.size() >= MAX_QUEUE_SIZE) {
        return;
    }
    DefectItem item;
    item.img = img; 
    item.detections = dets;
    item.frameCount = frameCount;
    item.timestamp = QDateTime::currentMSecsSinceEpoch();
    item.isMulti = isMulti;
    item.showInLog = showInLog;
    item.filmRatio = filmRatio;
    m_queue.enqueue(item);
    m_condition.wakeOne();
}

void ImageSaverThread::run() {
    while (m_running) {
        DefectItem item;
        {
            QMutexLocker locker(&m_mutex);
            while (m_queue.isEmpty() && m_running) {
                m_condition.wait(&m_mutex);
                if (m_queue.isEmpty()) {
                    emit queueEmpty();
                    malloc_trim(0);
                }
            }
            if (!m_running && m_queue.isEmpty()) break;
            if (!m_queue.isEmpty()) item = m_queue.dequeue();
        }
        if (m_queue.isEmpty()) {
            malloc_trim(0);
        }
        saveProcess(item);
    }
}

void ImageSaverThread::saveProcess(DefectItem& item) {
    if (item.detections.empty()) return;
    if (item.img.isNull()) return;

    if (item.img->format() != QImage::Format_RGB888) {
        QImage converted = item.img->convertToFormat(QImage::Format_RGB888);
        item.img = QSharedPointer<QImage>::create(converted);
    }
    
    int count = static_cast<int>(item.detections.size());
    QString subFolder = item.isMulti ? "Multi" : item.detections[0].label;
    QString imageDir = QString("%1/images/%2").arg(m_sessionPath).arg(subFolder);
    QDir dir(imageDir);
    if (!dir.exists()) dir.mkpath(".");
    
    int w = item.img->width();
    int h = item.img->height();
    float pxPerMm = (float)w / Config::REAL_WIDTH_MM;
    
    if (m_logMutex) {
        QMutexLocker locker(m_logMutex);
        QString timeStr = QDateTime::fromMSecsSinceEpoch(item.timestamp).toString("HH:mm:ss.zzz");
        item.timeString = timeStr;
        
        if (item.isMulti) {
            QFile csvFile(QString("%1/log_multi.csv").arg(m_sessionPath));
            if (csvFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
                QTextStream out(&csvFile);
                out << timeStr << "," << QString("Multi_T%1.jpg").arg(m_threadId);
                int limit = std::min(count, 20);
                for (int i = 0; i < limit; ++i) {
                    QString loc = item.detections[i].label;
                    if (loc.isEmpty()) loc = "Unknown";
                    QString clsName = QString::number(item.detections[i].classId);
                    if (m_classNames.count(item.detections[i].classId)) {
                        clsName = m_classNames[item.detections[i].classId];
                    }
                    float startX = item.detections[i].bbox.x() / pxPerMm;
                    float endX = (item.detections[i].bbox.x() + item.detections[i].bbox.width()) / pxPerMm;
                    out << "," << clsName << "," << loc << "," << QString::number(startX, 'f', 2) << "," << QString::number(endX, 'f', 2);
                }
                for (int i = limit; i < 20; ++i) out << ",,,,";
                out << "\n";
                csvFile.close();
            }
        } else {
            if (m_maxDefectsPtr) {
                int current = m_maxDefectsPtr->load();
                while (count > current && !m_maxDefectsPtr->compare_exchange_weak(current, count));
            }
            QFile csvFile(QString("%1/log_body.tmp").arg(m_sessionPath));
            if (csvFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
                QTextStream out(&csvFile);
                out << timeStr << "," << item.frameCount;
                for (const auto& d : item.detections) {
                    QString loc = d.label;
                    if (loc.isEmpty()) loc = "Unknown";
                    float startX = d.bbox.x() / pxPerMm;
                    float endX = (d.bbox.x() + d.bbox.width()) / pxPerMm;
                    QString clsName = QString::number(d.classId);
                    if (m_classNames.count(d.classId)) {
                        clsName = m_classNames[d.classId];
                    }
                    out << "," << clsName << "," << loc << "," << QString::number(startX, 'f', 2) << "," << QString::number(endX, 'f', 2);
                }
                out << "\n";
                csvFile.close();
            }
        }
    } else {
        item.timeString = QDateTime::fromMSecsSinceEpoch(item.timestamp).toString("HH:mm:ss.zzz");
    }
    
    int rulerHeight = 30;
    QImage saveImg(w, h + rulerHeight, QImage::Format_RGB888);
    saveImg.fill(Qt::white);
    QPainter p(&saveImg);
    p.setPen(QPen(Qt::black, 1));
    p.setFont(QFont("Arial", 8));
    for (int mm = 0; mm <= (int)Config::REAL_WIDTH_MM; ++mm) {
        int x = static_cast<int>(mm * pxPerMm);
        if (x >= w) break;
        if (mm % 5 == 0) {
            p.drawLine(x, 0, x, 20);
            if (mm % 10 == 0) p.drawText(x + 2, 18, QString::number(mm));
        } else {
            p.drawLine(x, 0, x, 8);
        }
    }
    
    p.drawImage(0, rulerHeight, *item.img);
    p.translate(0, rulerHeight);
    
    for (const auto& d : item.detections) {
        p.setPen(QPen(d.label == "Film" ? Qt::blue : Qt::red, 3));
        p.drawRect(d.bbox);
    }
    p.end();
    
    item.img = QSharedPointer<QImage>::create(saveImg);
    
    QString prefix = item.isMulti ? "Multi" : subFolder;
    QString filename = QString("%1/%2_%3_T%4.jpg").arg(imageDir).arg(prefix)
        .arg(QDateTime::fromMSecsSinceEpoch(item.timestamp).toString("yyyyMMdd_HHmmss_zzz"))
        .arg(m_threadId);
    
    item.img->save(filename, "JPG");
    item.filePath = filename;
    item.img.clear();
    
    emit imageSaved(item, count);
}