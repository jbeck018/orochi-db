# Multi-stage Dockerfile for HowlerOps Dashboard
# Optimized for production deployment on Fly.io
# Uses Bun runtime for improved performance

# Stage 1: Build Stage
# Installs all dependencies and builds the React application
FROM oven/bun:1.2-debian AS builder

# Build argument for API URL (passed from fly.toml)
ARG VITE_API_URL
ENV VITE_API_URL=${VITE_API_URL}

# Set working directory
WORKDIR /app

# Copy package files first (leverages Docker layer caching)
COPY package.json bun.lock* ./

# Install dependencies with Bun
RUN bun install --frozen-lockfile || bun install

# Copy source code
COPY . .

# Build the application with Vite using Bun
# VITE_API_URL is available at build time for client-side code
RUN bun --bun run build

# Stage 2: Runtime Stage
# Minimal production image with Bun runtime
FROM oven/bun:1.2-debian

WORKDIR /app

# Copy built application from builder stage
COPY --from=builder /app/dist ./dist
COPY --from=builder /app/server ./server
COPY --from=builder /app/node_modules ./node_modules
COPY --from=builder /app/package.json ./

# The bun user already exists in oven/bun image
# Just set ownership
RUN chown -R bun:bun /app

# Switch to non-root user
USER bun

# Set NODE_ENV for production optimizations
ENV NODE_ENV=production

# Expose the port the application listens on
EXPOSE 3000

# Start the application with Bun
CMD ["bun", "run", "server/index.ts"]
