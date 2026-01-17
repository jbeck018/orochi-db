# Build stage
FROM golang:1.22-alpine AS builder

# Install build dependencies
RUN apk add --no-cache git ca-certificates

# Set working directory
WORKDIR /app

# Copy go mod files
COPY go.mod go.sum ./

# Download dependencies
RUN go mod download

# Copy source code
COPY . .

# Build the binary
RUN CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build \
    -ldflags="-w -s -X main.version=$(git describe --tags --always --dirty 2>/dev/null || echo 'dev')" \
    -o /autoscaler \
    ./cmd/autoscaler

# Runtime stage
FROM alpine:3.19

# Install runtime dependencies
RUN apk add --no-cache ca-certificates tzdata

# Create non-root user
RUN addgroup -g 1000 autoscaler && \
    adduser -u 1000 -G autoscaler -s /bin/sh -D autoscaler

# Copy binary from builder
COPY --from=builder /autoscaler /usr/local/bin/autoscaler

# Set ownership
RUN chown autoscaler:autoscaler /usr/local/bin/autoscaler

# Switch to non-root user
USER autoscaler

# Expose ports
# gRPC
EXPOSE 50051
# Prometheus metrics
EXPOSE 9090
# Health checks
EXPOSE 8080

# Set entrypoint
ENTRYPOINT ["/usr/local/bin/autoscaler"]

# Default command (can be overridden)
CMD ["--config", "/etc/autoscaler/config.yaml"]
