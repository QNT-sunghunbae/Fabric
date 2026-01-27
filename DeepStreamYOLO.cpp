#include "DeepStreamYOLO.h"
#include "config.h" 

#include <QDebug>
#include <QDateTime>
#include <QCoreApplication> 
#include <QDir>
#include <cstring>

DeepStreamYOLO::DeepStreamYOLO(QObject* parent) : QObject(parent) { 
    gst_init(nullptr, nullptr); 
}

DeepStreamYOLO::~DeepStreamYOLO() { 
    stop(); 
    destroyPipeline(); 
}

bool DeepStreamYOLO::initialize(int numCameras, int fps) {
    m_numCameras = numCameras; 
    m_fps.store(fps); 
    m_appsrcs.resize(numCameras, nullptr);
    if (!createPipeline()) { 
        emit errorOccurred("Failed to create GStreamer pipeline"); 
        return false; 
    }
    return true;
}

void DeepStreamYOLO::setFPS(int fps) {
    if (fps <= 0) return;
    int currentFps = m_fps.load();
    if (abs(currentFps - fps) > (currentFps * 0.2)) {
        m_fps.store(fps);
        if (m_streammux) g_object_set(G_OBJECT(m_streammux), "batched-push-timeout", 1000000 / fps, nullptr);
    }
}

bool DeepStreamYOLO::createPipeline() {
    m_pipeline = gst_pipeline_new("fabric-defect-pipeline");
    if (!m_pipeline) return false;

    m_streammux = gst_element_factory_make("nvstreammux", "streammux");
    g_object_set(G_OBJECT(m_streammux), "batch-size", m_numCameras, "width", Config::CAMERA_WIDTH, "height", Config::CAMERA_HEIGHT,
        "batched-push-timeout", 1000000 / m_fps.load(), "live-source", TRUE, "enable-padding", FALSE, nullptr);
    gst_bin_add(GST_BIN(m_pipeline), m_streammux);

    for (int i = 0; i < m_numCameras; i++) {
        GstElement* appsrc = gst_element_factory_make("appsrc", QString("appsrc%1").arg(i).toUtf8().constData());
        GstElement* nvvidconv = gst_element_factory_make("nvvideoconvert", QString("conv%1").arg(i).toUtf8().constData());
        GstElement* capsfilter = gst_element_factory_make("capsfilter", QString("caps%1").arg(i).toUtf8().constData());
        g_object_set(G_OBJECT(nvvidconv), "compute-hw", 1, nullptr);

        GstCaps* caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGB", "width", G_TYPE_INT, Config::CAMERA_WIDTH,
            "height", G_TYPE_INT, Config::CAMERA_HEIGHT, "framerate", GST_TYPE_FRACTION, m_fps.load(), 1, nullptr);
        g_object_set(G_OBJECT(appsrc), "caps", caps, "format", GST_FORMAT_TIME, "is-live", TRUE, "block", TRUE, nullptr);
        gst_caps_unref(caps);

        GstCaps* nvCaps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "NV12", nullptr);
        GstCapsFeatures* features = gst_caps_features_new("memory:NVMM", nullptr);
        gst_caps_set_features(nvCaps, 0, features);
        g_object_set(G_OBJECT(capsfilter), "caps", nvCaps, nullptr);
        gst_caps_unref(nvCaps);

        gst_bin_add_many(GST_BIN(m_pipeline), appsrc, nvvidconv, capsfilter, nullptr);
        gst_element_link_many(appsrc, nvvidconv, capsfilter, nullptr);

        GstPad* srcpad = gst_element_get_static_pad(capsfilter, "src");
        GstPad* sinkpad = gst_element_request_pad_simple(m_streammux, QString("sink_%1").arg(i).toUtf8().constData());
        gst_pad_link(srcpad, sinkpad);
        gst_object_unref(srcpad); 
        gst_object_unref(sinkpad);
        m_appsrcs[i] = appsrc;
    }

    QString appDir = QCoreApplication::applicationDirPath();
    QString absoluteConfigPath = QDir::cleanPath(appDir + QDir::separator() + Config::YOLO_CONFIG_PATH);

    m_pgie = gst_element_factory_make("nvinfer", "pgie");
    g_object_set(G_OBJECT(m_pgie), "config-file-path", absoluteConfigPath.toUtf8().constData(), "batch-size", m_numCameras, nullptr);

    m_nvvidconv = gst_element_factory_make("nvvideoconvert", "nvvidconv_pre_osd");
    m_nvosd = gst_element_factory_make("nvdsosd", "nvosd");
    GstElement* nvvidconv_post = gst_element_factory_make("nvvideoconvert", "nvvidconv_post_osd");
    
    if(m_nvvidconv) g_object_set(G_OBJECT(m_nvvidconv), "compute-hw", 1, nullptr);
    if(nvvidconv_post) g_object_set(G_OBJECT(nvvidconv_post), "compute-hw", 1, nullptr);
    g_object_set(G_OBJECT(m_nvosd), "process-mode", 0, nullptr);

    GstElement* outCapsFilter = gst_element_factory_make("capsfilter", "out_caps");
    GstCaps* outCaps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGBA", nullptr);
    GstCapsFeatures* outFeatures = gst_caps_features_new("memory:NVMM", nullptr);
    gst_caps_set_features(outCaps, 0, outFeatures);
    g_object_set(G_OBJECT(outCapsFilter), "caps", outCaps, nullptr);
    gst_caps_unref(outCaps);

    m_appsink = gst_element_factory_make("appsink", "appsink");
    
    // ⭐ appsink 버퍼 최적화
    g_object_set(G_OBJECT(m_appsink), 
        "emit-signals", TRUE, 
        "sync", FALSE,  
        "max-buffers", 2,  
        "drop", TRUE,  
        "async", FALSE,  
        nullptr);
    
    g_signal_connect(m_appsink, "new-sample", G_CALLBACK(onNewSample), this);

    gst_bin_add_many(GST_BIN(m_pipeline), m_pgie, m_nvvidconv, m_nvosd, nvvidconv_post, outCapsFilter, m_appsink, nullptr);
    gst_element_link_many(m_streammux, m_pgie, m_nvvidconv, m_nvosd, nvvidconv_post, outCapsFilter, m_appsink, nullptr);

    GstPad* osdSinkPad = gst_element_get_static_pad(m_nvosd, "sink");
    gst_pad_add_probe(osdSinkPad, GST_PAD_PROBE_TYPE_BUFFER, osdSinkPadBufferProbe, this, nullptr);
    gst_object_unref(osdSinkPad);
    return true;
}

void DeepStreamYOLO::destroyPipeline() {
    if (m_pipeline) { 
        gst_element_set_state(m_pipeline, GST_STATE_NULL); 
        gst_object_unref(m_pipeline); 
        m_pipeline = nullptr; 
    }
    m_appsrcs.clear();
}

void DeepStreamYOLO::start() { 
    if (m_pipeline) gst_element_set_state(m_pipeline, GST_STATE_PLAYING); 
    m_running.store(true); 
}

void DeepStreamYOLO::stop() { 
    m_running.store(false); 
    if (m_pipeline) gst_element_set_state(m_pipeline, GST_STATE_NULL); 
}

void DeepStreamYOLO::pushFrame(int cameraIndex, const uint8_t* rgbData, int width, int height, qint64 timestamp) {
    if (!m_running.load() || cameraIndex >= m_numCameras) return;
    GstElement* appsrc = m_appsrcs[cameraIndex];
    gsize size = width * height * 3;
    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, size, nullptr);
    if (buffer) {
        GstMapInfo map; 
        gst_buffer_map(buffer, &map, GST_MAP_WRITE);
        memcpy(map.data, rgbData, size); 
        gst_buffer_unmap(buffer, &map);
        GST_BUFFER_PTS(buffer) = timestamp * GST_MSECOND;
        GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale_int(1, GST_SECOND, m_fps.load());
        gst_app_src_push_buffer(GST_APP_SRC(appsrc), buffer);
    }
}

void DeepStreamYOLO::setExceptionRatio(float ratio) { 
    m_exceptionRatio.store(ratio);
    {
        QMutexLocker locker(&m_roiMutex);
        m_roiCache.valid = false;
        qDebug() << "[ROI] 비율 변경됨:" << ratio << "캐시 무효화";
    }
}

QString DeepStreamYOLO::classifyDetection(const QRect& bbox, int frameWidth, int frameHeight) {
    QRect roiRect;
    
    {
        QMutexLocker locker(&m_roiMutex);
        if (m_roiCache.valid && 
            m_roiCache.frameWidth == frameWidth && 
            m_roiCache.frameHeight == frameHeight) {
            roiRect = m_roiCache.roiRect;
        } else {
            float ratio = m_exceptionRatio.load();
            float fabricRatio = Config::FABRIC_WIDTH_MM / Config::REAL_WIDTH_MM;
            int fabricPixelW = static_cast<int>(frameWidth * fabricRatio);
            int fabricX1 = (frameWidth - fabricPixelW) / 2;
            int roiW = static_cast<int>(fabricPixelW * ratio);
            int roiX1 = fabricX1 + (fabricPixelW - roiW) / 2;
            
            roiRect = QRect(roiX1, 0, roiW, frameHeight);
            
            m_roiCache.frameWidth = frameWidth;
            m_roiCache.frameHeight = frameHeight;
            m_roiCache.roiRect = roiRect;
            m_roiCache.valid = true;
            
            qDebug() << "[ROI] frameW:" << frameWidth << "frameH:" << frameHeight 
                     << "ratio:" << ratio << "ROI:" << roiRect;
        }
    }
    
    // bbox 전체가 ROI에 포함되는지 확인
    bool isContained = roiRect.contains(bbox);
    
    static int logCount = 0;
    if (++logCount % 10 == 1) {
        qDebug() << "[CLASSIFY] bbox:" << bbox << "ROI:" << roiRect 
                 << "contains:" << isContained << "→" << (isContained ? "Film" : "Fabric");
    }
    
    // bbox 전체가 ROI 안에 있으면 Film, 아니면 Fabric
    if (isContained) return "Film";
    return "Fabric";
}

GstPadProbeReturn DeepStreamYOLO::osdSinkPadBufferProbe(GstPad* pad, GstPadProbeInfo* info, gpointer userData) {
    DeepStreamYOLO* self = static_cast<DeepStreamYOLO*>(userData);
    GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
    NvDsBatchMeta* batchMeta = gst_buffer_get_nvds_batch_meta(buf);
    if (batchMeta) {
        for (NvDsMetaList* l_frame = batchMeta->frame_meta_list; l_frame != nullptr; l_frame = l_frame->next) {
            NvDsFrameMeta* frameMeta = (NvDsFrameMeta*)(l_frame->data);
            for (NvDsMetaList* l_obj = frameMeta->obj_meta_list; l_obj != nullptr; l_obj = l_obj->next) {
                NvDsObjectMeta* objMeta = (NvDsObjectMeta*)(l_obj->data);
                // TARGET_CLASS_ID가 -1이면 모든 클래스 허용
                if (Config::TARGET_CLASS_ID >= 0 && objMeta->class_id != Config::TARGET_CLASS_ID) continue;
                QRect bbox(objMeta->rect_params.left, objMeta->rect_params.top, objMeta->rect_params.width, objMeta->rect_params.height);
                QString label = self->classifyDetection(bbox, frameMeta->source_frame_width, frameMeta->source_frame_height);
                if (label == "Film") objMeta->rect_params.border_color = {0.0, 0.4, 1.0, 1.0}; 
                else objMeta->rect_params.border_color = {1.0, 0.2, 0.2, 1.0}; 
            }
        }
    }
    return GST_PAD_PROBE_OK;
}

// [Optimized Function]
GstFlowReturn DeepStreamYOLO::onNewSample(GstAppSink* sink, gpointer userData) {
    DeepStreamYOLO* self = static_cast<DeepStreamYOLO*>(userData);
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;
    
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    NvDsBatchMeta* batchMeta = gst_buffer_get_nvds_batch_meta(buffer);
    GstMapInfo map;
    
    if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        NvBufSurface* surface = (NvBufSurface*)map.data;
        
        for (uint32_t i = 0; i < surface->numFilled; i++) {
            if (NvBufSurfaceMap(surface, i, 0, NVBUF_MAP_READ) != 0) {
                continue; 
            }
            
            NvBufSurfaceSyncForCpu(surface, i, 0);
            
            // 데이터 포인터 획득 (복사 없음)
            const uchar* dataPtr = (const uchar*)surface->surfaceList[i].mappedAddr.addr[0];
            int width = surface->surfaceList[i].width;
            int height = surface->surfaceList[i].height;
            int pitch = surface->surfaceList[i].pitch;
            
            std::vector<Detection> detections;
            detections.reserve(20); 
            
            if (batchMeta) {
                for (NvDsMetaList* l_frame = batchMeta->frame_meta_list; l_frame; l_frame = l_frame->next) {
                    NvDsFrameMeta* frameMeta = (NvDsFrameMeta*)l_frame->data;
                    if (frameMeta->source_id != i) continue;
                    
                    for (NvDsMetaList* l_obj = frameMeta->obj_meta_list; l_obj; l_obj = l_obj->next) {
                        NvDsObjectMeta* objMeta = (NvDsObjectMeta*)l_obj->data;
                        if (Config::TARGET_CLASS_ID >= 0 && objMeta->class_id != Config::TARGET_CLASS_ID) continue;
                        
                        Detection det;
                        det.classId = objMeta->class_id;
                        det.confidence = objMeta->confidence;
                        det.bbox = QRect(
                            objMeta->rect_params.left,
                            objMeta->rect_params.top,
                            objMeta->rect_params.width,
                            objMeta->rect_params.height
                        );
                        det.label = self->classifyDetection(det.bbox, width, height);
                        detections.push_back(det);
                    }
                }
            }
            
            static int sampleCount = 0;
            if (++sampleCount % 30 == 1) {
                qDebug() << "[DeepStream] 프레임" << i << "에서" << detections.size() << "개 감지됨";
            }
            
            // ⭐ [Performance Optimized]
            // 1. GStreamer Raw 데이터로부터 최초 1회 복사하여 QImage 생성.
            //    (GStreamer 버퍼가 해제되어도 안전하도록 Copy 필수)
            // 2. QSharedPointer로 감싸서, 이후 전달 과정(Signal/Slot)에서의 추가 복사 방지.
            QSharedPointer<QImage> sharedImage = QSharedPointer<QImage>::create(
                dataPtr, width, height, pitch, QImage::Format_RGBA8888
            );
            
            // 주의: QImage(data, w, h, ...) 생성자는 데이터를 복사하지 않고 참조만 합니다.
            // GStreamer 메모리는 곧 해제되므로, 반드시 deep copy()를 수행해야 안전합니다.
            // 위 create() 대신 아래처럼 명시적으로 copy() 후 create 합니다.
            QImage tempImg(dataPtr, width, height, pitch, QImage::Format_RGBA8888);
            QSharedPointer<QImage> safeImage = QSharedPointer<QImage>::create(tempImg.copy());

            // ⭐ std::move로 벡터 소유권 이전
            emit self->frameProcessed(i, safeImage, std::move(detections));
            
            NvBufSurfaceUnMap(surface, i, 0);
        }
        gst_buffer_unmap(buffer, &map);
    }
    
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}