# Multi-stage Dockerfile for Orochi Cloud Dashboard
# Optimized for production deployment on Fly.io
# Uses Alpine Linux for minimal size and attack surface

# Stage 1: Build Stage
# Installs all dependencies and builds the React application
FROM node:18-alpine as builder

# Set working directory
WORKDIR /app

# Install additional build dependencies if needed
RUN apk add --no-cache python3 make g++ \
    && rm -rf /var/cache/apk/*

# Copy package files first (leverages Docker layer caching)
COPY package*.json ./

# Install dependencies
# Use npm ci for reproducible builds (instead of npm install)
RUN npm ci && npm cache clean --force

# Copy source code
COPY . .

# Build the application with Vite
# Output goes to dist/ directory
RUN npm run build

# Stage 2: Runtime Stage
# Minimal production image with only runtime dependencies
FROM node:18-alpine

# Install dumb-init for proper signal handling
# Critical for graceful shutdowns in containers
RUN apk add --no-cache dumb-init \
    && rm -rf /var/cache/apk/*

WORKDIR /app

# Copy built application from builder stage
COPY --from=builder /app/dist ./dist
COPY --from=builder /app/node_modules ./node_modules
COPY --from=builder /app/package*.json ./

# Create non-root user for enhanced security
# Running as root in containers is a security risk
RUN addgroup -g 1001 -S nodejs && \
    adduser -S nodejs -u 1001 && \
    chown -R nodejs:nodejs /app

# Switch to non-root user
USER nodejs

# Expose the port the application listens on
# Fly.io will forward traffic to this port
EXPOSE 3000

# Health check
# Enables Fly.io to detect unhealthy instances and restart them
# Grace period allows app startup time before first check
# Retries and interval control how aggressively we check
HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
    CMD node -e "require('http').get('http://localhost:3000', (r) => {if (r.statusCode !== 200) throw new Error(r.statusCode)})"

# Use dumb-init as entrypoint
# Ensures proper signal handling (SIGTERM, SIGINT)
# Required for graceful shutdowns
ENTRYPOINT ["dumb-init", "--"]

# Start the application
# Server is built by Vite and listens on port 3000
CMD ["node", "dist/server/server.js"]
