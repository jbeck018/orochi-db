# PgDog Router - Production Dockerfile
# Multi-stage build for Orochi DB connection pooler
#
# Build: docker build --platform linux/amd64 -t pgdog-router .
# Run:   docker run -p 5432:5432 -p 6432:6432 -p 9090:9090 pgdog-router

# ==============================================================================
# Stage 1: Build PgDog from source
# ==============================================================================
FROM rust:1.75-bookworm AS builder

# Build arguments
ARG PGDOG_VERSION=latest
ARG PGDOG_REPO=https://github.com/pgdogdev/pgdog.git

# Install build dependencies
RUN apt-get update && apt-get install -y \
    libssl-dev \
    pkg-config \
    cmake \
    libclang-dev \
    git \
    && rm -rf /var/lib/apt/lists/*

# Clone PgDog repository
WORKDIR /build
RUN if [ "${PGDOG_VERSION}" = "latest" ]; then \
        git clone --depth 1 ${PGDOG_REPO}; \
    else \
        git clone --depth 1 --branch ${PGDOG_VERSION} ${PGDOG_REPO}; \
    fi

# Build PgDog with release optimizations
WORKDIR /build/pgdog
RUN cargo build --release

# Verify the binary was built
RUN test -f /build/pgdog/target/release/pgdog && \
    echo "PgDog binary built successfully"

# ==============================================================================
# Stage 2: Runtime image
# ==============================================================================
FROM debian:bookworm-slim

# Labels for container metadata
LABEL org.opencontainers.image.title="PgDog Router"
LABEL org.opencontainers.image.description="PostgreSQL connection pooler and query router for Orochi DB"
LABEL org.opencontainers.image.vendor="Orochi DB"
LABEL org.opencontainers.image.source="https://github.com/orochi-db/orochi-db"

# Install runtime dependencies
RUN apt-get update && apt-get install -y \
    libssl3 \
    ca-certificates \
    curl \
    netcat-openbsd \
    postgresql-client \
    && rm -rf /var/lib/apt/lists/* \
    && apt-get clean

# Create non-root user for security
RUN groupadd -r pgdog && useradd -r -g pgdog pgdog

# Copy PgDog binary from builder stage
COPY --from=builder /build/pgdog/target/release/pgdog /usr/local/bin/pgdog

# Ensure binary is executable
RUN chmod +x /usr/local/bin/pgdog

# Create required directories
RUN mkdir -p /etc/pgdog/certs \
    && mkdir -p /var/log/pgdog \
    && mkdir -p /var/run/pgdog \
    && chown -R pgdog:pgdog /etc/pgdog /var/log/pgdog /var/run/pgdog

# Copy configuration templates
COPY config/pgdog.toml.tmpl /etc/pgdog/pgdog.toml.tmpl
COPY config/users.toml.tmpl /etc/pgdog/users.toml.tmpl

# Copy startup and health check scripts
COPY scripts/entrypoint.sh /entrypoint.sh
COPY scripts/health-check.sh /health-check.sh

# Make scripts executable
RUN chmod +x /entrypoint.sh /health-check.sh

# Set ownership of configuration files
RUN chown -R pgdog:pgdog /etc/pgdog

# Environment variables with defaults
ENV PGDOG_CONFIG=/etc/pgdog/pgdog.toml
ENV PGDOG_USERS_CONFIG=/etc/pgdog/users.toml
ENV PGDOG_LOG_LEVEL=info
ENV PGDOG_HOST=0.0.0.0
ENV PGDOG_PORT=5432
ENV PGDOG_ADMIN_PORT=6432
ENV PGDOG_METRICS_PORT=9090

# Expose ports
# 5432 - PostgreSQL protocol (client connections)
# 6432 - Admin interface
# 9090 - Prometheus metrics
EXPOSE 5432 6432 9090

# Health check configuration
# Checks if PgDog is accepting connections on the main port
HEALTHCHECK --interval=30s --timeout=10s --start-period=15s --retries=3 \
    CMD /health-check.sh || exit 1

# Run as non-root user
USER pgdog

# Set working directory
WORKDIR /etc/pgdog

# Entrypoint and default command
ENTRYPOINT ["/entrypoint.sh"]
CMD ["pgdog"]
