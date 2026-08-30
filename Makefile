SHELL := /bin/bash
.DEFAULT_GOAL := help

.PHONY: help check check-docs check-repository ci

help: ## Show the available repository commands.
	@printf '%s\n' \
		'Veclet repository commands:' \
		'' \
		'  make help              Show this help.' \
		'  make check             Run every check available in this checkout.' \
		'  make check-docs        Validate local Markdown links.' \
		'  make check-repository  Validate repository layout and hygiene.' \
		'  make ci                Run the same aggregate checks used by CI.'

check: check-docs check-repository ## Run every check available in this checkout.

check-docs: ## Validate local Markdown links.
	@./tools/ci/check-docs.sh

check-repository: ## Validate repository layout and hygiene.
	@./tools/ci/check-repository.sh

ci: check ## Run the same aggregate checks used by CI.
