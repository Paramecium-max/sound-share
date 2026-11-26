#include "util/json.h"
#include "util/utf8.h"

#include <stdexcept>
#include <type_traits>
#include <utility>

namespace miplay::json {
namespace {

class Reader {
public:
    explicit Reader(std::string_view text) : input_(text) {}

    Value parse() {
        Value value = read(0);
        skip_space();
        if (!input_.empty()) invalid();
        return value;
    }

private:
    [[noreturn]] static void invalid() { throw std::invalid_argument("无效的 JSON 请求"); }

    void skip_space() {
        while (!input_.empty() && std::string_view(" \t\r\n").find(input_.front()) != std::string_view::npos) {
            input_.remove_prefix(1);
        }
    }

    char peek() const { return input_.empty() ? '\0' : input_.front(); }

    char next() {
        if (input_.empty()) invalid();
        char c = input_.front();
        input_.remove_prefix(1);
        return c;
    }

    bool eat(char c) {
        if (!input_.empty() && peek() == c) {
            next();
            return true;
        }
        return false;
    }

    void require(char c) {
        if (!eat(c)) invalid();
    }

    void literal(std::string_view text) {
        if (!input_.starts_with(text)) invalid();
        input_.remove_prefix(text.size());
    }

    uint32_t hex4() {
        uint32_t code = 0;
        for (unsigned i = 0; i < 4; ++i) {
            const char c = next();
            unsigned nibble = c >= '0' && c <= '9' ? c - '0' :
                              c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                              c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
            if (nibble == 16) invalid();
            code = (code << 4) | nibble;
        }
        return code;
    }

    std::string string() {
        require('"');
        std::string out;
        while (!eat('"')) {
            if (static_cast<unsigned char>(peek()) < 0x20) invalid();
            if (!eat('\\')) {
                const size_t size = utf8::scalar_length(input_);
                if (!size) invalid();
                out += input_.substr(0, size);
                input_.remove_prefix(size);
                continue;
            }
            switch (next()) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t scalar = hex4();
                if (scalar >= 0xD800 && scalar <= 0xDBFF) {
                    literal("\\u");
                    const uint32_t low = hex4();
                    if (low < 0xDC00 || low > 0xDFFF) invalid();
                    scalar = 0x10000 + ((scalar - 0xD800) << 10) + low - 0xDC00;
                } else if (scalar >= 0xDC00 && scalar <= 0xDFFF) {
                    invalid();
                }
                utf8::append(out, scalar);
                break;
            }
            default: invalid();
            }
        }
        return out;
    }

    static bool digit(char c) { return c >= '0' && c <= '9'; }

    void consume_digits() {
        if (!digit(peek())) invalid();
        while (digit(peek())) next();
    }

    Number number() {
        const auto original = input_;
        eat('-');
        if (!eat('0')) consume_digits();
        if (eat('.')) consume_digits();
        if (eat('e') || eat('E')) {
            if (!eat('+')) eat('-');
            consume_digits();
        }
        return {std::string(original.substr(0, original.size() - input_.size()))};
    }

    Value read(unsigned depth) {
        if (depth >= 32) invalid();
        skip_space();
        switch (peek()) {
        case '"': return string();
        case 't': literal("true"); return true;
        case 'f': literal("false"); return false;
        case 'n': literal("null"); return {};
        case '[': {
            next();
            Value::Array array;
            skip_space();
            if (!eat(']')) {
                do {
                    array.push_back(read(depth + 1));
                    skip_space();
                } while (eat(','));
                require(']');
            }
            return array;
        }
        case '{': {
            next();
            Value::Object object;
            skip_space();
            if (!eat('}')) {
                do {
                    skip_space();
                    auto key = string();
                    skip_space();
                    require(':');
                    if (!object.emplace(std::move(key), read(depth + 1)).second) invalid();
                    skip_space();
                } while (eat(','));
                require('}');
            }
            return object;
        }
        default: return number();
        }
    }

    std::string_view input_;
};

void append(std::string& out, const Value& value) {
    std::visit([&](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::nullptr_t>) out += "null";
        else if constexpr (std::is_same_v<T, bool>) out += item ? "true" : "false";
        else if constexpr (std::is_same_v<T, Number>) out += item.text;
        else if constexpr (std::is_same_v<T, std::string>) out += quote(item);
        else if constexpr (std::is_same_v<T, Value::Array>) {
            out += '[';
            bool first = true;
            for (const auto& element : item) {
                if (!std::exchange(first, false)) out += ',';
                append(out, element);
            }
            out += ']';
        } else {
            out += '{';
            bool first = true;
            for (const auto& [key, element] : item) {
                if (!std::exchange(first, false)) out += ',';
                out += quote(key);
                out += ':';
                append(out, element);
            }
            out += '}';
        }
    }, value.data);
}

} // namespace

Value parse(std::string_view text) {
    return Reader(text).parse();
}

std::string quote(std::string_view text) {
    std::string out = "\"";
    constexpr char hex[] = "0123456789abcdef";
    while (!text.empty()) {
        const auto c = static_cast<unsigned char>(text.front());
        if (c < 0x20) {
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
            text.remove_prefix(1);
        } else if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
            text.remove_prefix(1);
        } else {
            const auto size = utf8::scalar_length(text);
            if (size) out += text.substr(0, size);
            else out += "\\ufffd";
            text.remove_prefix(size ? size : 1);
        }
    }
    out += '"';
    return out;
}

std::string dump(const Value& value) {
    std::string out;
    append(out, value);
    return out;
}

} // namespace miplay::json
