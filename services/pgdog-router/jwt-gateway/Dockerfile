# Build stage
FROM golang:1.22-alpine AS builder

# Install build dependencies
RUN apk add --no-cache git ca-certificates

WORKDIR /app

# Copy go mod files
COPY go.mod go.sum ./

# Download dependencies
RUN go mod download

# Copy source code
COPY . .

# Build the binary
RUN CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build \
    -ldflags="-w -s -X main.version=1.0.0 -X main.buildTime=$(date -u +%Y-%m-%dT%H:%M:%SZ) -X main.gitCommit=$(git rev-parse --short HEAD 2>/dev/null || echo 'unknown')" \
    -o /jwt-proxy \
    ./cmd/jwt-proxy

# Runtime stage
FROM alpine:3.19

# Install runtime dependencies
RUN apk add --no-cache ca-certificates tzdata

# Create non-root user
RUN addgroup -g 1000 -S jwtproxy && \
    adduser -u 1000 -S jwtproxy -G jwtproxy

WORKDIR /app

# Copy binary from builder
COPY --from=builder /jwt-proxy /app/jwt-proxy

# Copy example config for reference
COPY config.yaml.example /app/config.yaml.example

# Set ownership
RUN chown -R jwtproxy:jwtproxy /app

# Switch to non-root user
USER jwtproxy

# Expose the proxy port
EXPOSE 5433

# Health check
HEALTHCHECK --interval=30s --timeout=5s --start-period=5s --retries=3 \
    CMD nc -z localhost 5433 || exit 1

# Default command
ENTRYPOINT ["/app/jwt-proxy"]
CMD ["-config", "/app/config.yaml"]
