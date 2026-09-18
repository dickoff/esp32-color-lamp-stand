IDF_PY ?= $(IDF_PATH)/tools/idf.py
PYTHON ?= python3
BATCH_COUNT ?= 10

.PHONY: build flash monitor flash-monitor flash-batch

build:
	$(PYTHON) "$(IDF_PY)" build

flash:
	$(PYTHON) "$(IDF_PY)" flash

monitor:
	$(PYTHON) "$(IDF_PY)" monitor

flash-monitor:
	$(PYTHON) "$(IDF_PY)" flash monitor

flash-batch:
	$(PYTHON) tools/flash_batch.py --idf-py "$(IDF_PY)" --count $(BATCH_COUNT)
