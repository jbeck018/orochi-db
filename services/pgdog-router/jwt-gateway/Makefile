.PHONY: build test clean docker lint fmt

# Build variables
VERSION ?= 1.0.0
BUILD_TIME := $(shell date -u +%Y-%m-%dT%H:%M:%SZ)
GIT_COMMIT := $(shell git rev-parse --short HEAD 2>/dev/null || echo "unknown")
LDFLAGS := -ldflags "-w -s -X main.version=$(VERSION) -X main.buildTime=$(BUILD_TIME) -X main.gitCommit=$(GIT_COMMIT)"

# Output binary
BINARY := jwt-proxy

# Default target
all: build

# Build the binary
build:
	GOWORK=off go build $(LDFLAGS) -o $(BINARY) ./cmd/jwt-proxy

# Build for Linux (for containers)
build-linux:
	GOWORK=off GOOS=linux GOARCH=amd64 CGO_ENABLED=0 go build $(LDFLAGS) -o $(BINARY)-linux ./cmd/jwt-proxy

# Run tests
test:
	GOWORK=off go test -v ./...

# Run tests with coverage
test-coverage:
	GOWORK=off go test -v -coverprofile=coverage.out ./...
	go tool cover -html=coverage.out -o coverage.html

# Format code
fmt:
	go fmt ./...

# Lint code
lint:
	golangci-lint run ./...

# Clean build artifacts
clean:
	rm -f $(BINARY) $(BINARY)-linux coverage.out coverage.html

# Build Docker image
docker:
	docker build -t pgdog-jwt-gateway:$(VERSION) .
	docker tag pgdog-jwt-gateway:$(VERSION) pgdog-jwt-gateway:latest

# Run locally
run: build
	./$(BINARY) -config config.yaml

# Run with debug logging
run-debug: build
	JWT_GATEWAY_LOG_LEVEL=debug ./$(BINARY)

# Generate test RSA keys (for development only)
generate-keys:
	mkdir -p keys
	openssl genrsa -out keys/private.pem 2048
	openssl rsa -in keys/private.pem -pubout -out keys/public.pem
	@echo "Keys generated in ./keys/"

# Install dependencies
deps:
	GOWORK=off go mod download
	GOWORK=off go mod tidy

# Help
help:
	@echo "PgDog JWT Gateway Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  build         Build the binary"
	@echo "  build-linux   Build for Linux/amd64"
	@echo "  test          Run tests"
	@echo "  test-coverage Run tests with coverage report"
	@echo "  fmt           Format code"
	@echo "  lint          Run linter"
	@echo "  clean         Clean build artifacts"
	@echo "  docker        Build Docker image"
	@echo "  run           Build and run locally"
	@echo "  run-debug     Run with debug logging"
	@echo "  generate-keys Generate test RSA keys"
	@echo "  deps          Download dependencies"
	@echo "  help          Show this help"
