#ifndef TYPES_H
#define TYPES_H

#include <QImage>
#include <QSharedPointer> // [Optimized] 스마트 포인터 사용을 위한 헤더 추가
#include <QString>
#include <vector>
#include <QMetaType>
#include "DeepStreamYOLO.h" 

struct DefectItem {
    // [Performance Optimized] 
    // QImage를 값(Value)으로 저장하면 전달 시마다 깊은 복사(Deep Copy)가 발생합니다.
    // QSharedPointer를 사용하여 이미지 데이터의 참조(메모리 주소)만 공유하도록 변경했습니다.
    QSharedPointer<QImage> img;                 
    
    std::vector<Detection> detections; 
    long long frameCount;
    qint64 timestamp;
    QString timeString;
    bool isMulti = false; 
    QString filePath;           
    bool showInLog = false;
    float filmRatio = 0.2f; 
};

Q_DECLARE_METATYPE(DefectItem)

#endif // TYPES_H