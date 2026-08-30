SHELL := /bin/bash
.DEFAULT_GOAL := help

BUF ?= buf
BUF_VERSION ?= 1.72.0
CMAKE ?= cmake
CTEST ?= ctest
GO ?= go
GO_VERSION ?= 1.26.7
PROTO_BASELINE ?= main

.PHONY: check check-cpp check-docs check-go check-proto check-proto-breaking \
	check-repository ci generate-go help

help: ## Show the available repository commands.
	@printf '%s\n' \
		'Veclet repository commands:' \
		'' \
		'  make help              Show this help.' \
		'  make check             Run every check available in this checkout.' \
		'  make check-docs        Validate local Markdown links.' \
		'  make generate-go       Generate ignored Go protobuf bindings locally.' \
		'  make check-go          Generate, format-check, vet, and test Go bindings.' \
		'  make check-cpp         Generate, build, and test C++ bindings.' \
		'  make check-proto       Format, lint, and build protobuf sources.' \
		'  make check-proto-breaking  Compare protobuf sources with main.' \
		'  make check-repository  Validate repository layout and hygiene.' \
		'  make ci                Run the same aggregate checks used by CI.'

check: check-docs check-repository check-proto check-go check-cpp ## Run every check available in this checkout.

generate-go: ## Generate ignored Go protobuf bindings with pinned local tools.
	@actual_version="$$($(GO) version | awk '{print $$3}')"; \
		test "$$actual_version" = "go$(GO_VERSION)" || { \
			printf 'go %s required; found %s\n' "$(GO_VERSION)" "$$actual_version" >&2; \
			exit 1; \
		}
	@go_path="$$(dirname "$$(command -v '$(GO)')")"; \
		PATH="$$go_path:$$PATH" $(BUF) generate --template buf.gen.go.yaml

check-go: generate-go ## Format-check, vet, and test generated Go bindings.
	@gofmt_path="$$(dirname "$$(command -v '$(GO)')")/gofmt"; \
		unformatted="$$($$gofmt_path -l tools/toolchain)"; \
		test -z "$$unformatted" || { \
			printf 'unformatted Go files:\n%s\n' "$$unformatted" >&2; \
			exit 1; \
		}
	@$(GO) mod tidy -diff
	@$(GO) vet ./...
	@$(GO) test ./...

check-cpp: ## Generate, build, and test C++ bindings with vcpkg.
	@test -n "$(VCPKG_ROOT)" || { \
		printf 'VCPKG_ROOT must name the pinned vcpkg checkout\n' >&2; \
		exit 1; \
	}
	@buf_path="$$(command -v '$(BUF)')"; \
		$(CMAKE) --preset cpp -DBUF_EXECUTABLE="$$buf_path"
	@$(CMAKE) --build --preset cpp
	@$(CTEST) --preset cpp

check-docs: ## Validate local Markdown links.
	@./tools/ci/check-docs.sh

check-proto: ## Format, lint, and build protobuf sources.
	@actual_version="$$($(BUF) --version)"; \
		test "$$actual_version" = "$(BUF_VERSION)" || { \
			printf 'buf %s required; found %s\n' "$(BUF_VERSION)" "$$actual_version" >&2; \
			exit 1; \
		}
	@$(BUF) format --diff --exit-code
	@$(BUF) lint
	@$(BUF) build >/dev/null

check-proto-breaking: check-proto ## Compare protobuf sources with main.
	@PATH="$$(dirname "$(BUF)"):$$PATH" ./tools/ci/check-proto-breaking.sh '$(PROTO_BASELINE)'

check-repository: ## Validate repository layout and hygiene.
	@./tools/ci/check-repository.sh

ci: check check-proto-breaking ## Run the same aggregate checks used by CI.
