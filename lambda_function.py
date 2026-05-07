import json
import time
import urllib.request
import boto3
import logging
from datetime import datetime, timedelta, timezone

logger = logging.getLogger()
logger.setLevel(logging.INFO)

OBSERVE_ENDPOINT_TEMPLATE = "https://{customer_id}.collect.observeinc.com/v2/otel/v1/traces"
MAX_RETRIES = 3
BACKOFF_BASE = 2
LOOKBACK_SECONDS = 90
BATCH_GET_LIMIT = 5


def get_config():
    import os
    return {
        "observe_customer_id": os.environ["OBSERVE_CUSTOMER_ID"],
        "observe_token": os.environ["OBSERVE_TOKEN"],
        "aws_region": os.environ.get("AWS_REGION", "us-west-2"),
    }


def xray_trace_id_to_otel(xray_id):
    parts = xray_id.split("-")
    return parts[1] + parts[2]


def epoch_to_nanos(ts):
    return str(int(ts * 1_000_000_000))


def build_otlp_span(segment, trace_id_hex, is_subsegment=False, root_end_time=None):
    if is_subsegment and segment.get("namespace") == "remote":
        kind = 3
    elif is_subsegment:
        kind = 4
    else:
        kind = 2

    start_ns = epoch_to_nanos(segment["start_time"])
    end_ns = epoch_to_nanos(segment.get("end_time", segment["start_time"]))

    # For zero-duration spans, use the root segment's end_time if available
    # (API Gateway doesn't accurately record subsegment end times)
    if end_ns == start_ns:
        if root_end_time is not None:
            end_ns = epoch_to_nanos(root_end_time)
        else:
            end_ns = str(int(start_ns) + 1_000_000)  # floor to 1ms as last resort

    span = {
        "traceId": trace_id_hex,
        "spanId": segment["id"],
        "name": segment.get("name", "unknown"),
        "kind": kind,
        "startTimeUnixNano": start_ns,
        "endTimeUnixNano": end_ns,
        "attributes": [],
        "status": {},
    }

    if "parent_id" in segment:
        span["parentSpanId"] = segment["parent_id"]

    if segment.get("fault"):
        span["status"] = {"code": 2, "message": "fault"}
    elif segment.get("error"):
        span["status"] = {"code": 2, "message": "error"}
    else:
        span["status"] = {"code": 1}

    http = segment.get("http", {})
    req = http.get("request", {})
    resp = http.get("response", {})

    if req.get("url"):
        span["attributes"].append({"key": "http.url", "value": {"stringValue": req["url"]}})
    if req.get("method"):
        span["attributes"].append({"key": "http.method", "value": {"stringValue": req["method"]}})
    if resp.get("status"):
        span["attributes"].append({"key": "http.status_code", "value": {"intValue": str(resp["status"])}})
    if req.get("client_ip"):
        span["attributes"].append({"key": "net.peer.ip", "value": {"stringValue": req["client_ip"]}})
    if req.get("user_agent"):
        span["attributes"].append({"key": "http.user_agent", "value": {"stringValue": req["user_agent"]}})

    aws = segment.get("aws", {})
    if "account_id" in aws:
        span["attributes"].append({"key": "cloud.account.id", "value": {"stringValue": aws["account_id"]}})
    if "api_gateway" in aws:
        gw = aws["api_gateway"]
        if gw.get("api_id"):
            span["attributes"].append({"key": "aws.api_gateway.api_id", "value": {"stringValue": gw["api_id"]}})
        if gw.get("stage"):
            span["attributes"].append({"key": "aws.api_gateway.stage", "value": {"stringValue": gw["stage"]}})
        if gw.get("request_id"):
            span["attributes"].append({"key": "aws.api_gateway.request_id", "value": {"stringValue": gw["request_id"]}})

    if segment.get("origin"):
        span["attributes"].append({"key": "aws.xray.origin", "value": {"stringValue": segment["origin"]}})

    span["attributes"].append({"key": "cloud.provider", "value": {"stringValue": "aws"}})

    return span


def process_subsegments(subsegments, trace_id_hex, parent_span_id, root_end_time=None):
    spans = []
    for sub in subsegments:
        sub_span = build_otlp_span(sub, trace_id_hex, is_subsegment=True, root_end_time=root_end_time)
        sub_span["parentSpanId"] = parent_span_id
        spans.append(sub_span)
        if "subsegments" in sub:
            spans.extend(process_subsegments(sub["subsegments"], trace_id_hex, sub["id"], root_end_time=root_end_time))
    return spans


def send_to_observe(resource_spans, config):
    endpoint = OBSERVE_ENDPOINT_TEMPLATE.format(customer_id=config["observe_customer_id"])
    payload = json.dumps({"resourceSpans": resource_spans}).encode("utf-8")

    for attempt in range(1, MAX_RETRIES + 1):
        try:
            req = urllib.request.Request(
                endpoint,
                data=payload,
                headers={
                    "Content-Type": "application/json",
                    "Authorization": f"Bearer {config['observe_token']}",
                    "x-observe-target-package": "Tracing",
                },
                method="POST",
            )
            resp = urllib.request.urlopen(req, timeout=15)
            logger.info(f"Observe response: {resp.status} (attempt {attempt})")
            return True
        except urllib.error.HTTPError as e:
            body = e.read().decode("utf-8", errors="replace")
            logger.warning(f"Attempt {attempt}/{MAX_RETRIES} failed: HTTP {e.code} - {body}")
            if e.code in (429, 500, 502, 503, 504) and attempt < MAX_RETRIES:
                time.sleep(BACKOFF_BASE ** attempt)
                continue
            raise
        except Exception as e:
            logger.warning(f"Attempt {attempt}/{MAX_RETRIES} failed: {e}")
            if attempt < MAX_RETRIES:
                time.sleep(BACKOFF_BASE ** attempt)
                continue
            raise

    return False


def get_time_window(event):
    if "time" in event:
        end_time = datetime.fromisoformat(event["time"].replace("Z", "+00:00"))
    else:
        end_time = datetime.now(timezone.utc)

    start_time = end_time - timedelta(seconds=LOOKBACK_SECONDS)
    return start_time, end_time


def handler(event, context):
    config = get_config()
    xray = boto3.client("xray", region_name=config["aws_region"])

    start_time, end_time = get_time_window(event)
    logger.info(f"Polling X-Ray traces from {start_time.isoformat()} to {end_time.isoformat()}")

    trace_ids = []
    next_token = None

    while True:
        kwargs = {
            "StartTime": start_time,
            "EndTime": end_time,
            "Sampling": False,
        }
        if next_token:
            kwargs["NextToken"] = next_token

        resp = xray.get_trace_summaries(**kwargs)
        batch_ids = [s["Id"] for s in resp.get("TraceSummaries", [])]
        trace_ids.extend(batch_ids)

        next_token = resp.get("NextToken")
        if not next_token:
            break

    if not trace_ids:
        logger.info("No traces found in window")
        return {"statusCode": 200, "body": "No traces"}

    logger.info(f"Found {len(trace_ids)} traces")

    all_spans = []

    for i in range(0, len(trace_ids), BATCH_GET_LIMIT):
        batch = trace_ids[i:i + BATCH_GET_LIMIT]
        traces_resp = xray.batch_get_traces(TraceIds=batch)

        for trace_data in traces_resp.get("Traces", []):
            trace_id_hex = xray_trace_id_to_otel(trace_data["Id"])

            for seg_doc in trace_data.get("Segments", []):
                segment = json.loads(seg_doc["Document"])

                # Skip X-Ray inferred segments — these are synthetic representations
                # of OTel-instrumented backends; the real span comes from the OTel collector
                if segment.get("inferred"):
                    continue

                root_end_time = segment.get("end_time")
                span = build_otlp_span(segment, trace_id_hex)
                all_spans.append(span)

                if "subsegments" in segment:
                    all_spans.extend(
                        process_subsegments(segment["subsegments"], trace_id_hex, segment["id"], root_end_time=root_end_time)
                    )

    if not all_spans:
        logger.info("No spans extracted from traces")
        return {"statusCode": 200, "body": "No spans"}

    resource_spans = [{
        "resource": {
            "attributes": [
                {"key": "service.name", "value": {"stringValue": "aws-api-gateway"}},
                {"key": "service.namespace", "value": {"stringValue": "x-ray-poller"}},
                {"key": "cloud.provider", "value": {"stringValue": "aws"}},
                {"key": "cloud.region", "value": {"stringValue": config["aws_region"]}},
            ]
        },
        "scopeSpans": [{
            "scope": {"name": "xray-to-otlp-poller", "version": "1.0.0"},
            "spans": all_spans,
        }]
    }]

    send_to_observe(resource_spans, config)
    logger.info(f"Forwarded {len(all_spans)} spans from {len(trace_ids)} traces")

    return {"statusCode": 200, "body": f"Processed {len(all_spans)} spans from {len(trace_ids)} traces"}
