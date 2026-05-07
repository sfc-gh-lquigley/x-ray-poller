# C++ X-Ray Trace Propagation

## Difficulty compared to other languages

| Language | Effort | What you do |
|----------|--------|-------------|
| Java | ~2 min | Add one JVM flag: `-Dotel.propagators=xray,tracecontext` |
| Python | ~5 min | `pip install` one package + 4 lines of config |
| C++ | ~30 min | Copy one header file + add ~10 lines to your request handler |

C++ requires more effort because there is no official AWS X-Ray propagator package for C++. This directory provides a drop-in solution.

## What's included

| File | What it is |
|------|-----------|
| `xray_propagator.h` | Drop-in header (copy this into your project) |
| `xray_propagator_test.cpp` | 13 unit tests proving it handles edge cases |
| `main.cpp` | Complete working HTTP server example |
| `build.sh` | One-command build + test |

## Quick start (run the example)

```bash
# On any Linux with g++ and libcurl installed:
sudo yum install -y gcc-c++ libcurl-devel   # Amazon Linux
# or: sudo apt install g++ libcurl4-openssl-dev  # Ubuntu

# Build and test
chmod +x build.sh && ./build.sh

# Run (listens on port 8082, exports spans to localhost:4318)
./demo-backend-cpp
```

## Integrating into YOUR C++ service

### You need:
- Your service already handles HTTP requests somehow (any framework)
- An OTel collector running on localhost:4318 (or wherever you export spans)

### Step 1: Copy the header file

Copy `xray_propagator.h` into your project. It's a single file, no dependencies beyond `<string>`, `<optional>`, `<unordered_map>` (C++17 standard library).

### Step 2: Add to your request handler

Here's the minimal integration. Adapt the header extraction to your HTTP framework:

```cpp
#include "xray_propagator.h"

// In your request handler:
std::unordered_map<std::string, std::string> headers;
// ^^^ Populate this from your HTTP framework. Example:
// Boost.Beast:  for (auto& field : req) headers[field.name_string()] = field.value();
// cpp-httplib:  for (auto& h : req.headers) headers[h.first] = h.second;
// gRPC:         for (auto& md : context->client_metadata()) headers[md.first] = md.second;
// nginx module: extract from ngx_http_request_t->headers_in

auto ctx = AwsXRayPropagator::extract(headers);
```

### Step 3: Use the trace context

**If you already use opentelemetry-cpp SDK:**
```cpp
if (ctx.has_value() && ctx->sampled) {
    // Convert to OTel SpanContext and use as parent
    // (see opentelemetry-cpp docs for StartSpanOptions with parent context)
}
```

**If you export spans manually (like our example does):**
```cpp
if (ctx.has_value() && ctx->sampled) {
    // These go into your OTLP JSON payload:
    std::string trace_id = ctx->trace_id;         // "69fbb89b61b4b8813ed9af665d19b7cf"
    std::string parent_id = ctx->parent_span_id;  // "35522aaea7971245"
    std::string span_id = your_random_16_hex();   // generate a new one for YOUR span
}
```

**If the header is missing or malformed:**
```cpp
if (!ctx.has_value()) {
    // Not an API Gateway request (or header stripped by proxy)
    // Start a new trace as normal — nothing special needed
}
```

### That's it.

The propagator handles all the edge cases (malformed headers, wrong format, missing fields, extra fields, Sampled=0). You don't need to think about X-Ray trace ID format conversion — it outputs standard 32-char OTLP trace IDs.

## What the propagator does internally

```
Input header:  "Root=1-69fbb89b-61b4b8813ed9af665d19b7cf;Parent=35522aaea7971245;Sampled=1"
                     │         │                          │                       │
                     ▼         ▼                          ▼                       ▼
               version=1  timestamp=69fbb89b         span_id=35522aaea7971245  sampled=true
                          random=61b4b8813ed9af665d19b7cf

Output:
  trace_id:        "69fbb89b61b4b8813ed9af665d19b7cf"  (timestamp + random, no dashes)
  parent_span_id:  "35522aaea7971245"
  sampled:         true
```

## Running the unit tests

```bash
g++ -std=c++17 -O2 -o xray_propagator_test xray_propagator_test.cpp && ./xray_propagator_test
```

Expected output:
```
PASS: test_valid_header
PASS: test_field_order_independence
PASS: test_additional_fields_ignored
PASS: test_sampled_zero
PASS: test_missing_parent
PASS: test_malformed_root_too_short
PASS: test_malformed_root_wrong_version
PASS: test_malformed_root_non_hex
PASS: test_malformed_parent_wrong_length
PASS: test_empty_header
PASS: test_no_root_field
PASS: test_case_insensitive_header_lookup
PASS: test_whitespace_tolerance

All 13 tests passed.
```
