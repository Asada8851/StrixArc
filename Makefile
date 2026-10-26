# Makefile for AMD RDNA 3 (Radeon 780M) macOS Driver Project

CXX ?= clang++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O2
INCLUDES = -Isrc -Isrc/amd -Isrc/uapi -Isrc/winsys

TEST_BIN_DIR = build/tests
TESTS = $(TEST_BIN_DIR)/test_rdna3_discovery_uma \
        $(TEST_BIN_DIR)/test_rdna3_ip_dcn \
        $(TEST_BIN_DIR)/test_rdna3_ucode \
        $(TEST_BIN_DIR)/test_r3n_abi \
        $(TEST_BIN_DIR)/test_rdna3_chip \
        $(TEST_BIN_DIR)/fwplan

.PHONY: all test clean kext fixtures

all: test

$(TEST_BIN_DIR):
	mkdir -p $(TEST_BIN_DIR)

$(TEST_BIN_DIR)/test_rdna3_discovery_uma: src/amd/rdna3_ip_discovery.cpp src/amd/rdna3_rebar_uma.cpp tests/test_rdna3_discovery_uma.cpp | $(TEST_BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

$(TEST_BIN_DIR)/test_rdna3_ip_dcn: src/amd/rdna3_ip_blocks.cpp src/amd/rdna3_dcn314_scanout.cpp src/amd/rdna3_rebar_uma.cpp tests/test_rdna3_ip_dcn.cpp | $(TEST_BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

$(TEST_BIN_DIR)/test_rdna3_ucode: src/amd/rdna3_ucode_extract.cpp tests/test_rdna3_ucode.cpp | $(TEST_BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

$(TEST_BIN_DIR)/test_r3n_abi: src/uapi/r3n_user_client.cpp src/winsys/radv_r3n_winsys.cpp tests/test_r3n_abi.cpp | $(TEST_BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

$(TEST_BIN_DIR)/test_rdna3_chip: tests/test_rdna3_chip.cpp src/amd/rdna3_chip.h tests/test_rdna3_chip_c.c | $(TEST_BIN_DIR)
	$(CC) -std=c99 -Wall -Wextra -Werror -Isrc/amd -c tests/test_rdna3_chip_c.c -o $(TEST_BIN_DIR)/test_rdna3_chip_c.o
	$(CXX) $(CXXFLAGS) $(INCLUDES) tests/test_rdna3_chip.cpp $(TEST_BIN_DIR)/test_rdna3_chip_c.o -o $@

# fwplan is a tool, not a test: built here so CI compiles it; `test` runs it on the Strix Point fixtures
$(TEST_BIN_DIR)/fwplan: tools/fwplan/fwplan.cpp src/amd/rdna3_ucode_extract.cpp | $(TEST_BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

test: $(TESTS)
	@echo "=== Running RDNA 3 Driver Unit Tests ==="
	@for t in $(filter-out %/fwplan,$(TESTS)); do \
		echo "Running $$t..."; \
		$$t || exit 1; \
	done
	@if [ -e tests/fixtures/strix/gc_11_5_0_rlc.bin ]; then \
		echo "Running fwplan on the Strix Point fixtures..."; \
		$(TEST_BIN_DIR)/fwplan tests/fixtures/strix gc_11_5_0 sdma_6_1_0.bin > $(TEST_BIN_DIR)/fwplan_strix.txt || exit 1; \
		test $$(grep -vc '^#' $(TEST_BIN_DIR)/fwplan_strix.txt) -eq 17 || { echo "fwplan: expected 17 payloads"; exit 1; }; \
	fi
	@echo "All unit tests passed successfully!"

fixtures:
	bash tools/fetch_test_firmware.sh tests/fixtures/phoenix phoenix
	bash tools/fetch_test_firmware.sh tests/fixtures/strix strix

kext:
	@echo "Building PhxBringup.kext..."
	python3 tools/build_kext.py

clean:
	rm -rf build/
