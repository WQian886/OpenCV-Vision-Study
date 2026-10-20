#pragma once
// ============================================================
// mini_json — 一个够用就好的 JSON 读取器(无依赖,纯标准库)
//
// ## 为什么自己写一个
//
// 板型配方要同时被 W6(命令行,不用 Qt)和 W7(Qt 算子)读。
// 如果两边各写一个解析器,那就是**同一个格式两份实现** ——
// 本项目在这件事上已经栽过四遍(EXPECTS[6] / lo[0] 越界守卫 /
// 守卫本身 / 两处金层掩膜漂开),每一遍都是"改了一处,另一处没改"。
//
// 所以解析器也只留一个:W7 的 board_recipe 改成调用这里,
// W6 直接调用这里。格式由 tools/export_recipes.py 产出,
// 解析器跟着格式走,两边不可能各自漂开。
//
// ## 支持范围
//
// 对象 / 数组 / 字符串 / 数字 / 布尔 / null —— 够了。
// 写盘(序列化)不支持:配方只读不写,写盘由 Python 那边的 json.dump 负责。
//
// ## UTF-8
//
// 字符串按字节存(std::string),不转码。配方里的中文标题就是 UTF-8 字节,
// 原样进出即可。\uXXXX 转义也支持(转成 UTF-8 字节序列),
// 虽然导出脚本写的是 ensure_ascii=False 的原始 UTF-8,基本用不上这条。
// ============================================================

#include <string>
#include <vector>
#include <map>

namespace mini_json {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() : m_type(Type::Null) {}

    Type type() const { return m_type; }
    bool isNull()   const { return m_type == Type::Null; }
    bool isBool()   const { return m_type == Type::Bool; }
    bool isNumber() const { return m_type == Type::Number; }
    bool isString() const { return m_type == Type::String; }
    bool isArray()  const { return m_type == Type::Array; }
    bool isObject() const { return m_type == Type::Object; }

    bool               asBool(bool def = false) const;
    double             asNumber(double def = 0.0) const;
    std::string        asString(const std::string& def = std::string()) const;
    const std::vector<Value>& asArray() const;

    // 对象取成员。**找不到返回 nullptr**,不返回默认值 ——
    // "字段不存在"和"字段存在但是 0"是两件不同的事,
    // 配方里 shift_tol_pct 就有这个区别(缺省 = 用默认容差)。
    const Value* find(const std::string& key) const;
    bool has(const std::string& key) const { return find(key) != nullptr; }

    // 数组取元素
    const Value& at(size_t i) const;
    size_t size() const;

    // ---- 构造(只给解析器用) ----
    void setNull()               { m_type = Type::Null; }
    void setBool(bool v)         { m_type = Type::Bool;   m_bool = v; }
    void setNumber(double v)     { m_type = Type::Number; m_number = v; }
    void setString(std::string v){ m_type = Type::String; m_string = std::move(v); }
    void setArray(std::vector<Value> v) { m_type = Type::Array;  m_array = std::move(v); }
    void setObject()             { m_type = Type::Object; m_object.clear(); }
    void insert(const std::string& key, Value v) { m_object[key] = std::move(v); }

private:
    Type        m_type;
    bool        m_bool   = false;
    double      m_number = 0.0;
    std::string m_string;
    std::vector<Value> m_array;
    std::map<std::string, Value> m_object;
};

// 解析一段 JSON 文本。
//
// 失败时 error 里写**人话**:出错的字符位置 + 期望什么。
// 这条很重要 —— 配方写坏了的时候,"配方加载失败"四个字等于没说话,
// 产线上要靠这句话定位是哪个字段写错了。
bool parse(const std::string& text, Value* out, std::string* error);

} // namespace mini_json
