#pragma once

#include <concepts>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace miplay::json {

struct Number {
    std::string text;
};

struct Value {
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value, std::less<>>;
    using Data = std::variant<std::nullptr_t, bool, Number, std::string, Array, Object>;

    Value() = default;
    Value(bool value) : data(value) {}
    template<std::integral T>
    Value(T value) : data(Number{std::to_string(value)}) {}
    Value(Number value) : data(std::move(value)) {}
    Value(const char* value) : data(std::string(value)) {}
    Value(std::string value) : data(std::move(value)) {}
    Value(Array value) : data(std::move(value)) {}
    Value(Object value) : data(std::move(value)) {}

    Data data = nullptr;
};

Value parse(std::string_view text);
std::string dump(const Value& value);
std::string quote(std::string_view text);

} // namespace miplay::json
