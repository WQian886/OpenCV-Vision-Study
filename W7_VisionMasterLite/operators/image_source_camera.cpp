// ============================================================
// image_source_camera.cpp
// 依赖: 海康 MVS SDK (MvCameraControl.h / MvCameraControl.lib)
// 编译条件: MVS_DIR 环境变量指向 SDK 根目录(默认 D:\MVS)
// 没装 SDK 时此 cpp 编译成空算子(no-op),保持 W7 仍能编译
// ============================================================

#include "image_source_camera.h"
#include "operator_defaults.h"
#include "param_read.h"

#include <QFileInfo>
#include <QDir>
#include <QJsonObject>
#include <QDateTime>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QStandardPaths>
#include <QStringList>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <cstring>

#ifdef HAS_MVS_SDK
#include "MvCameraControl.h"
#endif

namespace w7 {

// ============================================================
// 相机会话(只在装了 SDK 的构建里存在)
// ============================================================
namespace {

#ifdef HAS_MVS_SDK

// ============================================================
// 一个进程一份会话:句柄 + 连的是哪台相机
// ============================================================
//
// ⚠ 2026-10-24 之前这里是:
//     static void* s_handle = nullptr;
//     static bool  s_initialized = false;
//   那个布尔量**不记连的是哪台相机**,于是出了两件事:
//
//   1. 改了 camera_ip 也不重连 —— 还在用老相机,输出里却报新 IP。
//      (静默失效:界面上摆的和实际跑的不是一回事。)
//   2. 句柄只开不关 —— StopGrabbing / CloseDevice / DestroyHandle
//      一次都没调过。
//
//   现在按 IP 记;换 IP 之前先把旧的释放掉。
// ============================================================
struct CameraSession {
    void*  handle = nullptr;
    QString ip;

    bool isOpenFor(const QString& want) const {
        return handle != nullptr && ip == want;
    }

    void release() {
        if (!handle) return;
        MV_CC_StopGrabbing(handle);
        MV_CC_CloseDevice(handle);
        MV_CC_DestroyHandle(handle);
        handle = nullptr;
        ip.clear();
    }
};

CameraSession g_session;

// SDK 给的 IP 是**网络字节序**(高位在前): 0x0A000001 = 10.0.0.1
//
// ⚠ 这一份是**唯一的**"SDK 的整数 IP -> 文本 IP"换算。
//   假 SDK 那边自己也有一个"文本 -> 整数"的换算(方向相反,不是同一份)。
QString ipToText(unsigned int ip) {
    return QString("%1.%2.%3.%4")
        .arg((ip & 0xFF000000u) >> 24)
        .arg((ip & 0x00FF0000u) >> 16)
        .arg((ip & 0x0000FF00u) >> 8)
        .arg((ip & 0x000000FFu));
}

// ============================================================
// 枚举 -> 选相机 -> 打开 -> 开始取流
// ============================================================
//
// 失败时把**为什么**写进 err。静默失败比崩溃难查。
// 顺带:每一步失败都把已经拿到的资源还回去 ——
// 半开的句柄比没打开还难收拾。
bool openCamera(const QString& cameraIp, QString* err) {
    MV_CC_DEVICE_INFO_LIST stDeviceList;
    memset(&stDeviceList, 0, sizeof(stDeviceList));

    const int ret = MV_CC_EnumDevices(MV_GIGE_DEVICE, &stDeviceList);
    if (ret != MV_OK) {
        *err = QString("枚举相机失败: ret=%1").arg(ret);
        return false;
    }
    if (stDeviceList.nDeviceNum == 0) {
        *err = QString("未发现 GigE 相机 (IP=%1, 枚举到 0 台)").arg(cameraIp);
        return false;
    }

    // ------------------------------------------------------------
    // 挑相机:IP 必须**对上**。
    //
    // ⚠ 之前是 `int selected = 0;` 然后"找到就改" ——
    //   **没找到也用第 0 台**。用户填 A 的 IP,相机接在 B 上,
    //   于是连的是 B,输出里却报 A —— 静默失效。
    // ------------------------------------------------------------
    QStringList found;
    int selected = -1;
    for (unsigned i = 0; i < stDeviceList.nDeviceNum; ++i) {
        MV_CC_DEVICE_INFO* info = stDeviceList.pDeviceInfo[i];
        if (!info) continue;
        if (info->nTLayerType != MV_GIGE_DEVICE) continue;
        const QString ip = ipToText(info->SpecialInfo.stGigEInfo.nCurrentIp);
        found << ip;
        if (ip == cameraIp) { selected = static_cast<int>(i); break; }
    }
    if (selected < 0) {
        *err = QString("没找到 IP=%1 的 GigE 相机(枚举到 %2 台: %3)")
                   .arg(cameraIp)
                   .arg(stDeviceList.nDeviceNum)
                   .arg(found.isEmpty() ? QString("(没有 GigE 设备)") : found.join(", "));
        return false;
    }

    void* handle = nullptr;
    const int cret = MV_CC_CreateHandle(&handle, stDeviceList.pDeviceInfo[selected]);
    if (cret != MV_OK || handle == nullptr) {
        *err = QString("MV_CC_CreateHandle 失败: ret=%1").arg(cret);
        return false;
    }
    const int oret = MV_CC_OpenDevice(handle);
    if (oret != MV_OK) {
        MV_CC_DestroyHandle(handle);
        *err = QString("MV_CC_OpenDevice 失败: ret=%1").arg(oret);
        return false;
    }
    const int sret = MV_CC_StartGrabbing(handle);
    if (sret != MV_OK) {
        MV_CC_CloseDevice(handle);
        MV_CC_DestroyHandle(handle);
        *err = QString("MV_CC_StartGrabbing 失败: ret=%1").arg(sret);
        return false;
    }

    g_session.handle = handle;
    g_session.ip = cameraIp;
    return true;
}

#endif // HAS_MVS_SDK

} // namespace

// ============================================================
// 运行时探测
// ============================================================
bool ImageSourceCamera::runtimeAvailable() {
#ifdef HAS_MVS_SDK
    return true;
#else
    return false;
#endif
}

QString ImageSourceCamera::runtimeUnavailableReason() {
    if (runtimeAvailable()) return QString();
    return QStringLiteral(
        "海康 MVS SDK 未安装,无法连接工业相机。"
        "请从海康机器人官网下载 MVS_STD_V2.1.2,解压到 D:\\MVS,"
        "并设置环境变量 MVS_DIR=D:\\MVS 后重新编译。");
}

void ImageSourceCamera::releaseSession() {
#ifdef HAS_MVS_SDK
    g_session.release();
#else
    // 没装 SDK:本来就没有会话,空操作。
#endif
}

QJsonObject ImageSourceCamera::defaultParams() const {
    QJsonObject p;
    p["camera_ip"]   = QString::fromUtf8(ImageSourceCameraDefaults::kCameraIp);
    p["exposure"]    = ImageSourceCameraDefaults::kExposureMs;    // ms
    p["trigger_mode"] = ImageSourceCameraDefaults::kTriggerMode;  // 0=连续, 1=软触发
    return p;
}

QJsonObject ImageSourceCamera::run(const QJsonObject& params, const QJsonObject& /*inputs*/) {
    QJsonObject output;
    // 旋钮:类型不对就报错,不补默认值(理由见 param_read.h)。
    // 兜底值来自 defaultParams() —— 声明值只有一份。
    //
    // ⚠ 这三个旋钮在**两个分支之前**读:没装 SDK 的那一支虽然不用它们,
    //   也要先把参数读对 —— 免得"SDK 没装"和"参数配坏了"两件事混成一件。
    QString cameraIp;
    double exposureMs = 0.0;
    int triggerMode = 0;
    QString err;
    if (!readTextKnob(params, defaultParams(), "camera_ip", &cameraIp, &err) ||
        !readDoubleKnob(params, defaultParams(), "exposure", &exposureMs, &err) ||
        !readIntKnob(params, defaultParams(), "trigger_mode", &triggerMode, &err)) {
        output["error"] = err;
        return output;
    }

#ifndef HAS_MVS_SDK
    // === 兜底:SDK 未装,返回友好错误 ===
    // 用的是 runtimeUnavailableReason() —— 和面板上写的是同一句话,
    // 只有一份,不会出现"面板说 A、Run 说 B"。
    output["error"] = ImageSourceCamera::runtimeUnavailableReason();
    return output;
#else
    // === 真实 MVS SDK 调用 ===
    //
    // ⚠ 这半边在 2026-10-24 之前**一次都没被编译过** ——
    //   本机没装 MVS SDK,CMake 走空实现,那半边根本不参与编译。
    //   把它编起来之后抓到的问题见 notes_W7_RunEngine_2026-10.md 三十一节。

    // ------------------------------------------------------------
    // 1. 连接:按 IP 记。换 IP 要重连,重连前先把旧的关掉。
    // ------------------------------------------------------------
    if (!g_session.isOpenFor(cameraIp)) {
        g_session.release();
        QString err;
        if (!openCamera(cameraIp, &err)) {
            output["error"] = err;
            return output;
        }
    }

    // ------------------------------------------------------------
    // 2. 每帧参数:触发模式 + 曝光
    // ------------------------------------------------------------
    //
    // ⚠ 触发模式以前**只在建连时**设一次 —— 用户改了旋钮,相机那边
    //   一点没变(静默失效)。
    //
    // ⚠ 软触发是**两件事**:
    //       TriggerMode   = MV_TRIGGER_MODE_ON
    //       TriggerSource = MV_TRIGGER_SOURCE_SOFTWARE
    //   只设 TriggerMode 的话,相机还在等**外部信号** → 永远不出图 →
    //   GetImageBuffer 一直超时。
    //
    // ⚠⚠ 之前这里写的是 `MV_TRIGGER_MODE_SOFT` —— **真 SDK 里没有这个
    //   常量**。那一行从来没被编译过,所以没人发现。
    // ------------------------------------------------------------
    {
        const unsigned int modeValue =
            (triggerMode == 1) ? MV_TRIGGER_MODE_ON : MV_TRIGGER_MODE_OFF;
        const int mret = MV_CC_SetEnumValue(g_session.handle, "TriggerMode", modeValue);
        if (mret != MV_OK) {
            output["error"] = QString("设置 TriggerMode 失败: ret=%1").arg(mret);
            return output;
        }
        if (triggerMode == 1) {
            const int sret = MV_CC_SetEnumValue(g_session.handle, "TriggerSource",
                                                MV_TRIGGER_SOURCE_SOFTWARE);
            if (sret != MV_OK) {
                output["error"] = QString("设置 TriggerSource 失败: ret=%1").arg(sret);
                return output;
            }
        }
    }

    // 曝光:毫秒 -> 微秒
    {
        const int fret = MV_CC_SetFloatValue(g_session.handle, "ExposureTime",
                                             (float)exposureMs * 1000.0f);
        if (fret != MV_OK) {
            output["error"] = QString("设置 ExposureTime 失败: ret=%1").arg(fret);
            return output;
        }
    }

    // 软触发:给一帧发一次命令
    if (triggerMode == 1) {
        const int cret = MV_CC_SetCommandValue(g_session.handle, "TriggerSoftware");
        if (cret != MV_OK) {
            output["error"] = QString("软触发命令失败: ret=%1").arg(cret);
            return output;
        }
    }

    // === 取一帧 ===
    MV_FRAME_OUT stOutFrame;
    memset(&stOutFrame, 0, sizeof(stOutFrame));
    const int ret = MV_CC_GetImageBuffer(g_session.handle, &stOutFrame, 1000);  // 1s 超时
    if (ret != MV_OK) {
        output["error"] = QString("GetImageBuffer 失败: ret=%1").arg(ret);
        return output;
    }

    // ------------------------------------------------------------
    // 像素格式:源格式和转出来的格式,不是一回事
    // ------------------------------------------------------------
    MV_FRAME_FRAME_INFO* fi = &stOutFrame.stFrameInfo;
    int srcChannels = 0;   // SDK 缓冲区里,一个像素占几个字节
    int outChannels = 0;   // 转成 cv::Mat 之后,几个通道
    if (fi->enPixelType == PixelType_Gvsp_BGR8_Packed) {
        srcChannels = 3;
        outChannels = 3;
    } else if (fi->enPixelType == PixelType_Gvsp_Mono8) {
        srcChannels = 1;
        outChannels = 1;
    } else if (fi->enPixelType == PixelType_Gvsp_BayerRG8 ||
               fi->enPixelType == PixelType_Gvsp_BayerBG8 ||
               fi->enPixelType == PixelType_Gvsp_BayerGR8 ||
               fi->enPixelType == PixelType_Gvsp_BayerGB8) {
        srcChannels = 1;   // Bayer 是单通道
        outChannels = 3;   // 解完马赛克是三通道
    } else {
        MV_CC_FreeImageBuffer(g_session.handle, &stOutFrame);
        // ⚠ 宽度给 8 —— 像素类型是 32 位的,给 4 位的话
        //   0x02180014 会被写成 "0x2180014",少一位,看着像另一个数。
        output["error"] = QString("不支持的像素格式: 0x%1")
                              .arg((qulonglong)fi->enPixelType, 8, 16, QChar('0'));
        return output;
    }

    // 缓冲区够不够:不够就别读。
    // 越界读不会报错,只会崩 —— 而且崩的是整个程序,不是"这一条流水线失败"。
    const size_t need = static_cast<size_t>(fi->nHeight) *
                        static_cast<size_t>(fi->nWidth) *
                        static_cast<size_t>(srcChannels);
    if (static_cast<size_t>(stOutFrame.nFrameLen) < need) {
        MV_CC_FreeImageBuffer(g_session.handle, &stOutFrame);
        output["error"] = QString("相机给的缓冲区太小: 需要 %1 字节, 实际 %2 字节")
                              .arg(need)
                              .arg(stOutFrame.nFrameLen);
        return output;
    }

    // 零拷贝转 cv::Mat (按实际像素格式构造)
    cv::Mat img;
    if (fi->enPixelType == PixelType_Gvsp_BGR8_Packed) {
        img = cv::Mat((int)fi->nHeight, (int)fi->nWidth, CV_8UC3, stOutFrame.pBufAddr);
    } else if (fi->enPixelType == PixelType_Gvsp_Mono8) {
        img = cv::Mat((int)fi->nHeight, (int)fi->nWidth, CV_8UC1, stOutFrame.pBufAddr);
    } else {
        cv::Mat bayer((int)fi->nHeight, (int)fi->nWidth, CV_8UC1, stOutFrame.pBufAddr);
        // ⚠ Bayer 有四种排布(RG / GR / GB / BG),各用各的转换码。
        //   之前**一律**按 BG 转 —— 那三种排布出来的颜色是错的,
        //   而且不报错、不崩溃,只是颜色悄悄反了。
        if (fi->enPixelType == PixelType_Gvsp_BayerRG8) {
            cv::cvtColor(bayer, img, cv::COLOR_BayerRG2BGR);
        } else if (fi->enPixelType == PixelType_Gvsp_BayerGR8) {
            cv::cvtColor(bayer, img, cv::COLOR_BayerGR2BGR);
        } else if (fi->enPixelType == PixelType_Gvsp_BayerGB8) {
            cv::cvtColor(bayer, img, cv::COLOR_BayerGB2BGR);
        } else {
            cv::cvtColor(bayer, img, cv::COLOR_BayerBG2BGR);  // 默认 BG
        }
    }
    // 写临时文件给下游算子
    QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (tempDir.isEmpty()) tempDir = QDir::tempPath();
    QString tempPath = QString("%1/w7_camera_frame_%2.png")
                           .arg(tempDir)
                           .arg(QDateTime::currentMSecsSinceEpoch());
    if (!cv::imwrite(tempPath.toStdString(), img)) {
        MV_CC_FreeImageBuffer(g_session.handle, &stOutFrame);
        output["error"] = "imwrite 临时文件失败: " + tempPath;
        return output;
    }
    MV_CC_FreeImageBuffer(g_session.handle, &stOutFrame);

    output["image_path"] = tempPath;
    output["width"]      = img.cols;
    output["height"]     = img.rows;
    output["channels"]   = outChannels;
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
    // 相机 IP:用声明的默认值当初值。
    //
    // ⚠ 这里以前是 new QLineEdit(空) + setPlaceholderText("如 192.168.1.10")。
    //   两个问题:
    //   1. 框里初始是空的 → collect() 把空串写回 params →
    //      defaultParams() 声明的默认值被吃掉了。
    //   2. 提示语里那个 IP 是 ImageSourceCameraDefaults::kCameraIp 的抄本。
    m_cameraIp = new QLineEdit(QString::fromUtf8(ImageSourceCameraDefaults::kCameraIp));

    m_exposure = new QDoubleSpinBox;
    m_exposure->setRange(0.01, 1000.0);
    m_exposure->setSingleStep(1.0);
    m_exposure->setSuffix(" ms");
    m_exposure->setValue(ImageSourceCameraDefaults::kExposureMs);

    m_trigger = new QComboBox;
    m_trigger->addItem("连续采图", 0);
    m_trigger->addItem("软触发", 1);

    m_layout->addRow(tr("相机 IP (GigE)"), m_cameraIp);
    m_layout->addRow(tr("曝光时间"), m_exposure);
    m_layout->addRow(tr("触发模式"), m_trigger);

    // ============================================================
    // 运行时缺失:旋钮置灰 + 写明原因
    // ============================================================
    // 没装 MVS SDK 时,这个算子整体是空实现 —— 曝光、触发模式这些
    // 旋钮当前**一个都不生效**。摆着能调但没用的旋钮 = 骗人。
    //
    // 置灰只是"别让人以为能调",真正要紧的是把原因写出来:
    // 去哪儿下载、解压到哪、设哪个环境变量。
    // ============================================================
    if (!ImageSourceCamera::runtimeAvailable()) {
        m_cameraIp->setEnabled(false);
        m_exposure->setEnabled(false);
        m_trigger->setEnabled(false);

        m_hint = new QLabel;
        m_hint->setWordWrap(true);
        m_hint->setText(tr("⚠️ %1").arg(ImageSourceCamera::runtimeUnavailableReason()));
        m_hint->setStyleSheet("color:#c05000;");
        m_layout->addRow(m_hint);
    }
}

void ImageSourceCameraForm::setParams(const QJsonObject& params) {
    // 兜底用**声明的默认值**。
    // ⚠ camera_ip 以前是 toString() 不带参数 —— 那是**空串**,不是声明值
    //   ("192.168.1.10"):存盘里 IP 存成了数字时,面板显示空串,
    //   算子却按声明 IP 跑 —— 界面上摆的和实际跑的不是一回事。
    const QJsonObject d = ImageSourceCamera().defaultParams();
    if (params.contains("camera_ip"))
        m_cameraIp->setText(knobTextForWidget(params.value("camera_ip"), d, "camera_ip"));
    if (params.contains("exposure"))
        m_exposure->setValue(params.value("exposure").toDouble(ImageSourceCameraDefaults::kExposureMs));
    if (params.contains("trigger_mode")) {
        int v = params.value("trigger_mode").toInt(ImageSourceCameraDefaults::kTriggerMode);
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