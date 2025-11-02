PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin
SYSTEMD_DIR = /etc/systemd/system

SERVICE_NAME = lcd-system-monitor
SERVICE_FILE = $(SERVICE_NAME).service
PY_SCRIPT = $(SERVICE_NAME).py

install:
	@echo "Installing $(PY_SCRIPT) to $(BINDIR)"
	install -Dm755 $(PY_SCRIPT) $(BINDIR)/$(PY_SCRIPT)

	@echo "Installing systemd service file..."
	install -Dm644 $(SERVICE_FILE) $(SYSTEMD_DIR)/$(SERVICE_FILE)

	@echo "Reloading systemd daemon..."
	systemctl daemon-reload

	@echo "Enabling $(SERVICE_NAME) service..."
	systemctl enable $(SERVICE_NAME).service

	@echo "Starting $(SERVICE_NAME) service..."
	systemctl restart $(SERVICE_NAME).service

	@echo "Installation complete."

uninstall:
	@echo "Stopping and disabling service..."
	-systemctl stop $(SERVICE_NAME).service || true
	-systemctl disable $(SERVICE_NAME).service || true

	@echo "Removing installed files..."
	rm -f $(BINDIR)/$(PY_SCRIPT)
	rm -f $(SYSTEMD_DIR)/$(SERVICE_FILE)

	@echo "Reloading systemd daemon..."
	systemctl daemon-reload

	@echo "Uninstall complete."
