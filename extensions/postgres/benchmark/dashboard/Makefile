# Orochi DB Benchmark Dashboard Makefile
#
# Commands for generating reports and running the dashboard

.PHONY: all setup report charts serve clean help

# Default target
all: report

# Install Python dependencies
setup:
	@echo "Installing Python dependencies..."
	pip3 install -r requirements.txt

# Generate full report with charts
report:
	@echo "Generating benchmark report..."
	./generate_report.sh -c -o ./output

# Generate report without charts (faster)
report-fast:
	@echo "Generating benchmark report (no charts)..."
	./generate_report.sh -o ./output

# Generate only charts
charts:
	@echo "Generating charts..."
	python3 chart_generator.py -i ./output/data/summary.json -o ./output/charts

# Start the web server
serve:
	@echo "Starting web dashboard..."
	python3 web_server.py -r ../results -p 5000

# Start static file server (simpler, no Flask required)
serve-static:
	@echo "Starting static file server..."
	cd output && python3 -m http.server 8000

# Generate report and serve
dev: report serve

# Clean generated files
clean:
	rm -rf output/
	rm -rf __pycache__/
	rm -f *.pyc

# Show help
help:
	@echo "Orochi DB Benchmark Dashboard"
	@echo ""
	@echo "Available targets:"
	@echo "  make setup        - Install Python dependencies"
	@echo "  make report       - Generate full report with charts"
	@echo "  make report-fast  - Generate report without charts"
	@echo "  make charts       - Generate only charts"
	@echo "  make serve        - Start web dashboard (Flask)"
	@echo "  make serve-static - Start static file server"
	@echo "  make dev          - Generate report and start server"
	@echo "  make clean        - Remove generated files"
	@echo "  make help         - Show this help"
