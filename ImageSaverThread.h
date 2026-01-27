#ifndef IMAGESAVERTHREAD_H
#define IMAGESAVERTHREAD_H

#include <QThread>
#include <QMutex>
#include <QQueue>
#include <QWaitCondition>
#include <QImage>
#include <QSharedPointer>
#include <atomic>
#include <map>
#include <vector>
#include "Types.h"
#include "config.h"

// ==========================================================================
// ImageSaverThread: 불량 이미지를 디스크에 저장하는 별도 스레드
// ==========================================================================
class ImageSaverThread : public QThread {
    Q_OBJECT
public:
    explicit ImageSaverThread(int id, QObject* parent = nullptr);
    ~ImageSaverThread();

    void stop();
    void setSessionInfo(const QString& path, QMutex* logMutex, std::atomic<int>* maxDefects);
    void setClassMap(const std::map<int, QString>& classMap);
    bool isQueueEmpty();
    
    // [Optimized] QSharedPointer 사용
    void enqueue(QSharedPointer<QImage> img, const std::vector<Detection>& dets, long long frameCount, 
                 bool isMulti, bool showInLog, float filmRatio);
    void clearQueue();

signals:
    void imageSaved(DefectItem item, int defectCount);
    void queueEmpty();

protected:
    void run() override;

private:
    void saveProcess(DefectItem& item);

    int m_threadId;
    std::atomic<bool> m_running{true};
    QQueue<DefectItem> m_queue;
    QMutex m_mutex;
    QWaitCondition m_condition;
    
    QString m_sessionPath;
    QMutex* m_logMutex = nullptr;
    std::atomic<int>* m_maxDefectsPtr = nullptr;
    std::map<int, QString> m_classNames;
    
    static constexpr int MAX_QUEUE_SIZE = 100;
};

#endif // IMAGESAVERTHREAD_H