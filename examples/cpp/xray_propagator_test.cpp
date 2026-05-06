#include "xray_propagator.h"
#include <cassert>
#include <iostream>

void test_valid_header() {
    auto ctx = AwsXRayPropagator::parse(
        "Root=1-69fbb89b-61b4b8813ed9af665d19b7cf;Parent=35522aaea7971245;Sampled=1");
    assert(ctx.has_value());
    assert(ctx->trace_id == "69fbb89b61b4b8813ed9af665d19b7cf");
    assert(ctx->parent_span_id == "35522aaea7971245");
    assert(ctx->sampled == true);
    std::cout << "PASS: test_valid_header\n";
}

void test_field_order_independence() {
    auto ctx = AwsXRayPropagator::parse(
        "Sampled=1;Parent=abcdef0123456789;Root=1-aabbccdd-112233445566778899aabbcc");
    assert(ctx.has_value());
    assert(ctx->trace_id == "aabbccdd112233445566778899aabbcc");
    assert(ctx->parent_span_id == "abcdef0123456789");
    assert(ctx->sampled == true);
    std::cout << "PASS: test_field_order_independence\n";
}

void test_additional_fields_ignored() {
    auto ctx = AwsXRayPropagator::parse(
        "Root=1-69fbb89b-61b4b8813ed9af665d19b7cf;Parent=35522aaea7971245;Sampled=1;Self=1-abc-def;Lineage=abc123");
    assert(ctx.has_value());
    assert(ctx->trace_id == "69fbb89b61b4b8813ed9af665d19b7cf");
    assert(ctx->parent_span_id == "35522aaea7971245");
    std::cout << "PASS: test_additional_fields_ignored\n";
}

void test_sampled_zero() {
    auto ctx = AwsXRayPropagator::parse(
        "Root=1-69fbb89b-61b4b8813ed9af665d19b7cf;Parent=35522aaea7971245;Sampled=0");
    assert(ctx.has_value());
    assert(ctx->sampled == false);
    std::cout << "PASS: test_sampled_zero\n";
}

void test_missing_parent() {
    auto ctx = AwsXRayPropagator::parse(
        "Root=1-69fbb89b-61b4b8813ed9af665d19b7cf;Sampled=1");
    assert(ctx.has_value());
    assert(ctx->trace_id == "69fbb89b61b4b8813ed9af665d19b7cf");
    assert(ctx->parent_span_id.empty());
    std::cout << "PASS: test_missing_parent\n";
}

void test_malformed_root_too_short() {
    auto ctx = AwsXRayPropagator::parse("Root=1-abc;Parent=35522aaea7971245;Sampled=1");
    assert(!ctx.has_value());
    std::cout << "PASS: test_malformed_root_too_short\n";
}

void test_malformed_root_wrong_version() {
    auto ctx = AwsXRayPropagator::parse(
        "Root=2-69fbb89b-61b4b8813ed9af665d19b7cf;Parent=35522aaea7971245;Sampled=1");
    assert(!ctx.has_value());
    std::cout << "PASS: test_malformed_root_wrong_version\n";
}

void test_malformed_root_non_hex() {
    auto ctx = AwsXRayPropagator::parse(
        "Root=1-ZZZZZZZZ-61b4b8813ed9af665d19b7cf;Parent=35522aaea7971245;Sampled=1");
    assert(!ctx.has_value());
    std::cout << "PASS: test_malformed_root_non_hex\n";
}

void test_malformed_parent_wrong_length() {
    auto ctx = AwsXRayPropagator::parse(
        "Root=1-69fbb89b-61b4b8813ed9af665d19b7cf;Parent=abc;Sampled=1");
    assert(ctx.has_value());
    assert(ctx->parent_span_id.empty());
    std::cout << "PASS: test_malformed_parent_wrong_length\n";
}

void test_empty_header() {
    auto ctx = AwsXRayPropagator::parse("");
    assert(!ctx.has_value());
    std::cout << "PASS: test_empty_header\n";
}

void test_no_root_field() {
    auto ctx = AwsXRayPropagator::parse("Parent=35522aaea7971245;Sampled=1");
    assert(!ctx.has_value());
    std::cout << "PASS: test_no_root_field\n";
}

void test_case_insensitive_header_lookup() {
    std::unordered_map<std::string, std::string> headers = {
        {"X-AMZN-TRACE-ID", "Root=1-69fbb89b-61b4b8813ed9af665d19b7cf;Parent=35522aaea7971245;Sampled=1"}
    };
    auto ctx = AwsXRayPropagator::extract(headers);
    assert(ctx.has_value());
    assert(ctx->trace_id == "69fbb89b61b4b8813ed9af665d19b7cf");
    std::cout << "PASS: test_case_insensitive_header_lookup\n";
}

void test_whitespace_tolerance() {
    auto ctx = AwsXRayPropagator::parse(
        "Root=1-69fbb89b-61b4b8813ed9af665d19b7cf ; Parent=35522aaea7971245 ; Sampled=1");
    assert(ctx.has_value());
    assert(ctx->trace_id == "69fbb89b61b4b8813ed9af665d19b7cf");
    assert(ctx->parent_span_id == "35522aaea7971245");
    std::cout << "PASS: test_whitespace_tolerance\n";
}

int main() {
    test_valid_header();
    test_field_order_independence();
    test_additional_fields_ignored();
    test_sampled_zero();
    test_missing_parent();
    test_malformed_root_too_short();
    test_malformed_root_wrong_version();
    test_malformed_root_non_hex();
    test_malformed_parent_wrong_length();
    test_empty_header();
    test_no_root_field();
    test_case_insensitive_header_lookup();
    test_whitespace_tolerance();

    std::cout << "\nAll 13 tests passed.\n";
    return 0;
}
