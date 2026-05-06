# X-Ray to Observe OTLP Poller

Polls AWS X-Ray API for traces from managed services (API Gateway, etc.) and forwards them as OTLP spans to [Observe](https://www.observeinc.com/).

## Why?

AWS API Gateway (and other managed services) emit traces exclusively to X-Ray. There is no way to redirect these traces to an external collector. This Lambda function bridges the gap by polling the X-Ray API and converting traces to OTLP format for ingestion into Observe (or any OTLP-compatible backend).

This is the same pattern used by Datadog, New Relic, and other observability vendors for their AWS X-Ray integrations.

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│  AWS Account                                            │
│                                                         │
│  API Gateway (X-Ray enabled)                            │
│       │                                                 │
│       ▼                                                 │
│  X-Ray Service (stores segments internally)             │
│       │                                                 │
│       │  GetTraceSummaries / BatchGetTraces              │
│       ▼                                                 │
│  ┌──────────────────────────┐                           │
│  │  Lambda: X-Ray Poller    │──── on failure ───► SQS DLQ│
│  │  (EventBridge: 1/min)    │                           │
│  └───────────┬──────────────┘                           │
│              │                                          │
└──────────────┼──────────────────────────────────────────┘
               │ OTLP/JSON POST
               ▼
     Observe OTLP Endpoint
     (https://<customer>.collect.observeinc.com/v2/otel/v1/traces)
```

## Prerequisites

1. An AWS account with API Gateway (or other X-Ray-enabled services) generating traces
2. X-Ray tracing **enabled** on your API Gateway stage
3. An Observe tenant with an ingest token

### Enable X-Ray on API Gateway (if not already)

**REST API (V1):**
```bash
aws apigateway update-stage \
  --rest-api-id YOUR_API_ID \
  --stage-name prod \
  --patch-operations op=replace,path=/tracingEnabled,value=true
```

**Note:** HTTP APIs (V2) do not support X-Ray tracing. You must use REST APIs.

## Deployment

### Step 1: Deploy the CloudFormation stack

```bash
aws cloudformation create-stack \
  --stack-name xray-observe-poller \
  --template-body file://xray-poller.yaml \
  --capabilities CAPABILITY_NAMED_IAM \
  --parameters \
    ParameterKey=ObserveCustomerId,ParameterValue=YOUR_CUSTOMER_ID \
    ParameterKey=ObserveToken,ParameterValue=YOUR_DATASTREAM_TOKEN:YOUR_BEARER_TOKEN \
    ParameterKey=PollIntervalMinutes,ParameterValue=1
```

### Step 2: Deploy the Lambda code

```bash
zip xray_poller.zip lambda_function.py

aws lambda update-function-code \
  --function-name xray-observe-poller-function \
  --zip-file fileb://xray_poller.zip
```

### Step 3: Verify

Send a request through your API Gateway:
```bash
curl https://YOUR_API_ID.execute-api.REGION.amazonaws.com/prod/your-endpoint
```

Wait 90 seconds, then check Lambda logs:
```bash
aws logs tail /aws/lambda/xray-observe-poller-function --since 5m
```

You should see:
```
Found N traces
Forwarded M spans from N traces
```

Traces will appear in Observe in the Tracing dataset with `service.name = "aws-api-gateway"`.

## Configuration

| Environment Variable | Description |
|---------------------|-------------|
| `OBSERVE_CUSTOMER_ID` | Your Observe tenant ID |
| `OBSERVE_TOKEN` | Ingest token (`datastream_token:bearer_token`) |
| `AWS_REGION` | Auto-set by Lambda runtime |

## How it works

1. EventBridge triggers the Lambda every N minutes, passing the invocation timestamp
2. Lambda computes a 90-second lookback window from the invocation time
3. Calls `GetTraceSummaries` (with pagination) to find all traces in the window
4. Calls `BatchGetTraces` (5 at a time) to fetch full segment documents
5. Converts X-Ray segments to OTLP spans (trace ID format, attributes, timing)
6. POSTs the OTLP JSON payload to Observe with retry (3 attempts, exponential backoff)
7. On total failure, Lambda fails and event goes to SQS Dead Letter Queue

### X-Ray to OTLP Trace ID conversion

X-Ray format: `1-{timestamp_hex}-{random_hex}` (e.g., `1-69fba214-225558552b59105d29f40f79`)
OTLP format: `{timestamp_hex}{random_hex}` (e.g., `69fba214225558552b59105d29f40f79`)

This ensures trace continuity — if your backend services use the `AwsXRayPropagator` with OTel, their spans will have the same trace ID and stitch together in Observe.

## Connecting backend traces

For full end-to-end trace stitching (API Gateway span → your backend spans), configure your backend OTel SDK to use the AWS X-Ray propagator:

**Python:**
```python
from opentelemetry.propagators.aws import AwsXRayPropagator
from opentelemetry.propagators.composite import CompositePropagator
from opentelemetry.trace.propagation.tracecontext import TraceContextTextMapPropagator
from opentelemetry import propagate

propagate.set_global_textmap(CompositePropagator([
    TraceContextTextMapPropagator(),
    AwsXRayPropagator(),
]))
```

Install: `pip install opentelemetry-propagator-aws-xray opentelemetry-sdk opentelemetry-exporter-otlp`

**Java (OTel Java Agent — zero-code instrumentation):**

No code changes required. Attach the OTel Java Agent and configure via environment variables or system properties:

```bash
# Download the agent
curl -sL https://github.com/open-telemetry/opentelemetry-java-instrumentation/releases/download/v2.10.0/opentelemetry-javaagent.jar -o opentelemetry-javaagent.jar

# Run your app with the agent
java -javaagent:opentelemetry-javaagent.jar \
    -Dotel.service.name=my-backend \
    -Dotel.exporter.otlp.endpoint=http://localhost:4318 \
    -Dotel.exporter.otlp.protocol=http/protobuf \
    -Dotel.propagators=xray,tracecontext \
    -jar my-app.jar
```

The key setting is `-Dotel.propagators=xray,tracecontext` — this enables the X-Ray propagator which extracts `X-Amzn-Trace-Id` headers from incoming requests.

Alternatively, set via environment variables:
```bash
export OTEL_SERVICE_NAME=my-backend
export OTEL_EXPORTER_OTLP_ENDPOINT=http://localhost:4318
export OTEL_EXPORTER_OTLP_PROTOCOL=http/protobuf
export OTEL_PROPAGATORS=xray,tracecontext
java -javaagent:opentelemetry-javaagent.jar -jar my-app.jar
```

**C++ (custom propagator — no official X-Ray propagator exists):**

A hardened custom propagator is provided in [`examples/cpp/xray_propagator.h`](examples/cpp/xray_propagator.h). It parses the `X-Amzn-Trace-Id` header with:
- Field-order independence (Root, Parent, Sampled can appear in any order)
- Format validation (rejects malformed trace/span IDs)
- Sampled=0 handling (skips span creation for unsampled requests)
- Graceful fallback (bad/missing header → new trace, never crashes)

Usage:
```cpp
#include "xray_propagator.h"

// Extract from HTTP request headers (case-insensitive lookup)
std::unordered_map<std::string, std::string> headers = get_request_headers();
auto ctx = AwsXRayPropagator::extract(headers);

if (ctx.has_value() && ctx->sampled) {
    // Create span with extracted trace context
    std::string trace_id = ctx->trace_id;         // 32 hex chars (OTLP format)
    std::string parent_id = ctx->parent_span_id;  // 16 hex chars
    // ... create OTel span with this parent context
} else if (!ctx.has_value()) {
    // No X-Ray header — start a new trace
}
```

Build requirements: `g++ -std=c++17`, `libcurl` for OTLP HTTP export. See [`examples/cpp/build.sh`](examples/cpp/build.sh) for the full build script. Unit tests: `examples/cpp/xray_propagator_test.cpp` (13 test cases).

This allows your backend to extract the `X-Amzn-Trace-Id` header that API Gateway passes through, using the same trace ID for its own spans.

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| "No traces found in window" | X-Ray tracing not enabled on API Gateway | Enable with `tracingEnabled: true` on the stage |
| HTTP 401 from Observe | Bad token | Verify `OBSERVE_TOKEN` format: `datastreamToken:bearerToken` |
| HTTP 415 from Observe | Wrong content type | Should not happen — the code sets `application/json` |
| Traces in Observe but not stitched | Backend not using X-Ray propagator | Add `AwsXRayPropagator` to your backend OTel config |
| Lambda timing out | Too many traces in window | Increase Lambda timeout or reduce poll interval |
| Messages in DLQ | Observe endpoint unreachable or returning errors | Check DLQ messages for error details |

## Files

| File | Purpose |
|------|---------|
| `lambda_function.py` | The Lambda function code |
| `xray-poller.yaml` | Production CloudFormation (poller only) |
| `xray-poller-test.yaml` | Test CloudFormation (includes demo API Gateway) |
| `examples/java/` | Spring Boot backend with OTel Java Agent |
| `examples/cpp/` | C++ backend with hardened custom X-Ray propagator |

## Limitations

- **Latency**: Traces appear in Observe ~90 seconds after the original request (polling delay)
- **HTTP APIs (V2)**: AWS HTTP APIs do not support X-Ray tracing — only REST APIs (V1) work
- **Managed services only**: This poller is specifically for traces that AWS services send directly to X-Ray (API Gateway, DynamoDB, etc.). For your own code, use the OTel SDK directly — it's more efficient than going through X-Ray.
- **Missing CLIENT span in waterfall**: The API Gateway subsegment (outgoing HTTP call) may appear as a "missing span" in Observe's trace waterfall. This is cosmetic — Observe's trace assembly drops CLIENT spans that arrive after the initial trace batch. The traces still stitch correctly by `trace_id`, and the root API Gateway span appears properly. This does not affect alerting, latency measurement, or debugging.
