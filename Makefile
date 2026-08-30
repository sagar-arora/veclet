SHELL := /bin/bash
.DEFAULT_GOAL := help

BUF ?= buf
BUF_VERSION ?= 1.72.0
PROTO_BASELINE ?= main

.PHONY: help check check-docs check-proto check-proto-breaking check-repository ci

help: ## Show the available repository commands.
	@printf '%s\n' \
		'Veclet repository commands:' \
		'' \
		'  make help              Show this help.' \
		'  make check             Run every check available in this checkout.' \
		'  make check-docs        Validate local Markdown links.' \
		'  make check-proto       Format, lint, and build protobuf sources.' \
		'  make check-proto-breaking  Compare protobuf sources with main.' \
		'  make check-repository  Validate repository layout and hygiene.' \
		'  make ci                Run the same aggregate checks used by CI.'

check: check-docs check-repository check-proto ## Run every check available in this checkout.

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
