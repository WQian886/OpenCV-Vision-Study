#include "mini_json.h"

#include <cmath>
#include <cstdlib>
#include <sstream>

namespace mini_json {

// ============================================================
// Value 的取值
// ============================================================
bool Value::asBool(bool def) const {
    if (m_type == Type::Bool)   return m_bool;
    if (m_type == Type::Number) return m_number != 0.0;
    return def;
}

double Value::asNumber(double def) const {
    if (m_type == Type::Number) return m_number;
    return def;
}

std::string Value::asString(const std::string& def) const {
    if (m_type == Type::String) return m_string;
    return def;
}

const std::vector<Value>& Value::asArray() const {
    static const std::vector<Value> kEmpty;
    return m_type == Type::Array ? m_array : kEmpty;
}

const Value* Value::find(const std::string& key) const {
    if (m_type != Type::Object) return nullptr;
    auto it = m_object.find(key);
    return it == m_object.end() ? nullptr : &it->second;
}

const Value& Value::at(size_t i) const {
    static const Value kNull;
    if (m_type != Type::Array || i >= m_array.size()) return kNull;
    return m_array[i];
}

size_t Value::size() const {
    if (m_type == Type::Array)  return m_array.size();
    if (m_type == Type::Object) return m_object.size();
    return 0;
}

// ============================================================
// 解析器
// ============================================================
namespace {

class Parser {
public:
    Parser(const std::string& text) : m_s(text) {}

    bool run(Value* out, std::string* error) {
        skipWs();
        if (!parseValue(out, error)) return false;
        skipWs();
        if (m_i != m_s.size()) {
            fail(error, "结尾还有多余字符");
            return false;
        }
        return true;
    }

private:
    const std::string& m_s;
    size_t m_i = 0;

    // 出错信息要带位置,不然配方写坏了根本定位不到
    void fail(std::string* error, const std::string& what) const {
        if (!error) return;
        std::ostringstream os;
        os << "JSON 第 " << m_i << " 个字符处: " << what;
        *error = os.str();
    }

    void skipWs() {
        while (m_i < m_s.size()) {
            const char c = m_s[m_i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++m_i;
            else break;
        }
    }

    bool lit(const char* word) {
        const size_t n = std::char_traits<char>::length(word);
        if (m_s.compare(m_i, n, word) != 0) return false;
        m_i += n;
        return true;
    }

    bool parseValue(Value* out, std::string* error) {
        if (m_i >= m_s.size()) {
            // ⚠ 别写"内容为空" —— JSON 在这里截断(少了引号/括号)时也会
            //   走到这里,用户看到"内容为空"会以为文件是空的,
            //   而实际是语法错在第 N 个字符。消息要指向真正的原因。
            fail(error, "内容在这里就结束了(少了引号或括号?)");
            return false;
        }
        const char c = m_s[m_i];
        switch (c) {
            case '{': return parseObject(out, error);
            case '[': return parseArray(out, error);
            case '"': {
                std::string s;
                if (!parseString(&s, error)) return false;
                out->setString(std::move(s));
                return true;
            }
            case 't': if (lit("true"))  { out->setBool(true);  return true; }
                      fail(error, "期望 true"); return false;
            case 'f': if (lit("false")) { out->setBool(false); return true; }
                      fail(error, "期望 false"); return false;
            case 'n': if (lit("null"))  { out->setNull();       return true; }
                      fail(error, "期望 null"); return false;
            default:  return parseNumber(out, error);
        }
    }

    bool parseObject(Value* out, std::string* error) {
        ++m_i;                       // '{'
        out->setObject();
        skipWs();
        if (m_i < m_s.size() && m_s[m_i] == '}') { ++m_i; return true; }
        for (;;) {
            skipWs();
            if (m_i >= m_s.size() || m_s[m_i] != '"') {
                fail(error, "对象的键必须是字符串");
                return false;
            }
            std::string key;
            if (!parseString(&key, error)) return false;
            skipWs();
            if (m_i >= m_s.size() || m_s[m_i] != ':') {
                fail(error, "键后面缺 ':'");
                return false;
            }
            ++m_i;
            skipWs();
            Value v;
            if (!parseValue(&v, error)) return false;
            out->insert(key, std::move(v));
            skipWs();
            if (m_i < m_s.size() && m_s[m_i] == ',') { ++m_i; continue; }
            if (m_i < m_s.size() && m_s[m_i] == '}') { ++m_i; return true; }
            fail(error, "对象里缺 ',' 或 '}'");
            return false;
        }
    }

    bool parseArray(Value* out, std::string* error) {
        ++m_i;                       // '['
        std::vector<Value> items;
        skipWs();
        if (m_i < m_s.size() && m_s[m_i] == ']') { ++m_i; out->setArray(std::move(items)); return true; }
        for (;;) {
            skipWs();
            Value v;
            if (!parseValue(&v, error)) return false;
            items.push_back(std::move(v));
            skipWs();
            if (m_i < m_s.size() && m_s[m_i] == ',') { ++m_i; continue; }
            if (m_i < m_s.size() && m_s[m_i] == ']') { ++m_i; break; }
            fail(error, "数组里缺 ',' 或 ']'");
            return false;
        }
        out->setArray(std::move(items));
        return true;
    }

    bool parseNumber(Value* out, std::string* error) {
        const size_t start = m_i;
        if (m_i < m_s.size() && (m_s[m_i] == '-' || m_s[m_i] == '+')) ++m_i;
        // ⚠ 前导零必须拒绝("01" 不是合法 JSON)。
        //   宽松地接受它不会炸,但那是**静默地**接受 —— 写配方的人
        //   以为自己写了个 01,实际存的是 1。JSON 规范不允许,
        //   Qt 的 QJsonDocument 也不允许,这里跟它们保持一致:
        //   两个解析器对同一个输入给不同答案,是最难查的一类不一致。
        if (m_i < m_s.size() && m_s[m_i] == '0') {
            ++m_i;
            if (m_i < m_s.size() && std::isdigit(static_cast<unsigned char>(m_s[m_i]))) {
                fail(error, "数字不能有前导零");
                return false;
            }
        } else {
            bool any = false;
            while (m_i < m_s.size() && std::isdigit(static_cast<unsigned char>(m_s[m_i]))) {
                ++m_i; any = true;
            }
            if (!any) { fail(error, "这里应该是个数字(却不是)"); return false; }
        }
        if (m_i < m_s.size() && m_s[m_i] == '.') {
            ++m_i;
            while (m_i < m_s.size() && std::isdigit(static_cast<unsigned char>(m_s[m_i]))) {
                ++m_i;
            }
        }
        if (m_i < m_s.size() && (m_s[m_i] == 'e' || m_s[m_i] == 'E')) {
            ++m_i;
            if (m_i < m_s.size() && (m_s[m_i] == '-' || m_s[m_i] == '+')) ++m_i;
            while (m_i < m_s.size() && std::isdigit(static_cast<unsigned char>(m_s[m_i]))) ++m_i;
        }
        out->setNumber(std::strtod(m_s.substr(start, m_i - start).c_str(), nullptr));
        return true;
    }

    // 字符串:原样存 UTF-8 字节,只处理转义
    bool parseString(std::string* out, std::string* error) {
        ++m_i;                       // 开头的 '"'
        out->clear();
        while (m_i < m_s.size()) {
            const char c = m_s[m_i];
            if (c == '"') { ++m_i; return true; }
            if (c == '\\') {
                ++m_i;
                if (m_i >= m_s.size()) { fail(error, "转义符后面没了"); return false; }
                const char e = m_s[m_i++];
                switch (e) {
                    case '"':  out->push_back('"');  break;
                    case '\\': out->push_back('\\'); break;
                    case '/':  out->push_back('/');  break;
                    case 'b':  out->push_back('\b'); break;
                    case 'f':  out->push_back('\f'); break;
                    case 'n':  out->push_back('\n'); break;
                    case 'r':  out->push_back('\r'); break;
                    case 't':  out->push_back('\t'); break;
                    case 'u': {
                        unsigned cp = 0;
                        if (!readHex4(&cp, error)) return false;
                        appendUtf8(out, cp);
                        break;
                    }
                    default:
                        fail(error, "不认识的转义字符");
                        return false;
                }
                continue;
            }
            out->push_back(c);
            ++m_i;
        }
        fail(error, "字符串没闭合");
        return false;
    }

    bool readHex4(unsigned* out, std::string* error) {
        if (m_i + 4 > m_s.size()) { fail(error, "\\u 转义不够 4 位十六进制"); return false; }
        unsigned v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = m_s[m_i + k];
            v <<= 4;
            if (c >= '0' && c <= '9')      v |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
            else { fail(error, "\\u 转义里有非十六进制字符"); return false; }
        }
        m_i += 4;
        *out = v;
        return true;
    }

    // 码点 -> UTF-8 字节(标准做法:1/2/3/4 字节)
    static void appendUtf8(std::string* out, unsigned cp) {
        if (cp < 0x80) {
            out->push_back(char(cp));
        } else if (cp < 0x800) {
            out->push_back(char(0xC0 | (cp >> 6)));
            out->push_back(char(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out->push_back(char(0xE0 | (cp >> 12)));
            out->push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(char(0x80 | (cp & 0x3F)));
        } else {
            out->push_back(char(0xF0 | (cp >> 18)));
            out->push_back(char(0x80 | ((cp >> 12) & 0x3F)));
            out->push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(char(0x80 | (cp & 0x3F)));
        }
    }
};

} // namespace

bool parse(const std::string& text, Value* out, std::string* error) {
    if (!out) return false;
    Parser p(text);
    return p.run(out, error);
}

} // namespace mini_json
