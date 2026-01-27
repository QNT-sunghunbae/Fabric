#ifndef DEEPSTREAMYOLO_H
#define DEEPSTREAMYOLO_H

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <nvbufsurface.h>
#include <gstnvdsmeta.h>

#include <QObject>
#include <QImage>
#include <QSharedPointer> // [Optimized] 스마트 포인터 헤더 추가
#include <QRect>
#include <QMutex>
#include <vector>
#include <atomic>

struct Detection {
    int classId;
    float confidence;
    QRect bbox;
    int cameraIndex;
    QString label; 
};

class DeepStreamYOLO : public QObject {
    Q_OBJECT

public:
    explicit DeepStreamYOLO(QObject* parent = nullptr);
    ~DeepStreamYOLO();

    bool initialize(int numCameras, int fps);
    void start();
    void stop();
    bool isRunning() const { return m_running.load(); }
    void setFPS(int fps);
    void pushFrame(int cameraIndex, const uint8_t* rgbData, int width, int height, qint64 timestamp);
    void setExceptionRatio(float ratio);
    float getExceptionRatio() const { return m_exceptionRatio.load(); }

signals:
    // [Performance Optimized]
    // 이미지 전달 시 복사 오버헤드를 줄이기 위해 QSharedPointer 사용
    void frameProcessed(int cameraIndex, QSharedPointer<QImage> image, const std::vector<Detection>& detections);
    
    void errorOccurred(const QString& error);

private:
    static GstFlowReturn onNewSample(GstAppSink* sink, gpointer userData);
    static GstPadProbeReturn osdSinkPadBufferProbe(GstPad* pad, GstPadProbeInfo* info, gpointer userData);
    bool createPipeline();
    void destroyPipeline();
    QString classifyDetection(const QRect& bbox, int frameWidth, int frameHeight);
    
    GstElement* m_pipeline = nullptr;
    std::vector<GstElement*> m_appsrcs;
    GstElement* m_streammux = nullptr;
    GstElement* m_pgie = nullptr;
    GstElement* m_nvvidconv = nullptr;
    GstElement* m_nvosd = nullptr;
    GstElement* m_appsink = nullptr;

    std::atomic<bool> m_running{false};
    std::atomic<float> m_exceptionRatio{0.2f};
    int m_numCameras = 0;
    std::atomic<int> m_fps{30};
    qint64 m_lastDisplayTime = 0;

    struct ROICache {
        int frameWidth = 0;
        int frameHeight = 0;
        QRect roiRect;
        bool valid = false;
    } m_roiCache;
    
    QMutex m_roiMutex;  // ROI 캐시 보호용
};

#endif // DEEPSTREAMYOLO_H