#pragma once

#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace matonos::install::json {

struct Value {
    enum class Kind { kNull, kBool, kNumber, kString, kArray, kObject } kind = Kind::kNull;
    std::string scalar;
    std::vector<Value> array;
    std::map<std::string, Value> object;
    const Value* Get(const char* key) const {
        auto it = object.find(key);
        return it == object.end() ? nullptr : &it->second;
    }
    std::string String(const char* fallback = "") const {
        return kind == Kind::kString ? scalar : fallback;
    }
    uint64_t Number(uint64_t fallback = 0) const {
        if (kind != Kind::kNumber || scalar.empty() || scalar[0] == '-') return fallback;
        char* end = nullptr;
        const auto value = std::strtoull(scalar.c_str(), &end, 10);
        return end && *end == '\0' ? value : fallback;
    }
};

class Parser {
  public:
    explicit Parser(const std::string& input) : input_(input) {}
    bool Parse(Value* result) {
        Skip();
        if (!ValueAt(result, 0)) return false;
        Skip();
        return pos_ == input_.size();
    }
  private:
    void Skip() { while (pos_ < input_.size() && (input_[pos_] == ' ' || input_[pos_] == '\n' || input_[pos_] == '\r' || input_[pos_] == '\t')) ++pos_; }
    bool Take(char c) { if (pos_ < input_.size() && input_[pos_] == c) { ++pos_; return true; } return false; }
    bool String(std::string* output) {
        if (!Take('"')) return false;
        output->clear();
        while (pos_ < input_.size()) {
            unsigned char c = input_[pos_++];
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') { output->push_back(static_cast<char>(c)); continue; }
            if (pos_ == input_.size()) return false;
            switch (input_[pos_++]) {
                case '"': output->push_back('"'); break;
                case '\\': output->push_back('\\'); break;
                case '/': output->push_back('/'); break;
                case 'b': output->push_back('\b'); break;
                case 'f': output->push_back('\f'); break;
                case 'n': output->push_back('\n'); break;
                case 'r': output->push_back('\r'); break;
                case 't': output->push_back('\t'); break;
                // Control characters in these operation fields are never needed.
                default: return false;
            }
        }
        return false;
    }
    bool ValueAt(Value* value, unsigned depth) {
        if (depth > 24) return false;
        Skip();
        if (pos_ >= input_.size()) return false;
        if (input_[pos_] == '"') { value->kind = Value::Kind::kString; return String(&value->scalar); }
        if (Take('{')) {
            value->kind = Value::Kind::kObject; Skip();
            if (Take('}')) return true;
            do {
                std::string key; Skip();
                if (!String(&key)) return false;
                Skip(); if (!Take(':')) return false;
                Value child;
                if (!ValueAt(&child, depth + 1) || !value->object.emplace(key, std::move(child)).second) return false;
                Skip(); if (Take('}')) return true;
            } while (Take(','));
            return false;
        }
        if (Take('[')) {
            value->kind = Value::Kind::kArray; Skip();
            if (Take(']')) return true;
            do {
                Value child;
                if (!ValueAt(&child, depth + 1)) return false;
                value->array.push_back(std::move(child)); Skip();
                if (Take(']')) return true;
            } while (Take(','));
            return false;
        }
        if (input_.compare(pos_, 4, "true") == 0) { pos_ += 4; value->kind = Value::Kind::kBool; value->scalar = "true"; return true; }
        if (input_.compare(pos_, 5, "false") == 0) { pos_ += 5; value->kind = Value::Kind::kBool; value->scalar = "false"; return true; }
        if (input_.compare(pos_, 4, "null") == 0) { pos_ += 4; return true; }
        const size_t start = pos_;
        if (Take('-')) return false;
        if (Take('0')) {
            if (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') return false;
        } else {
            if (pos_ >= input_.size() || input_[pos_] < '1' || input_[pos_] > '9') return false;
            while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') ++pos_;
        }
        // Operation numeric values are integral byte counts.
        value->kind = Value::Kind::kNumber; value->scalar = input_.substr(start, pos_ - start); return true;
    }
    const std::string& input_;
    size_t pos_ = 0;
};

inline bool Parse(const std::string& input, Value* result) { return Parser(input).Parse(result); }

}  // namespace matonos::install::json
