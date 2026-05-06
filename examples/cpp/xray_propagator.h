#pragma once

#include <string>
#include <optional>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_map>

struct XRayTraceContext {
    std::string trace_id;
    std::string parent_span_id;
    bool sampled = true;
};

class AwsXRayPropagator {
public:
    static std::optional<XRayTraceContext> extract(
        const std::unordered_map<std::string, std::string>& headers) {

        std::string header_value;
        for (const auto& [key, value] : headers) {
            std::string lower_key = to_lower(key);
            if (lower_key == "x-amzn-trace-id") {
                header_value = value;
                break;
            }
        }

        if (header_value.empty()) {
            return std::nullopt;
        }

        return parse(header_value);
    }

    static std::optional<XRayTraceContext> parse(const std::string& header_value) {
        auto fields = split_fields(header_value);

        auto root_it = fields.find("Root");
        if (root_it == fields.end()) {
            return std::nullopt;
        }

        auto trace_id = parse_root(root_it->second);
        if (!trace_id.has_value()) {
            return std::nullopt;
        }

        XRayTraceContext ctx;
        ctx.trace_id = trace_id.value();

        auto parent_it = fields.find("Parent");
        if (parent_it != fields.end()) {
            auto span_id = validate_span_id(parent_it->second);
            if (span_id.has_value()) {
                ctx.parent_span_id = span_id.value();
            }
        }

        auto sampled_it = fields.find("Sampled");
        if (sampled_it != fields.end()) {
            ctx.sampled = (sampled_it->second != "0");
        }

        return ctx;
    }

private:
    static std::string to_lower(const std::string& s) {
        std::string result = s;
        std::transform(result.begin(), result.end(), result.begin(),
            [](unsigned char c) { return std::tolower(c); });
        return result;
    }

    static std::unordered_map<std::string, std::string> split_fields(
        const std::string& header) {
        std::unordered_map<std::string, std::string> fields;
        std::istringstream stream(header);
        std::string token;

        while (std::getline(stream, token, ';')) {
            token.erase(0, token.find_first_not_of(' '));
            token.erase(token.find_last_not_of(' ') + 1);

            auto eq_pos = token.find('=');
            if (eq_pos != std::string::npos) {
                std::string key = token.substr(0, eq_pos);
                std::string value = token.substr(eq_pos + 1);
                fields[key] = value;
            }
        }

        return fields;
    }

    static std::optional<std::string> parse_root(const std::string& root_value) {
        if (root_value.size() < 35) {
            return std::nullopt;
        }

        if (root_value[0] != '1' || root_value[1] != '-') {
            return std::nullopt;
        }

        auto second_dash = root_value.find('-', 2);
        if (second_dash == std::string::npos || second_dash != 10) {
            return std::nullopt;
        }

        std::string timestamp_hex = root_value.substr(2, 8);
        std::string random_hex = root_value.substr(11);

        if (timestamp_hex.size() != 8 || !is_hex(timestamp_hex)) {
            return std::nullopt;
        }

        if (random_hex.size() != 24 || !is_hex(random_hex)) {
            return std::nullopt;
        }

        return timestamp_hex + random_hex;
    }

    static std::optional<std::string> validate_span_id(const std::string& span_id) {
        if (span_id.size() != 16 || !is_hex(span_id)) {
            return std::nullopt;
        }
        return span_id;
    }

    static bool is_hex(const std::string& s) {
        return std::all_of(s.begin(), s.end(),
            [](unsigned char c) { return std::isxdigit(c); });
    }
};
