# Orochi Cloud Autoscaler Makefile

# Variables
BINARY_NAME=autoscaler
VERSION?=$(shell git describe --tags --always --dirty 2>/dev/null || echo "dev")
BUILD_TIME=$(shell date -u '+%Y-%m-%d_%H:%M:%S')
LDFLAGS=-ldflags "-w -s -X main.version=$(VERSION) -X main.buildTime=$(BUILD_TIME)"

# Go parameters
GOCMD=go
GOBUILD=$(GOCMD) build
GOCLEAN=$(GOCMD) clean
GOTEST=$(GOCMD) test
GOGET=$(GOCMD) get
GOMOD=$(GOCMD) mod
GOFMT=gofmt
GOLINT=golangci-lint

# Docker parameters
DOCKER_REGISTRY?=ghcr.io/orochi-db
DOCKER_IMAGE=$(DOCKER_REGISTRY)/autoscaler
DOCKER_TAG?=$(VERSION)

# Proto parameters
PROTOC=protoc
PROTO_DIR=api/proto
PROTO_OUT=api/proto

.PHONY: all build clean test coverage lint fmt proto docker docker-push help

all: lint test build

## Build commands
build: ## Build the binary
	$(GOBUILD) $(LDFLAGS) -o bin/$(BINARY_NAME) ./cmd/autoscaler

build-linux: ## Build for Linux
	CGO_ENABLED=0 GOOS=linux GOARCH=amd64 $(GOBUILD) $(LDFLAGS) -o bin/$(BINARY_NAME)-linux-amd64 ./cmd/autoscaler

build-darwin: ## Build for macOS
	CGO_ENABLED=0 GOOS=darwin GOARCH=amd64 $(GOBUILD) $(LDFLAGS) -o bin/$(BINARY_NAME)-darwin-amd64 ./cmd/autoscaler
	CGO_ENABLED=0 GOOS=darwin GOARCH=arm64 $(GOBUILD) $(LDFLAGS) -o bin/$(BINARY_NAME)-darwin-arm64 ./cmd/autoscaler

build-all: build-linux build-darwin ## Build for all platforms

clean: ## Clean build artifacts
	$(GOCLEAN)
	rm -rf bin/
	rm -rf coverage/

## Test commands
test: ## Run tests
	$(GOTEST) -v -race ./...

test-short: ## Run short tests
	$(GOTEST) -v -short ./...

coverage: ## Run tests with coverage
	mkdir -p coverage
	$(GOTEST) -v -race -coverprofile=coverage/coverage.out -covermode=atomic ./...
	$(GOCMD) tool cover -html=coverage/coverage.out -o coverage/coverage.html

benchmark: ## Run benchmarks
	$(GOTEST) -bench=. -benchmem ./...

## Code quality commands
lint: ## Run linter
	$(GOLINT) run ./...

fmt: ## Format code
	$(GOFMT) -s -w .

fmt-check: ## Check code formatting
	@test -z "$$($(GOFMT) -l .)" || (echo "Code is not formatted. Run 'make fmt'" && exit 1)

vet: ## Run go vet
	$(GOCMD) vet ./...

## Dependency commands
deps: ## Download dependencies
	$(GOMOD) download

deps-update: ## Update dependencies
	$(GOMOD) tidy
	$(GOGET) -u ./...

deps-verify: ## Verify dependencies
	$(GOMOD) verify

## Proto commands
proto: ## Generate protobuf code
	$(PROTOC) --go_out=$(PROTO_OUT) --go_opt=paths=source_relative \
		--go-grpc_out=$(PROTO_OUT) --go-grpc_opt=paths=source_relative \
		$(PROTO_DIR)/autoscaler.proto

proto-lint: ## Lint proto files
	buf lint $(PROTO_DIR)

## Docker commands
docker: ## Build Docker image
	docker build -t $(DOCKER_IMAGE):$(DOCKER_TAG) .
	docker tag $(DOCKER_IMAGE):$(DOCKER_TAG) $(DOCKER_IMAGE):latest

docker-push: ## Push Docker image
	docker push $(DOCKER_IMAGE):$(DOCKER_TAG)
	docker push $(DOCKER_IMAGE):latest

docker-run: ## Run Docker container locally
	docker run --rm -it \
		-p 50051:50051 \
		-p 9090:9090 \
		-p 8080:8080 \
		-e KUBERNETES_IN_CLUSTER=false \
		-e PROMETHEUS_ADDRESS=http://host.docker.internal:9090 \
		-v ~/.kube:/home/autoscaler/.kube:ro \
		$(DOCKER_IMAGE):$(DOCKER_TAG)

## Run commands
run: build ## Build and run locally
	./bin/$(BINARY_NAME)

run-dev: ## Run with development settings
	KUBERNETES_IN_CLUSTER=false \
	LOG_LEVEL=debug \
	$(GOCMD) run ./cmd/autoscaler

## Help command
help: ## Display this help
	@echo "Orochi Cloud Autoscaler"
	@echo ""
	@echo "Usage: make [target]"
	@echo ""
	@echo "Targets:"
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | sort | awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-15s\033[0m %s\n", $$1, $$2}'
