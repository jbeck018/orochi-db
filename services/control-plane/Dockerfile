# Multi-stage Dockerfile for Orochi Control Plane
# Optimized for production deployment

# Stage 1: Build
FROM golang:1.22-alpine AS builder

# Install build dependencies
RUN apk add --no-cache git ca-certificates

WORKDIR /app

# Copy go mod files first for better caching
COPY go.mod go.sum ./
RUN go mod download

# Copy source code
COPY . .

# Build the binary
RUN CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build \
    -ldflags="-w -s" \
    -o /app/server \
    ./cmd/server

# Stage 2: Runtime
FROM alpine:3.20

# Install runtime dependencies
RUN apk add --no-cache ca-certificates tzdata

# Create non-root user
RUN addgroup -g 1001 -S orochi && \
    adduser -S orochi -u 1001 -G orochi

WORKDIR /app

# Copy binary from builder
COPY --from=builder /app/server /app/server

# Set ownership
RUN chown -R orochi:orochi /app

# Switch to non-root user
USER orochi

# Expose ports
# HTTP API
EXPOSE 8080
# gRPC
EXPOSE 9090

# Health check
HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
    CMD wget --no-verbose --tries=1 --spider http://localhost:8080/health || exit 1

# Run the server
ENTRYPOINT ["/app/server"]
