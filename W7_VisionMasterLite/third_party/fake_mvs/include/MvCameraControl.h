// ============================================================
// 假 MVS SDK —— third_party/fake_mvs/include/MvCameraControl.h
// ============================================================
//
// ⚠ 这是**假的**海康 MVS SDK。它只在**测试目标**里用。
//   绝不能把它加进 w7_vm_lite —— 那等于把假 SDK 塞进发货的二进制。
//   (和 third_party/fake_onnxruntime/ 同一个规矩。)
//
// ------------------------------------------------------------
// 本机没装真 MVS SDK,所以这份头文件是**照公开文档抄的**,不是抄的真头文件。
// 抄的时候对着这些来源:
//   · 像素类型取值 = GigE Vision 标准的 MvGvspPixelType
//       Mono8=0x01080001  BayerGR8=0x01080008  BayerRG8=0x01080009
//       BayerGB8=0x0108000A  BayerBG8=0x0108000B
//       RGB8_Packed=0x02180014  BGR8_Packed=0x02180015
//   · MV_CC_DEVICE_INFO_LIST = { unsigned nDeviceNum; MV_CC_DEVICE_INFO* pDeviceInfo[256]; }
//   · nCurrentIp 是**网络字节序**(高位在前): 0x0A000001 = 10.0.0.1
//   · MV_CC_OpenDevice 在 C++ 下**有默认参数**(访问模式 / 切换密钥),
//     所以 MV_CC_OpenDevice(handle) 是合法的
//
// ⚠ 抄错的地方就是欠账 —— 见 notes/notes_W7_RunEngine_2026-10.md 三十一节。
//   本份头文件只能保证「算子自己的逻辑对」,保证不了「和真 SDK 对得上」。
//
// ============================================================
// ⚠⚠⚠ 故意**不**提供 MV_TRIGGER_MODE_SOFT
// ============================================================
//
// 真 SDK 里**没有** MV_TRIGGER_MODE_SOFT 这个常量 ——
// 软触发是两件事:
//     TriggerMode   = MV_TRIGGER_MODE_ON
//     TriggerSource = MV_TRIGGER_SOURCE_SOFTWARE
//
// 2026-10-24 之前,image_source_camera.cpp 里写着
//     triggerMode == 1 ? MV_TRIGGER_MODE_SOFT : MV_TRIGGER_MODE_OFF
// 而那半边代码**从来没被编译过**,所以没人发现这个常量压根不存在 ——
// 也就是说:**装上真 SDK 的机器上,这个算子编不过。**
//
// 假件要是"贴心地"补上这个常量,就把这个 bug 遮住了 ——
// 那正好违背本项目的规矩:
//     **假的东西要在该严的地方严。**
// 严的方向是「让真 SDK 会拒绝的东西,假件也拒绝」。
//
// 所以:这里只给 MV_TRIGGER_MODE_OFF / MV_TRIGGER_MODE_ON。
//     算子要是还引用 MV_TRIGGER_MODE_SOFT,编译就红 —— 那是**故意的**。
// ============================================================

#ifndef FAKE_MV_CAMERA_CONTROL_H
#define FAKE_MV_CAMERA_CONTROL_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

// ----------------------------------------------------------------
// 常量
// ----------------------------------------------------------------

#define MV_OK 0

// 设备层类型(真 SDK: MV_GIGE_DEVICE=0x00000001, MV_USB_DEVICE=0x00000002)
#define MV_GIGE_DEVICE 0x00000001u
#define MV_USB_DEVICE  0x00000002u

// 设备访问模式
#define MV_ACCESS_Exclusive 1u
#define MV_ACCESS_Control   2u
#define MV_ACCESS_Monitor   3u

// 触发模式(GenICam TriggerMode: Off=0, On=1)
//
// ⚠ 这里**没有** MV_TRIGGER_MODE_SOFT —— 见文件头那段说明。
#define MV_TRIGGER_MODE_OFF 0u
#define MV_TRIGGER_MODE_ON  1u

// 触发源(软触发 / 硬触发线)
#define MV_TRIGGER_SOURCE_SOFTWARE 0u
#define MV_TRIGGER_SOURCE_LINE0    1u

// 最多支持多少台设备
#define MV_MAX_DEVICE_NUM 256

// ----------------------------------------------------------------
// 类型
// ----------------------------------------------------------------

// GigE 设备信息(算子只用 nCurrentIp,其余是占位)
typedef struct {
    unsigned int  nCurrentIp;               // 网络字节序(高位在前)
    unsigned char chUserDefinedName[32];
    unsigned char chSerialNumber[16];
    unsigned char chModelName[32];
    unsigned int  nAlarmMode;
} MV_GIGE_DEVICE_INFO;

// 设备信息
typedef struct {
    unsigned int nTLayerType;
    struct {
        MV_GIGE_DEVICE_INFO stGigEInfo;
    } SpecialInfo;
} MV_CC_DEVICE_INFO;

// 设备列表
typedef struct {
    unsigned int       nDeviceNum;
    MV_CC_DEVICE_INFO* pDeviceInfo[MV_MAX_DEVICE_NUM];
} MV_CC_DEVICE_INFO_LIST;

// 像素类型(GigE Vision 标准值)
typedef enum : unsigned int {
    PixelType_Gvsp_Undefined   = 0xFFFFFFFFu,
    PixelType_Gvsp_Mono8       = 0x01080001u,
    PixelType_Gvsp_BayerGR8    = 0x01080008u,
    PixelType_Gvsp_BayerRG8    = 0x01080009u,
    PixelType_Gvsp_BayerGB8    = 0x0108000Au,
    PixelType_Gvsp_BayerBG8    = 0x0108000Bu,
    PixelType_Gvsp_RGB8_Packed = 0x02180014u,
    PixelType_Gvsp_BGR8_Packed = 0x02180015u
} MvGvspPixelType;

// 帧信息
typedef struct {
    unsigned int    nWidth;
    unsigned int    nHeight;
    unsigned int    nFrameNum;
    unsigned int    nFrameLen;
    MvGvspPixelType enPixelType;
    unsigned int    nReserved[8];
} MV_FRAME_FRAME_INFO;

// 取到的一帧
typedef struct {
    MV_FRAME_FRAME_INFO stFrameInfo;
    unsigned char*       pBufAddr;
    unsigned int         nFrameLen;
    unsigned int         nReserved[4];
} MV_FRAME_OUT;

// ============================================================
// fakeMvs 的内部状态
//
// ⚠ C++17 的 inline 变量 —— 头文件被两个 .cpp 同时包含也不会重复定义。
//   这条头文件会被 image_source_camera.cpp **和** 测试文件同时包含。
// ============================================================
namespace fakeMvs {

// 一台"相机"
struct Device {
    unsigned int ip        = 0;   // 网络字节序
    unsigned int layerType = MV_GIGE_DEVICE;
};

namespace detail {

inline std::vector<Device>            g_devices;
inline std::vector<MV_CC_DEVICE_INFO> g_deviceInfoStore;   // 预留 256,不会重分配
inline std::vector<unsigned char>     g_frameData;
inline std::vector<unsigned char>     g_buffer;
inline unsigned int                   g_frameW = 0;
inline unsigned int                   g_frameH = 0;
inline unsigned int                   g_bufferSize = 0;
inline MvGvspPixelType                g_pixelType = PixelType_Gvsp_BGR8_Packed;

inline int g_enumRet = MV_OK;
inline int g_createRet = MV_OK;
inline int g_openRet = MV_OK;
inline int g_startGrabRet = MV_OK;
inline int g_setEnumRet = MV_OK;
inline int g_setFloatRet = MV_OK;
inline int g_setCommandRet = MV_OK;
inline int g_getBufferRet = MV_OK;

inline int g_enumCount = 0;
inline int g_createCount = 0;
inline int g_openCount = 0;
inline int g_startGrabCount = 0;
inline int g_stopGrabCount = 0;
inline int g_closeCount = 0;
inline int g_destroyCount = 0;
inline int g_getBufferCount = 0;
inline int g_freeBufferCount = 0;
inline int g_setEnumCount = 0;
inline int g_setFloatCount = 0;
inline int g_setCommandCount = 0;
inline int g_openHandles = 0;

inline std::vector<std::pair<std::string, unsigned int> > g_enumCalls;
inline std::vector<std::pair<std::string, float> >        g_floatCalls;
inline std::vector<std::string>                           g_commandCalls;

// 假句柄 —— 真 SDK 里这是个不透明指针,这里给个唯一地址
inline char g_handleToken = 0;

} // namespace detail
} // namespace fakeMvs

// ============================================================
// SDK API(假的)
// ============================================================
inline int MV_CC_EnumDevices(unsigned int nTLayerType, MV_CC_DEVICE_INFO_LIST* pstDevList) {
    (void)nTLayerType;
    ++fakeMvs::detail::g_enumCount;
    if (fakeMvs::detail::g_enumRet != MV_OK) return fakeMvs::detail::g_enumRet;
    if (!pstDevList) return -1;
    pstDevList->nDeviceNum = 0;
    const std::size_t n = fakeMvs::detail::g_deviceInfoStore.size() > MV_MAX_DEVICE_NUM
                              ? static_cast<std::size_t>(MV_MAX_DEVICE_NUM)
                              : fakeMvs::detail::g_deviceInfoStore.size();
    for (std::size_t i = 0; i < n; ++i) {
        pstDevList->pDeviceInfo[i] = &fakeMvs::detail::g_deviceInfoStore[i];
    }
    pstDevList->nDeviceNum = static_cast<unsigned int>(n);
    return MV_OK;
}

inline int MV_CC_CreateHandle(void** handle, const MV_CC_DEVICE_INFO* pstDevInfo) {
    (void)pstDevInfo;
    ++fakeMvs::detail::g_createCount;
    if (!handle) return -1;
    if (fakeMvs::detail::g_createRet != MV_OK) {
        *handle = nullptr;
        return fakeMvs::detail::g_createRet;
    }
    ++fakeMvs::detail::g_openHandles;
    *handle = &fakeMvs::detail::g_handleToken;
    return MV_OK;
}

inline int MV_CC_OpenDevice(void* handle,
                            unsigned int nAccessMode = MV_ACCESS_Exclusive,
                            unsigned short nSwitchoverKey = 0) {
    (void)nAccessMode;
    (void)nSwitchoverKey;
    ++fakeMvs::detail::g_openCount;
    if (!handle) return -1;
    return fakeMvs::detail::g_openRet;
}

inline int MV_CC_StartGrabbing(void* handle) {
    ++fakeMvs::detail::g_startGrabCount;
    if (!handle) return -1;
    return fakeMvs::detail::g_startGrabRet;
}

inline int MV_CC_StopGrabbing(void* handle) {
    ++fakeMvs::detail::g_stopGrabCount;
    if (!handle) return -1;
    return MV_OK;
}

inline int MV_CC_CloseDevice(void* handle) {
    ++fakeMvs::detail::g_closeCount;
    if (!handle) return -1;
    return MV_OK;
}

inline int MV_CC_DestroyHandle(void* handle) {
    ++fakeMvs::detail::g_destroyCount;
    if (!handle) return -1;
    if (fakeMvs::detail::g_openHandles > 0) --fakeMvs::detail::g_openHandles;
    return MV_OK;
}

inline int MV_CC_SetEnumValue(void* handle, const char* key, unsigned int value) {
    (void)handle;
    ++fakeMvs::detail::g_setEnumCount;
    fakeMvs::detail::g_enumCalls.push_back(std::make_pair(std::string(key ? key : ""), value));
    return fakeMvs::detail::g_setEnumRet;
}

inline int MV_CC_SetCommandValue(void* handle, const char* key) {
    (void)handle;
    ++fakeMvs::detail::g_setCommandCount;
    fakeMvs::detail::g_commandCalls.push_back(std::string(key ? key : ""));
    return fakeMvs::detail::g_setCommandRet;
}

inline int MV_CC_SetFloatValue(void* handle, const char* key, float value) {
    (void)handle;
    ++fakeMvs::detail::g_setFloatCount;
    fakeMvs::detail::g_floatCalls.push_back(std::make_pair(std::string(key ? key : ""), value));
    return fakeMvs::detail::g_setFloatRet;
}

inline int MV_CC_GetImageBuffer(void* handle, MV_FRAME_OUT* pstFrame, unsigned int nMsec) {
    (void)nMsec;
    ++fakeMvs::detail::g_getBufferCount;
    if (!handle || !pstFrame) return -1;
    if (fakeMvs::detail::g_getBufferRet != MV_OK) return fakeMvs::detail::g_getBufferRet;

    const unsigned int want = fakeMvs::detail::g_bufferSize > 0
                                  ? fakeMvs::detail::g_bufferSize
                                  : static_cast<unsigned int>(fakeMvs::detail::g_frameData.size());
    // ⚠ 物理上多分配 64 字节:报告给调用方的 nFrameLen 仍然是 want(那才是契约),
    //   但真去读越界的地方不会踩到未映射的内存 —— 那样会**崩**,
    //   而崩掉的是整个测试程序,后面的用例一条都跑不到。
    //   零填充保证越界读到的内容是确定的(全 0),不会变成随机值。
    const std::size_t alloc = static_cast<std::size_t>(want) + 64u;
    if (fakeMvs::detail::g_buffer.size() < alloc) fakeMvs::detail::g_buffer.resize(alloc);
    std::memset(fakeMvs::detail::g_buffer.data(), 0, fakeMvs::detail::g_buffer.size());
    const std::size_t n = fakeMvs::detail::g_frameData.size() < want
                              ? fakeMvs::detail::g_frameData.size()
                              : want;
    if (n > 0) {
        std::memcpy(fakeMvs::detail::g_buffer.data(), fakeMvs::detail::g_frameData.data(), n);
    }

    pstFrame->pBufAddr = fakeMvs::detail::g_buffer.data();
    pstFrame->nFrameLen = want;
    pstFrame->stFrameInfo.nWidth = fakeMvs::detail::g_frameW;
    pstFrame->stFrameInfo.nHeight = fakeMvs::detail::g_frameH;
    pstFrame->stFrameInfo.nFrameLen = want;
    pstFrame->stFrameInfo.enPixelType = fakeMvs::detail::g_pixelType;
    return MV_OK;
}

inline int MV_CC_FreeImageBuffer(void* handle, MV_FRAME_OUT* pstFrame) {
    ++fakeMvs::detail::g_freeBufferCount;
    (void)handle;
    if (!pstFrame) return -1;
    // ⚠ 释放即清零 —— 和假 ONNX Runtime 一个规矩:
    //   把"用了已释放的内存"从「偶尔对偶尔错」变成**确定**的错。
    //   真 SDK 上那块内存是未定义的,不一定是 0。
    if (pstFrame->pBufAddr) {
        std::memset(pstFrame->pBufAddr, 0, fakeMvs::detail::g_buffer.size());
        pstFrame->pBufAddr = nullptr;
    }
    return MV_OK;
}

// ============================================================
// 控制面 / 观测面(测试专用 —— 算子一行都不调)
// ============================================================
namespace fakeMvs {

// 控制面:测试说"SDK 这次怎么表现",假件就怎么表现
inline void reset() {
    detail::g_devices.clear();
    detail::g_deviceInfoStore.clear();
    detail::g_deviceInfoStore.reserve(MV_MAX_DEVICE_NUM);
    detail::g_frameData.clear();
    detail::g_buffer.clear();
    detail::g_frameW = 0;
    detail::g_frameH = 0;
    detail::g_bufferSize = 0;
    detail::g_pixelType = PixelType_Gvsp_BGR8_Packed;
    detail::g_enumRet = MV_OK;
    detail::g_createRet = MV_OK;
    detail::g_openRet = MV_OK;
    detail::g_startGrabRet = MV_OK;
    detail::g_setEnumRet = MV_OK;
    detail::g_setFloatRet = MV_OK;
    detail::g_setCommandRet = MV_OK;
    detail::g_getBufferRet = MV_OK;
    detail::g_enumCount = 0;
    detail::g_createCount = 0;
    detail::g_openCount = 0;
    detail::g_startGrabCount = 0;
    detail::g_stopGrabCount = 0;
    detail::g_closeCount = 0;
    detail::g_destroyCount = 0;
    detail::g_getBufferCount = 0;
    detail::g_freeBufferCount = 0;
    detail::g_setEnumCount = 0;
    detail::g_setFloatCount = 0;
    detail::g_setCommandCount = 0;
    detail::g_openHandles = 0;
    detail::g_enumCalls.clear();
    detail::g_floatCalls.clear();
    detail::g_commandCalls.clear();
}

inline void setDevices(const std::vector<Device>& devs) {
    detail::g_devices = devs;
    detail::g_deviceInfoStore.clear();
    detail::g_deviceInfoStore.reserve(MV_MAX_DEVICE_NUM);
    for (std::size_t i = 0; i < devs.size(); ++i) {
        MV_CC_DEVICE_INFO info;
        std::memset(&info, 0, sizeof(info));
        info.nTLayerType = devs[i].layerType;
        info.SpecialInfo.stGigEInfo.nCurrentIp = devs[i].ip;
        detail::g_deviceInfoStore.push_back(info);
    }
}

inline void setEnumRet(int ret) { detail::g_enumRet = ret; }
inline void setCreateRet(int ret) { detail::g_createRet = ret; }
inline void setOpenRet(int ret) { detail::g_openRet = ret; }
inline void setStartGrabRet(int ret) { detail::g_startGrabRet = ret; }
inline void setSetEnumRet(int ret) { detail::g_setEnumRet = ret; }
inline void setSetFloatRet(int ret) { detail::g_setFloatRet = ret; }
inline void setSetCommandRet(int ret) { detail::g_setCommandRet = ret; }
inline void setGetBufferRet(int ret) { detail::g_getBufferRet = ret; }
inline void setPixelType(MvGvspPixelType t) { detail::g_pixelType = t; }
inline void setFrameData(const std::vector<unsigned char>& data) { detail::g_frameData = data; }
inline void setFrameSize(unsigned int w, unsigned int h) { detail::g_frameW = w; detail::g_frameH = h; }
inline void setBufferSize(unsigned int n) { detail::g_bufferSize = n; }

inline int enumCount() { return detail::g_enumCount; }
inline int createCount() { return detail::g_createCount; }
inline int openCount() { return detail::g_openCount; }
inline int startGrabCount() { return detail::g_startGrabCount; }
inline int stopGrabCount() { return detail::g_stopGrabCount; }
inline int closeCount() { return detail::g_closeCount; }
inline int destroyCount() { return detail::g_destroyCount; }
inline int getBufferCount() { return detail::g_getBufferCount; }
inline int freeBufferCount() { return detail::g_freeBufferCount; }
inline int setEnumCount() { return detail::g_setEnumCount; }
inline int setFloatCount() { return detail::g_setFloatCount; }
inline int setCommandCount() { return detail::g_setCommandCount; }
inline int openHandles() { return detail::g_openHandles; }

inline int enumCallCountFor(const std::string& key) {
    int n = 0;
    for (std::size_t i = 0; i < detail::g_enumCalls.size(); ++i)
        if (detail::g_enumCalls[i].first == key) ++n;
    return n;
}

inline unsigned int lastEnumValueFor(const std::string& key) {
    for (std::size_t i = detail::g_enumCalls.size(); i > 0; --i)
        if (detail::g_enumCalls[i - 1].first == key) return detail::g_enumCalls[i - 1].second;
    return 0xFFFFFFFFu;
}

inline std::string lastFloatKey() {
    return detail::g_floatCalls.empty() ? std::string() : detail::g_floatCalls.back().first;
}

inline float lastFloatValue() {
    return detail::g_floatCalls.empty() ? 0.0f : detail::g_floatCalls.back().second;
}

inline std::string lastCommandKey() {
    return detail::g_commandCalls.empty() ? std::string() : detail::g_commandCalls.back();
}

inline unsigned int ipToUint(const std::string& dotted) {
    unsigned int out = 0;
    unsigned int part = 0;
    for (std::size_t i = 0; i < dotted.size(); ++i) {
        const char c = dotted[i];
        if (c >= '0' && c <= '9') {
            part = part * 10 + static_cast<unsigned int>(c - '0');
        } else if (c == '.') {
            out = (out << 8) | (part & 0xFFu);
            part = 0;
        }
    }
    out = (out << 8) | (part & 0xFFu);
    return out;
}

} // namespace fakeMvs

#endif // FAKE_MV_CAMERA_CONTROL_H
