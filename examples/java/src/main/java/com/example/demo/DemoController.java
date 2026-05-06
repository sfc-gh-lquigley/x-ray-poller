package com.example.demo;

import io.opentelemetry.api.trace.Span;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.RestController;

import java.time.Instant;
import java.util.Map;

@RestController
public class DemoController {

    @GetMapping("/demo")
    public Map<String, Object> demo() {
        Span span = Span.current();
        String traceId = span.getSpanContext().getTraceId();

        return Map.of(
            "service", "demo-backend-java",
            "status", "ok",
            "timestamp", Instant.now().toEpochMilli() / 1000.0,
            "trace_id", traceId
        );
    }

    @GetMapping("/health")
    public Map<String, String> health() {
        return Map.of("status", "healthy");
    }
}
