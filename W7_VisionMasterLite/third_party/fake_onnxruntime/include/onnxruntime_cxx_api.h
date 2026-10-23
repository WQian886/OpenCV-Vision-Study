#pragma once
// ============================================================
// 假 ONNX Runtime —— 只为测 dl_infer 的推理分支
// ============================================================
//
// === 为什么要有这个文件 ===
//
// dl_infer 的推理路径(#ifdef HAS_ONNXRUNTIME 那半边,约 150 行)
// **从来没有被编译过一次**,更没有被跑过:
//   本机没装 ONNX Runtime -> CMake 走空实现 -> 那半边根本不参与编译。
//
// 这不只是"少测了一块"。那半边里有:
//   · letterbox + BGR2RGB + /255 + NCHW 的**顺序**(顺序错了不报错,
//     数值一样但通道序反了,结果全错)
//   · 输出张量到 cv::Mat 的**形状包装**(yolov5 / yolov8 两种布局
//     长得完全不一样,包错了不报错,只是全检测不出来)
//   · NMS 之后的检测框 JSON
// 这些恰恰是**静默出错**的重灾区 ——
// "不崩、不报错、数字看着正常、结论是错的"。
//
// === 为什么是"假 SDK"而不是"改代码绕过 SDK" ===
//
// 改代码绕过 = 推理路径永远只能靠真模型 + 真 SDK 验证,拿不到就别验。
// 造一套假 SDK = 推理路径**真的被编译、真的被跑**,
// 而且能被**完全控制**(输出张量是脚本给的,输入张量是记录下来的)。
//
// 拿假的算子去验"真算子接得上吗"是自欺欺人;
// 但拿**真的 dl_infer 源码**去跑**假 SDK**,验的是 dl_infer 自己的逻辑 ——
// 那是本项目的目的。
//
// === 依赖面 ===
//
// 零 OpenCV、零 Qt、零 ONNX Runtime。纯 C++17。
// 只实现 dl_infer.cpp 实际用到的那些 API(一共 8 个,见下表)。
//
// ============================================================
// 它实现了哪些 API(只有这些,别的不实现)
// ============================================================
//
//   Ort::Env(LoggingLevel, const char*)
//   Ort::Env::GetAllocatorInfo()                     -> const Ort::AllocatorInfo*
//   Ort::SessionOptions::SetGraphOptimizationLevel(GraphOptimizationLevel)
//   Ort::SessionOptions::SetIntraOpNumThreads(int)
//   Ort::Session(Env&, const char* model_path, const SessionOptions&)
//   Ort::Session::GetInputNameAllocated(size_t, const Ort::AllocatorInfo*)
//   Ort::Session::GetOutputNameAllocated(size_t, const Ort::AllocatorInfo*)
//   Ort::Session::Run(RunOptions, const char* const*, const Value*, size_t,
//                      const char* const*, size_t)  -> std::vector<Value>
//   Ort::Value::CreateTensor<T>(const Ort::AllocatorInfo*, T*, size_t,
//                                const int64_t*, size_t)
//   Ort::Value::GetTensorTypeAndShapeInfo()         -> TensorTypeAndShapeInfo
//   Ort::Value::GetTensorData<T>()                   -> const T*
//   TensorTypeAndShapeInfo::GetShape() / GetElementCount()
//   Ort::Exception::what()
//
// ============================================================
// ⚠ 签名是**照着 dl_infer.cpp 的调用**定的,不是照着真 SDK 的头抄的
// ============================================================
//
// 本机没装真 SDK,抄不了它的头文件。这里能做的只有:
//   1. 用**指针**形式(GetAllocatorInfo() 返回 const Ort::AllocatorInfo*,
//      CreateTensor / GetInputNameAllocated 收 const Ort::AllocatorInfo*)
//      —— 真 ONNX Runtime 的 CreateTensor 和 GetInputNameAllocated
//      一直就是指针形式,所以 dl_infer.cpp 里那两行**不用改**。
//   2. 真的 SDK 里 Env::GetAllocatorInfo() 的返回类型在版本间变过
//      (有的版本返回**值**)。要是你装的版本返回的是值,
//      dl_infer.cpp 那两行要加一个 `&`。
//      → 这一条登记在 notes/notes_W7_RunEngine_2026-10.md 的片 4 一节。
//
// ============================================================
// 假 SDK 故意比真的**严格**的地方
// ============================================================
//
// 1. **模型文件不存在 -> 抛 Ort::Exception**。
//    真 SDK 也是这样(加载不出来的模型会抛)。
//    这一条让 dl_infer 的"模型加载失败"分支变得可测。
//
// 2. **名字对了才对**。
//    Run() 会检查传进来的 input/output 名字是不是脚本里登记的那些,
//    不是就抛异常。
//    → 这一条是**故意**的:见下面"它抓到了什么"。
//
// 3. **释放掉的字符串内存会被清零**。
//    真 SDK 释放之后那块内存是"未定义" —— 你读它可能拿到原来的字符串,
//    也可能拿到一堆垃圾,取决于分配器干了什么。
//    假 SDK 把它清零,于是"用了已释放的内存"变成**确定**的错,而不是
//    偶尔对偶尔错。
//
// → 它抓到了什么:dl_infer.cpp 里
//       const char* inNames[] = {session.GetInputNameAllocated(0, ...).get()};
//   这个临时 AllocatedStringPtr 在整条语句结束时就死了,
//   inNames[0] 指向的是**已释放的内存** —— 真 SDK 上是 UAF,
//   假 SDK 上是确定的空串 -> Run() 报"名字不对" -> 测试抓得到。
//
// ============================================================

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace Ort { class Value; }

// ============================================================
// 日志级别 / 图优化级别
// ============================================================
//
// ⚠ 这两个枚举在**全局作用域**,不在 Ort 命名空间里 ——
//   真 ONNX Runtime 就是这么放的(onnxruntime_cxx_api.h 顶部),
//   dl_infer.cpp 用的就是 `GraphOptimizationLevel::ORT_ENABLE_ALL`
//   和 `ORT_LOGGING_LEVEL_WARNING` 这种**不带前缀**的写法。
//   放进 Ort 里的话,dl_infer.cpp 一个字不改都编不过 ——
//   那种"假 SDK 和真 SDK 长得不一样"的坑最难查。
//
enum LoggingLevel {
    ORT_LOGGING_LEVEL_VERBOSE = 0,
    ORT_LOGGING_LEVEL_INFO    = 1,
    ORT_LOGGING_LEVEL_WARNING = 2,
    ORT_LOGGING_LEVEL_ERROR   = 3,
    ORT_LOGGING_LEVEL_FATAL   = 4
};

enum GraphOptimizationLevel {
    ORT_DISABLE_ALL     = 0,
    ORT_ENABLE_BASIC    = 1,
    ORT_ENABLE_EXTENDED = 2,
    ORT_ENABLE_ALL      = 3
};

// ============================================================
// fakeOrt —— 测试用的控制面
// ============================================================
//
// 假 SDK 的"输入输出"在这里被**脚本**掉:
//   · 输出张量(形状 + 数据)由测试设定 —— 想让模型吐出什么就吐什么
//   · 输入张量(形状 + 数据)被记下来 —— 用来验预处理做对没有
//
// ⚠ 这些函数**只有测试用**。dl_infer.cpp 一行都不调它们。
//   生产代码只调上面 Ort:: 里的那些。
//
// (这个命名空间放在 Ort 前面:Ort::Session 要调它,
//  所以得先有声明 —— 定义在文件末尾。)
//
namespace fakeOrt {

// --- 名字 ---
inline std::string& inputNameRef(size_t i) {
    static std::string s = "images";
    (void)i;
    return s;
}
inline std::string& outputNameRef(size_t i) {
    static std::string s = "output0";
    (void)i;
    return s;
}
inline const std::string& inputName(size_t i)  { return inputNameRef(i); }
inline const std::string& outputName(size_t i) { return outputNameRef(i); }

// --- 脚本化的输出 ---
inline std::vector<int64_t>& outputShapeRef() {
    static std::vector<int64_t> s;
    return s;
}
inline std::vector<float>& outputDataRef() {
    static std::vector<float> s;
    return s;
}

// --- 记下来的输入 ---
inline std::vector<int64_t>& lastInputShapeRef() {
    static std::vector<int64_t> s;
    return s;
}
inline std::vector<float>& lastInputDataRef() {
    static std::vector<float> s;
    return s;
}
inline int& runCountRef() {
    static int n = 0;
    return n;
}
inline int& sessionCountRef() {
    static int n = 0;
    return n;
}
inline bool& failModelLoadRef() {
    static bool b = false;
    return b;
}

// ============================================================
// 测试要用的函数
// ============================================================

// 全部复位。**每个用例开头都要调** ——
// 不然上一个用例的脚本会漏到下一个用例里,
// 那是最难查的一类"看起来对其实不对"。
inline void reset() {
    outputShapeRef().clear();
    outputDataRef().clear();
    lastInputShapeRef().clear();
    lastInputDataRef().clear();
    runCountRef() = 0;
    sessionCountRef() = 0;
    failModelLoadRef() = false;
    inputNameRef(0)  = "images";
    outputNameRef(0) = "output0";
}

// 设定"模型吐出来的张量"。
inline void setOutputTensor(const std::vector<int64_t>& shape,
                             const std::vector<float>& data) {
    outputShapeRef() = shape;
    outputDataRef()  = data;
}

// 让"加载模型"这一步失败(哪怕文件是存在的)。
// 真 SDK 也会这样:文件在,但不是合法的模型 -> 加载抛异常。
// 有了它,dl_infer 的 catch (Ort::Exception&) 那条分支才测得到。
inline void setFailModelLoad(bool b) { failModelLoadRef() = b; }

// --- 给测试读的 ---
inline std::vector<int64_t> lastInputShape() { return lastInputShapeRef(); }
inline std::vector<float>   lastInputData()  { return lastInputDataRef(); }
inline int runCount() { return runCountRef(); }

// 会话被创建了几次 —— 用来看 dl_infer 的"模型缓存"有没有生效。
// (加载几百 MB 的权重一次要 1~2 秒,每帧都付这个代价不可接受。)
inline int sessionCount() { return sessionCountRef(); }

// --- 需要完整的 Ort::Value,定义在文件末尾 ---
void recordInput(const Ort::Value* v);
Ort::Value makeOutput();

} // namespace fakeOrt

namespace Ort {

// ------------------------------------------------------------
// 异常
// ------------------------------------------------------------
// 真 SDK 里的 Ort::Exception 继承 std::exception,
// dl_infer 靠 `catch (const Ort::Exception& e)` 抓它并读 e.what()。
// 所以这两点必须对:得是**引用**能接住,what() 得给得出原因。
class Exception : public std::exception {
public:
    explicit Exception(std::string msg) : m_msg(std::move(msg)) {}
    const char* what() const noexcept override { return m_msg.c_str(); }
private:
    std::string m_msg;
};

// ------------------------------------------------------------
// 分配器信息
// ------------------------------------------------------------
// 真 SDK 里是个不透明句柄,这里也只需要一个"存在的东西"。
class AllocatorInfo {
public:
    AllocatorInfo() = default;
};

// ------------------------------------------------------------
// 环境
// ------------------------------------------------------------
class Env {
public:
    Env(LoggingLevel /*level*/, const char* /*name*/) {}

    // ⚠ 返回**指针** —— 真 SDK 里 CreateTensor / GetInputNameAllocated
    //   收的就是 const Ort::AllocatorInfo*,这样 dl_infer.cpp 那两行
    //   不用改就能对上真 API(见文件头那段说明)。
    const Ort::AllocatorInfo* GetAllocatorInfo() const {
        static const Ort::AllocatorInfo s_info;
        return &s_info;
    }
};

// ------------------------------------------------------------
// 会话选项
// ------------------------------------------------------------
class SessionOptions {
public:
    void SetGraphOptimizationLevel(GraphOptimizationLevel /*level*/) {}
    void SetIntraOpNumThreads(int /*n*/) {}
};

// ------------------------------------------------------------
// 分配好的字符串
// ------------------------------------------------------------
// 真 SDK 里这个类型是**只能移动**的(它管着一块要被释放的内存)。
// 这里也做成只能移动 —— 不是为了像,是因为"谁负责释放"这件事
// 正是 UAF 的来源,测试必须面对它。
class AllocatedStringPtr {
public:
    explicit AllocatedStringPtr(const char* s) {
        const size_t n = std::strlen(s);
        m_buf = new char[n + 1];
        std::memcpy(m_buf, s, n + 1);
    }
    ~AllocatedStringPtr() {
        if (m_buf) {
            // ⚠ 清零再释放 —— 见文件头"故意比真的严格的地方"第 3 条。
            //   真 SDK 释放之后那块内存是"未定义"的;
            //   这里让它确定地变成空串,于是"用了已释放的内存"
            //   变成一个**确定的**错,而不是偶尔对偶尔错。
            std::memset(m_buf, 0, std::strlen(m_buf) + 1);
            delete[] m_buf;
        }
    }
    AllocatedStringPtr(const AllocatedStringPtr&)            = delete;
    AllocatedStringPtr& operator=(const AllocatedStringPtr&) = delete;
    AllocatedStringPtr(AllocatedStringPtr&& o) noexcept : m_buf(o.m_buf) { o.m_buf = nullptr; }
    AllocatedStringPtr& operator=(AllocatedStringPtr&& o) noexcept {
        if (this != &o) { delete[] m_buf; m_buf = o.m_buf; o.m_buf = nullptr; }
        return *this;
    }
    const char* get() const { return m_buf; }
private:
    char* m_buf = nullptr;
};

// ------------------------------------------------------------
// 张量类型与形状
// ------------------------------------------------------------
class TensorTypeAndShapeInfo {
public:
    std::vector<int64_t> GetShape() const { return m_shape; }
    size_t GetElementCount() const {
        size_t n = 1;
        for (int64_t d : m_shape) n *= static_cast<size_t>(d);
        return n;
    }
    std::vector<int64_t> m_shape;
};

// ------------------------------------------------------------
// 张量值
// ------------------------------------------------------------
// ⚠ 只支持 float —— 真 SDK 的 Value 是类型擦除的,
//   但这个假 SDK 只服务一个调用方(dl_infer 只用 float),
//   所以**明确拒绝**别的类型:假的东西宁可窄,也不要悄悄接受
//   真 SDK 未必接受的东西。
class Value {
public:
    Value() = default;

    template <typename T>
    const T* GetTensorData() const {
        static_assert(std::is_same<T, float>::value,
                      "假 ONNX Runtime 只支持 float 张量");
        return m_data.data();
    }

    TensorTypeAndShapeInfo GetTensorTypeAndShapeInfo() const {
        TensorTypeAndShapeInfo info;
        info.m_shape = m_shape;
        return info;
    }

    // 真 SDK 是**拷贝**数据进去的(调用方那块内存可以马上释放)。
    // 这里也拷贝 —— 不然测的就不是 dl_infer 的行为了。
    template <typename T>
    static Value CreateTensor(const Ort::AllocatorInfo* /*allocator*/,
                              T* p_data, size_t p_data_len,
                              const int64_t* shape, size_t shape_len) {
        static_assert(std::is_same<T, float>::value,
                      "假 ONNX Runtime 只支持 float 张量");
        Value v;
        v.m_shape.assign(shape, shape + shape_len);
        v.m_data.assign(p_data, p_data + p_data_len);
        return v;
    }

    // 引用形式(有些 SDK 版本是这么写的)—— 留着,免得调用方要改。
    template <typename T>
    static Value CreateTensor(const Ort::AllocatorInfo& /*allocator*/,
                              T* p_data, size_t p_data_len,
                              const int64_t* shape, size_t shape_len) {
        return CreateTensor<T>(static_cast<const Ort::AllocatorInfo*>(nullptr),
                               p_data, p_data_len, shape, shape_len);
    }

    const std::vector<int64_t>& shape() const { return m_shape; }
    const std::vector<float>&   data()  const { return m_data; }

private:
    std::vector<int64_t> m_shape;
    std::vector<float>   m_data;
};

// ------------------------------------------------------------
// 运行选项
// ------------------------------------------------------------
struct RunOptions {
    explicit RunOptions(void* /*p*/ = nullptr) {}
};

// ------------------------------------------------------------
// 会话
// ------------------------------------------------------------
class Session {
public:
    Session(const Env& /*env*/, const char* model_path, const SessionOptions& /*opts*/) {
        ++fakeOrt::sessionCountRef();
        // ⚠ 模型文件不存在 -> 抛异常(真 SDK 也这样)。
        //   这条让 dl_infer 的"模型加载失败"分支变得**可测**。
        std::error_code ec;
        if (!std::filesystem::exists(std::filesystem::path(model_path ? model_path : ""), ec)) {
            throw Ort::Exception(std::string("fake-ort: 模型文件不存在: ") +
                                 (model_path ? model_path : "(null)"));
        }
        // 测试也可以**强制**加载失败(文件在,但不是合法模型)。
        if (fakeOrt::failModelLoadRef()) {
            throw Ort::Exception("fake-ort: 这不是合法的模型文件(测试强制失败)");
        }
    }

    AllocatedStringPtr GetInputNameAllocated(size_t index, const Ort::AllocatorInfo* /*alloc*/) const {
        const std::string n = fakeOrt::inputName(index);
        return AllocatedStringPtr(n.c_str());
    }

    AllocatedStringPtr GetOutputNameAllocated(size_t index, const Ort::AllocatorInfo* /*alloc*/) const {
        const std::string n = fakeOrt::outputName(index);
        return AllocatedStringPtr(n.c_str());
    }

    std::vector<Value> Run(const RunOptions& /*run_options*/,
                           const char* const* input_names,
                           const Value* input_values, size_t input_count,
                           const char* const* output_names, size_t output_count) const {
        // ⚠ 名字必须对 —— 见文件头"故意比真的严格的地方"第 2 条。
        for (size_t i = 0; i < input_count; ++i) {
            const std::string got = input_names && input_names[i] ? input_names[i] : "";
            const std::string want = fakeOrt::inputName(i);
            if (got != want) {
                throw Ort::Exception("fake-ort: 输入名不对(期望 \"" + want +
                                     "\", 实际 \"" + got +
                                     "\")—— 已释放的内存被清零了,这里读到的是空串");
            }
        }
        for (size_t i = 0; i < output_count; ++i) {
            const std::string got = output_names && output_names[i] ? output_names[i] : "";
            const std::string want = fakeOrt::outputName(i);
            if (got != want) {
                throw Ort::Exception("fake-ort: 输出名不对(期望 \"" + want +
                                     "\", 实际 \"" + got + "\")");
            }
        }

        // 记下收到的输入 —— 测"预处理做对没有"就靠它。
        fakeOrt::recordInput(input_count > 0 && input_values ? &input_values[0] : nullptr);

        // 输出是**脚本**给的 —— 测试想让模型吐出什么就吐什么。
        std::vector<Value> outs;
        outs.push_back(fakeOrt::makeOutput());
        return outs;
    }
};

} // namespace Ort

// ============================================================
// fakeOrt 里需要完整 Ort::Value 的那两个
// ============================================================
namespace fakeOrt {

// 记下收到的输入张量。
inline void recordInput(const Ort::Value* v) {
    ++runCountRef();
    if (!v) { lastInputShapeRef().clear(); lastInputDataRef().clear(); return; }
    lastInputShapeRef() = v->shape();
    lastInputDataRef()  = v->data();
}

// 造出脚本里的输出张量。
inline Ort::Value makeOutput() {
    if (outputShapeRef().empty()) {
        throw Ort::Exception("fake-ort: 测试没有设定输出张量(setOutputTensor)");
    }
    std::vector<int64_t> shape = outputShapeRef();
    std::vector<float>   data  = outputDataRef();
    return Ort::Value::CreateTensor<float>(static_cast<const Ort::AllocatorInfo*>(nullptr),
                                           data.data(), data.size(),
                                           shape.data(), shape.size());
}

} // namespace fakeOrt
