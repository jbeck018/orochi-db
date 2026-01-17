# Orochi DB - Monorepo Makefile
# This Makefile delegates to subdirectory Makefiles for building different components

.PHONY: all extension services cli dashboard clean test lint format help

# Default target - show help
all: help

#
# PostgreSQL Extension
#
extension:
	$(MAKE) -C extensions/postgres

extension-install:
	$(MAKE) -C extensions/postgres install

extension-clean:
	$(MAKE) -C extensions/postgres clean

extension-test:
	$(MAKE) -C extensions/postgres installcheck

extension-unit-test:
	cd extensions/postgres/test/unit && make && ./run_tests

#
# Go Services
#
services:
	$(MAKE) -C services

services-control-plane:
	cd services/control-plane && go build -o bin/control-plane ./cmd/server

services-provisioner:
	cd services/provisioner && go build -o bin/provisioner ./cmd/provisioner

services-autoscaler:
	cd services/autoscaler && go build -o bin/autoscaler ./cmd/autoscaler

services-clean:
	rm -rf services/control-plane/bin services/provisioner/bin services/autoscaler/bin

services-test:
	cd services/control-plane && go test ./...
	cd services/provisioner && go test ./...
	cd services/autoscaler && go test ./...

#
# CLI Tool
#
cli:
	cd tools/cli && go build -o bin/orochi ./cmd/orochi

cli-install: cli
	cp tools/cli/bin/orochi /usr/local/bin/orochi

cli-clean:
	rm -rf tools/cli/bin

cli-test:
	cd tools/cli && go test ./...

#
# Dashboard
#
dashboard:
	cd apps/dashboard && npm install && npm run build

dashboard-dev:
	cd apps/dashboard && npm install && npm run dev

dashboard-clean:
	rm -rf apps/dashboard/dist apps/dashboard/node_modules

#
# All Components
#
build-all: extension services cli dashboard

clean: extension-clean services-clean cli-clean dashboard-clean
	rm -rf go.work.sum

test: extension-unit-test services-test cli-test

#
# Code Quality
#
lint:
	$(MAKE) -C extensions/postgres lint
	cd services/control-plane && go vet ./...
	cd services/provisioner && go vet ./...
	cd services/autoscaler && go vet ./...
	cd tools/cli && go vet ./...

format:
	$(MAKE) -C extensions/postgres format
	cd services/control-plane && go fmt ./...
	cd services/provisioner && go fmt ./...
	cd services/autoscaler && go fmt ./...
	cd tools/cli && go fmt ./...

#
# Go Workspace
#
go-sync:
	go work sync

go-tidy:
	cd services/control-plane && go mod tidy
	cd services/provisioner && go mod tidy
	cd services/autoscaler && go mod tidy
	cd tools/cli && go mod tidy
	cd packages/go/shared && go mod tidy

#
# Development
#
dev: dashboard-dev

#
# Help
#
help:
	@echo "Orochi DB - Monorepo Build System"
	@echo ""
	@echo "PostgreSQL Extension:"
	@echo "  make extension           - Build the PostgreSQL extension"
	@echo "  make extension-install   - Install extension to PostgreSQL"
	@echo "  make extension-test      - Run extension regression tests"
	@echo "  make extension-unit-test - Run extension unit tests"
	@echo "  make extension-clean     - Clean extension build artifacts"
	@echo ""
	@echo "Go Services:"
	@echo "  make services            - Build all Go services"
	@echo "  make services-control-plane - Build control plane service"
	@echo "  make services-provisioner   - Build provisioner service"
	@echo "  make services-autoscaler    - Build autoscaler service"
	@echo "  make services-test          - Run service tests"
	@echo "  make services-clean         - Clean service binaries"
	@echo ""
	@echo "CLI Tool:"
	@echo "  make cli          - Build CLI tool"
	@echo "  make cli-install  - Install CLI to /usr/local/bin"
	@echo "  make cli-test     - Run CLI tests"
	@echo "  make cli-clean    - Clean CLI binary"
	@echo ""
	@echo "Dashboard:"
	@echo "  make dashboard      - Build dashboard for production"
	@echo "  make dashboard-dev  - Run dashboard in development mode"
	@echo "  make dashboard-clean - Clean dashboard build artifacts"
	@echo ""
	@echo "All Components:"
	@echo "  make build-all  - Build all components"
	@echo "  make clean      - Clean all build artifacts"
	@echo "  make test       - Run all tests"
	@echo "  make lint       - Run linters on all code"
	@echo "  make format     - Format all code"
	@echo ""
	@echo "Go Workspace:"
	@echo "  make go-sync    - Sync go.work file"
	@echo "  make go-tidy    - Run go mod tidy on all modules"
