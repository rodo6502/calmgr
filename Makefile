COBC ?= cobc
COBFLAGS ?= -free -std=default -Wall -Wextra -Wno-terminator -I src/copybooks
PYTHON ?= python3
TEST_TIMEOUT ?= 30s
BUILD := build
VERSION := 1.0.0
DIST_NAME := calmgr-$(VERSION)
NATIVE_SOURCES := src/native/appointment_sqlite.c
NATIVE_OBJECTS := $(patsubst %.c,$(BUILD)/%.o,$(NATIVE_SOURCES))
NATIVE_LIBS := -lsqlite3 -lsodium
PROGRAMS := calmgr calmgr-batch
MODULE_SOURCES := src/services/appointment-dates.cob src/services/appointment-recurrence.cob \
	src/services/appointment-repository.cob src/services/appointment-backup.cob \
	src/services/appointment-audit.cob src/services/appointment-export.cob \
	src/services/appointment-service.cob src/adapters/appointment-config.cob \
	src/adapters/appointment-json.cob
MODULE_OBJECTS := $(patsubst %.cob,$(BUILD)/%.o,$(MODULE_SOURCES))
TESTS := test-dates test-recurrence test-repository test-service test-json

.PHONY: all test check run-tui run-web clean dist
all: $(PROGRAMS)

calmgr: src/programs/calmgr.cob $(MODULE_OBJECTS) $(NATIVE_OBJECTS)
	$(COBC) -x $(COBFLAGS) -o $@ $^ $(NATIVE_LIBS)
calmgr-batch: src/programs/calmgr-batch.cob $(MODULE_OBJECTS) $(NATIVE_OBJECTS)
	$(COBC) -x $(COBFLAGS) -o $@ $^ $(NATIVE_LIBS)
$(BUILD)/%.o: %.cob Makefile
	mkdir -p $(dir $@)
	$(COBC) -c $(COBFLAGS) -o $@ $<
$(BUILD)/%.o: %.c Makefile
	mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -c -o $@ $<
$(BUILD)/tests/%: tests/%.cob $(MODULE_OBJECTS) $(NATIVE_OBJECTS)
	mkdir -p $(BUILD)/tests
	$(COBC) -x $(COBFLAGS) -o $@ $< $(MODULE_OBJECTS) $(NATIVE_OBJECTS) $(NATIVE_LIBS)

test: all $(addprefix $(BUILD)/tests/,$(TESTS))
	set -e; for test in $(addprefix $(BUILD)/tests/,$(TESTS)); do timeout $(TEST_TIMEOUT) $$test; done
	$(PYTHON) scripts/check_print_reports.py
	$(PYTHON) -m unittest discover -s tests -p 'test_*.py' -v
	$(PYTHON) scripts/check_examples.py
check: test
	$(PYTHON) -m py_compile web/web_app.py scripts/check_examples.py scripts/check_print_reports.py
	! rg -n 'shell[[:space:]]*=[[:space:]]*True|appointment_manager\.cob|appointment_batch\.cob' web src
run-tui: calmgr
	./calmgr
run-web: calmgr-batch
	$(PYTHON) web/web_app.py
clean:
	rm -rf build/dist build/src build/tests
	rm -f calmgr calmgr-batch calmgr-*.tar.gz
	rm -f build/test-*.dat build/test-*.log build/test-*.txt build/test-*.csv build/test-*.ics
	rm -f build/test-*.db build/test-*.db-wal build/test-*.db-shm
dist:
	rm -rf build/dist
	mkdir -p build/dist/$(DIST_NAME)
	tar --exclude='./build' --exclude='./data/*' --exclude='./*.tar.gz' --exclude='*/.pytest_cache' \
	  --exclude='*/__pycache__' --exclude='*.pyc' --exclude='./imports/*' \
	  --exclude='./request*.json' --exclude='./response*.json' --exclude='./sessions/*' \
	  --exclude='*.db' --exclude='*.db-wal' --exclude='*.db-shm' --exclude='*.tmp' \
	  -cf - . | tar -xf - -C build/dist/$(DIST_NAME)
	mkdir -p build/dist/$(DIST_NAME)/data build/dist/$(DIST_NAME)/imports
	touch build/dist/$(DIST_NAME)/data/.gitkeep build/dist/$(DIST_NAME)/imports/.gitkeep
	tar -czf $(DIST_NAME).tar.gz -C build/dist $(DIST_NAME)
	tar -tzf $(DIST_NAME).tar.gz >/dev/null
