.DEFAULT_GOAL := all
BLENDER ?= /Applications/Blender.app/Contents/MacOS/Blender
PYTHON ?= $(shell python3 scripts/find_python.py)
OUT ?= out
STEPS ?= 5
EXTRA ?=

.PHONY: all setup preview layers poses composite check stages clean restore

all: poses composite

setup:
	@if ! "$(PYTHON)" -c 'import PIL' >/dev/null 2>&1; then \
		if [ "$(PYTHON)" != '.venv/bin/python' ]; then echo 'PYTHON override must point to an interpreter with Pillow installed.' >&2; exit 1; fi; \
		python3 -m venv .venv && .venv/bin/python -m pip install -r scripts/requirements.txt; \
	fi
	@"$(PYTHON)" -c 'import PIL; print("Packer Pillow", PIL.__version__)'

preview: setup restore
	"$(BLENDER)" -b --factory-startup -P scripts/build_flower.py -- --fast --mode composite --out "$(OUT)/preview" $(EXTRA)
	"$(PYTHON)" scripts/pack_assets.py --manifest "$(OUT)/preview/source_manifest.json"

composite: setup restore
	"$(BLENDER)" -b --factory-startup -P scripts/build_flower.py -- --mode composite --out "$(OUT)/reference_composite" $(EXTRA)
	"$(PYTHON)" scripts/pack_assets.py --manifest "$(OUT)/reference_composite/source_manifest.json"

layers: setup restore
	"$(BLENDER)" -b --factory-startup -P scripts/build_flower.py -- --mode layers --out "$(OUT)" --save-blend $(EXTRA)
	"$(PYTHON)" scripts/pack_assets.py --manifest "$(OUT)/source_manifest.json"
	"$(PYTHON)" scripts/verify_assets.py "$(OUT)/manifest.json"

poses: setup restore
	"$(BLENDER)" -b --factory-startup -P scripts/build_flower.py -- --mode poses --steps "$(STEPS)" --out "$(OUT)" --save-blend $(EXTRA)
	"$(PYTHON)" scripts/pack_assets.py --manifest "$(OUT)/source_manifest.json"
	"$(PYTHON)" scripts/verify_assets.py "$(OUT)/manifest.json"

check: setup
	"$(PYTHON)" -m unittest discover -s scripts -p 'test_*.py'

stages: setup restore
	"$(BLENDER)" -b --factory-startup -P scripts/build_flower.py -- --stage camera --diagnostic --fast --out "$(OUT)/stages/camera"
	"$(BLENDER)" -b --factory-startup -P scripts/build_flower.py -- --stage petals --fast --width 200 --height 120 --out "$(OUT)/stages/petals"
	"$(BLENDER)" -b --factory-startup -P scripts/build_flower.py -- --stage leaves --fast --width 200 --height 120 --out "$(OUT)/stages/leaves"
	"$(BLENDER)" -b --factory-startup -P scripts/build_flower.py -- --stage materials --fast --width 200 --height 120 --out "$(OUT)/stages/materials"

restore:
	@cp scripts/README.md README.md
	@printf '%s\n' 'include scripts/Makefile' > Makefile

clean:
	python3 scripts/clean.py
