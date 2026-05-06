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

## Limitations

- **Latency**: Traces appear in Observe ~90 seconds after the original request (polling delay)
- **HTTP APIs (V2)**: AWS HTTP APIs do not support X-Ray tracing — only REST APIs (V1) work
- **Managed services only**: This poller is specifically for traces that AWS services send directly to X-Ray (API Gateway, DynamoDB, etc.). For your own code, use the OTel SDK directly — it's more efficient than going through X-Ray.
