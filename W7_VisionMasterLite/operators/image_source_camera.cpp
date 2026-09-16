// ============================================================
// image_source_camera.cpp
// 依赖: 海康 MVS SDK (MvCameraControl.h / MvCameraControl.lib)
// 编译条件: MVS_DIR 环境变量指向 SDK 根目录(默认 D:\MVS)
// 没装 SDK 时此 cpp 编译成空算子(no-op),保持 W7 仍能编译
// ============================================================

#include "image_source_camera.h"

#include <QFileInfo>
#include <QDir>
#include <QJsonObject>
#include <QDateTime>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QFormLayout>
#include <QStandardPaths>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#ifdef HAS_MVS_SDK
#include "MvCameraControl.h"
#endif

namespace w7 {

QJsonObject ImageSourceCamera::defaultParams() const {
    QJsonObject p;
    p["camera_ip"]   = "192.168.1.10";
    p["exposure"]    = 10.0;       // ms
    p["trigger_mode"] = 0;          // 0=连续, 1=软触发
    return p;
}

QJsonObject ImageSourceCamera::run(const QJsonObject& params, const QJsonObject& /*inputs*/) {
    QJsonObject output;
    QString cameraIp   = params.value("camera_ip").toString();
    double  exposureMs = params.value("exposure").toDouble(10.0);
    int     triggerMode = params.value("trigger_mode").toInt(0);

#ifndef HAS_MVS_SDK
    // === 兜底:SDK 未装,返回友好错误 ===
    output["error"] = "MVS SDK 未安装,无法连接工业相机。"
                      "请从海康机器人官网下载 MVS_STD_V2.1.2,"
                      "解压到 D:\\MVS 并设置环境变量 MVS_DIR=D:\\MVS。";
    return output;
#else
    // === 真实 MVS SDK 调用 ===
    static void* s_handle = nullptr;       // 全局句柄,只打开一次
    static bool s_initialized = false;
    if (!s_initialized) {
        MV_CC_DEVICE_INFO_LIST stDeviceList;
        memset(&stDeviceList, 0, sizeof(stDeviceList));
        int ret = MV_CC_EnumDevices(MV_GIGE_DEVICE, &stDeviceList);
        if (ret != MV_OK || stDeviceList.nDeviceNum == 0) {
            output["error"] = QString("未发现 GigE 相机 (IP=%1, ret=%2)")
                                  .arg(cameraIp).arg(ret);
            return output;
        }
        // 如果指定了 IP,选匹配的;否则选第一个
        int selected = 0;
        for (unsigned i = 0; i < stDeviceList.nDeviceNum; ++i) {
            MV_CC_DEVICE_INFO* info = stDeviceList.pDeviceInfo[i];
            if (info->nTLayerType == MV_GIGE_DEVICE) {
                QString ip = QString::number(
                    (info->SpecialInfo.stGigEInfo.nCurrentIp & 0xFF000000) >> 24) + "." +
                    QString::number((info->SpecialInfo.stGigEInfo.nCurrentIp & 0x00FF0000) >> 16) + "." +
                    QString::number((info->SpecialInfo.stGigEInfo.nCurrentIp & 0x0000FF00) >> 8) + "." +
                    QString::number(info->SpecialInfo.stGigEInfo.nCurrentIp & 0x000000FF);
                if (ip == cameraIp) { selected = (int)i; break; }
            }
        }
        MV_CC_CreateHandle(&s_handle, stDeviceList.pDeviceInfo[selected]);
        if (s_handle == nullptr) {
            output["error"] = "MV_CC_CreateHandle 失败";
            return output;
        }
        MV_CC_OpenDevice(s_handle);
        MV_CC_SetEnumValue(s_handle, "TriggerMode",
                           triggerMode == 1 ? MV_TRIGGER_MODE_SOFT : MV_TRIGGER_MODE_OFF);
        MV_CC_StartGrabbing(s_handle);
        s_initialized = true;
    }
    // 设曝光
    MV_CC_SetFloatValue(s_handle, "ExposureTime", (float)exposureMs * 1000.0f);
    // 软触发(若开启)
    if (triggerMode == 1) MV_CC_SetCommandValue(s_handle, "TriggerSoftware");

    // === 取一帧 ===
    MV_FRAME_OUT stOutFrame;
    memset(&stOutFrame, 0, sizeof(stOutFrame));
    int ret = MV_CC_GetImageBuffer(s_handle, &stOutFrame, 1000);  // 1s 超时
    if (ret != MV_OK) {
        output["error"] = QString("GetImageBuffer 失败: ret=%1").arg(ret);
        return output;
    }
    // 零拷贝转 cv::Mat (按实际像素格式构造)
    MV_FRAME_FRAME_INFO* fi = &stOutFrame.stFrameInfo;
    cv::Mat img;
    if (fi->enPixelType == PixelType_Gvsp_BGR8_Packed) {
        img = cv::Mat(fi->nHeight, fi->nWidth, CV_8UC3, stOutFrame.pBufAddr);
    } else if (fi->enPixelType == PixelType_Gvsp_Mono8) {
        img = cv::Mat(fi->nHeight, fi->nWidth, CV_8UC1, stOutFrame.pBufAddr);
    } else if (fi->enPixelType == PixelType_Gvsp_BayerRG8
            || fi->enPixelType == PixelType_Gvsp_BayerBG8
            || fi->enPixelType == PixelType_Gvsp_BayerGR8
            || fi->enPixelType == PixelType_Gvsp_BayerGB8) {
        cv::Mat bayer(fi->nHeight, fi->nWidth, CV_8UC1, stOutFrame.pBufAddr);
        cv::cvtColor(bayer, img, cv::COLOR_BayerBG2BGR);  // 默认 BG
    } else {
        MV_CC_FreeImageBuffer(s_handle, &stOutFrame);
        output["error"] = QString("不支持的像素格式: 0x%1")
                              .arg((qulonglong)fi->enPixelType, 4, 16, QChar('0'));
        return output;
    }
    // 写临时文件给下游算子
    QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (tempDir.isEmpty()) tempDir = QDir::tempPath();
    QString tempPath = QString("%1/w7_camera_frame_%2.png")
                           .arg(tempDir)
                           .arg(QDateTime::currentMSecsSinceEpoch());
    if (!cv::imwrite(tempPath.toStdString(), img)) {
        MV_CC_FreeImageBuffer(s_handle, &stOutFrame);
        output["error"] = "imwrite 临时文件失败: " + tempPath;
        return output;
    }
    MV_CC_FreeImageBuffer(s_handle, &stOutFrame);

    output["image_path"] = tempPath;
    output["width"]      = img.cols;
    output["height"]     = img.rows;
    output["channels"]   = img.channels();
    output["camera_ip"]  = cameraIp;
    return output;
#endif
}

QWidget* ImageSourceCamera::createParamWidget(QWidget* parent, const QJsonObject& /*params*/) {
    auto* form = new ImageSourceCameraForm(parent);
    form->initialize();
    return form;
}

// ----------------------------------------------------------------
// 参数面板实现
// ----------------------------------------------------------------
ImageSourceCameraForm::ImageSourceCameraForm(QWidget* parent) : ParamForm(parent) {}

void ImageSourceCameraForm::buildUI() {
    m_cameraIp = new QLineEdit;
    m_cameraIp->setPlaceholderText("如 192.168.1.10");

    m_exposure = new QDoubleSpinBox;
    m_exposure->setRange(0.01, 1000.0);
    m_exposure->setSingleStep(1.0);
    m_exposure->setSuffix(" ms");
    m_exposure->setValue(10.0);

    m_trigger = new QComboBox;
    m_trigger->addItem("连续采图", 0);
    m_trigger->addItem("软触发", 1);

    m_layout->addRow(tr("相机 IP (GigE)"), m_cameraIp);
    m_layout->addRow(tr("曝光时间"), m_exposure);
    m_layout->addRow(tr("触发模式"), m_trigger);
}

void ImageSourceCameraForm::setParams(const QJsonObject& params) {
    if (params.contains("camera_ip")) m_cameraIp->setText(params["camera_ip"].toString());
    if (params.contains("exposure"))  m_exposure->setValue(params["exposure"].toDouble());
    if (params.contains("trigger_mode")) {
        int v = params["trigger_mode"].toInt();
        for (int i = 0; i < m_trigger->count(); ++i) {
            if (m_trigger->itemData(i).toInt() == v) {
                m_trigger->setCurrentIndex(i);
                break;
            }
        }
    }
}

QJsonObject ImageSourceCameraForm::collect() const {
    QJsonObject p;
    p["camera_ip"]    = m_cameraIp->text();
    p["exposure"]     = m_exposure->value();
    p["trigger_mode"] = m_trigger->currentData().toInt();
    return p;
}

} // namespace w7