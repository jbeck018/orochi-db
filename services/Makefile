# Orochi Cloud - Monorepo Makefile

.PHONY: all build test clean dev install lint fmt help

# Default target
all: build

# Build all components
build: build-control-plane build-autoscaler build-provisioner build-cli build-dashboard
	@echo "All components built successfully"

build-control-plane:
	@echo "Building Control Plane API..."
	cd control-plane && go build -o bin/control-plane ./cmd/server

build-autoscaler:
	@echo "Building Autoscaler..."
	cd autoscaler && go build -o bin/autoscaler ./cmd/autoscaler

build-provisioner:
	@echo "Building Provisioner..."
	cd provisioner && go build -o bin/provisioner ./cmd/provisioner

build-cli:
	@echo "Building CLI..."
	cd cli && go build -o bin/orochi ./cmd/orochi

build-dashboard:
	@echo "Building Dashboard..."
	cd dashboard && npm run build

# Run tests
test: test-control-plane test-autoscaler test-provisioner test-cli test-dashboard
	@echo "All tests passed"

test-control-plane:
	cd control-plane && go test -v ./...

test-autoscaler:
	cd autoscaler && go test -v ./...

test-provisioner:
	cd provisioner && go test -v ./...

test-cli:
	cd cli && go test -v ./...

test-dashboard:
	cd dashboard && npm test

# Development mode
dev:
	@echo "Starting development environment..."
	$(MAKE) -j4 dev-control-plane dev-autoscaler dev-provisioner dev-dashboard

dev-control-plane:
	cd control-plane && go run ./cmd/server

dev-autoscaler:
	cd autoscaler && go run ./cmd/autoscaler

dev-provisioner:
	cd provisioner && go run ./cmd/provisioner

dev-dashboard:
	cd dashboard && npm run dev

# Install dependencies
install: install-go-deps install-node-deps
	@echo "All dependencies installed"

install-go-deps:
	cd control-plane && go mod download
	cd autoscaler && go mod download
	cd provisioner && go mod download
	cd cli && go mod download

install-node-deps:
	cd dashboard && npm install

# Linting
lint: lint-go lint-node
	@echo "Linting complete"

lint-go:
	cd control-plane && golangci-lint run
	cd autoscaler && golangci-lint run
	cd provisioner && golangci-lint run
	cd cli && golangci-lint run

lint-node:
	cd dashboard && npm run lint

# Format code
fmt:
	cd control-plane && go fmt ./...
	cd autoscaler && go fmt ./...
	cd provisioner && go fmt ./...
	cd cli && go fmt ./...
	cd dashboard && npm run format

# Clean build artifacts
clean:
	rm -rf control-plane/bin
	rm -rf autoscaler/bin
	rm -rf provisioner/bin
	rm -rf cli/bin
	rm -rf dashboard/.next
	rm -rf dashboard/node_modules

# Docker builds
docker-build:
	docker build -t orochi-cloud/control-plane:latest -f control-plane/Dockerfile control-plane
	docker build -t orochi-cloud/autoscaler:latest -f autoscaler/Dockerfile autoscaler
	docker build -t orochi-cloud/provisioner:latest -f provisioner/Dockerfile provisioner
	docker build -t orochi-cloud/dashboard:latest -f dashboard/Dockerfile dashboard

# Helm deployment
deploy:
	helm upgrade --install orochi-cloud ./infrastructure/helm/orochi-cloud

deploy-dev:
	helm upgrade --install orochi-cloud ./infrastructure/helm/orochi-cloud -f ./infrastructure/helm/orochi-cloud/values-dev.yaml

# Generate protobuf
proto:
	cd autoscaler && protoc --go_out=. --go-grpc_out=. api/proto/*.proto
	cd provisioner && protoc --go_out=. --go-grpc_out=. api/proto/*.proto

# Help
help:
	@echo "Orochi Cloud Makefile"
	@echo ""
	@echo "Usage:"
	@echo "  make build          - Build all components"
	@echo "  make test           - Run all tests"
	@echo "  make dev            - Start development servers"
	@echo "  make install        - Install all dependencies"
	@echo "  make lint           - Run linters"
	@echo "  make fmt            - Format code"
	@echo "  make clean          - Clean build artifacts"
	@echo "  make docker-build   - Build Docker images"
	@echo "  make deploy         - Deploy to Kubernetes"
	@echo "  make proto          - Generate protobuf files"
