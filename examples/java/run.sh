#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
AGENT_VERSION="2.10.0"
AGENT_JAR="$SCRIPT_DIR/opentelemetry-javaagent.jar"

if [ ! -f "$AGENT_JAR" ]; then
    echo "Downloading OTel Java Agent v${AGENT_VERSION}..."
    curl -sL "https://github.com/open-telemetry/opentelemetry-java-instrumentation/releases/download/v${AGENT_VERSION}/opentelemetry-javaagent.jar" \
        -o "$AGENT_JAR"
fi

if [ ! -f "$SCRIPT_DIR/target/demo-backend-java-1.0.0.jar" ]; then
    echo "Building Spring Boot app..."
    cd "$SCRIPT_DIR"
    mvn -q package -DskipTests
fi

exec java \
    -javaagent:"$AGENT_JAR" \
    -Dotel.service.name=demo-backend-java \
    -Dotel.exporter.otlp.endpoint=http://localhost:4318 \
    -Dotel.exporter.otlp.protocol=http/protobuf \
    -Dotel.propagators=xray,tracecontext \
    -Dotel.resource.attributes=service.namespace=webinar-demo,deployment.environment=production \
    -jar "$SCRIPT_DIR/target/demo-backend-java-1.0.0.jar"
