IDF_PY ?= idf.py

.PHONY: build flash monitor flash-monitor

build:
	$(IDF_PY) build

flash:
	$(IDF_PY) flash

monitor:
	$(IDF_PY) monitor

flash-monitor:
	$(IDF_PY) flash monitor
