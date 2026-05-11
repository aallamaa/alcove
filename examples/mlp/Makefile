# Tiny MLP character classifier on the UCI optical-digits dataset.
#
# Pipeline:
#   make data    download raw CSVs, pack into digits.bin (Python)
#   make db      digits.bin → digits.dump (alcove vec persistence)
#   make train   load digits.dump, train + report accuracy (alcove)
#   make         shorthand for: data → db → train
#
# All commands assume alcove has been built at ../../alcove. If it
# isn't there, build it with `(cd ../.. && make jit)` first.

ALCOVE := $(abspath ../../alcove)
PY     := python3

.PHONY: all data db train clean distclean

all: train

data: digits.bin
db:   digits.dump

digits.bin: prepare-data.py
	$(PY) prepare-data.py

digits.dump: digits.bin build-db.alc $(ALCOVE)
	$(ALCOVE) --noload build-db.alc

train: digits.dump $(ALCOVE) mlp.alc
	$(ALCOVE) --db digits.dump --no-init mlp.alc

# Side-by-side: baseline (per-element interpreter loops) vs tensor ops.
# Same architecture, same data, same epoch count — only the inner loops
# differ. Showcases the speedup from vec-dot / vec-axpy! etc.
benchmark: digits.dump $(ALCOVE) mlp.alc mlp-baseline.alc
	@echo "=================================================="
	@echo " BASELINE (per-element interpreter loops)"
	@echo "=================================================="
	@$(ALCOVE) --db digits.dump --no-init mlp-baseline.alc
	@echo
	@echo "=================================================="
	@echo " TENSOR OPS (vec-dot / vec-axpy! / vec-relu! …)"
	@echo "=================================================="
	@$(ALCOVE) --db digits.dump --no-init mlp.alc

# Quick sanity check: how big are the pieces?
sizes:
	@ls -lh digits.bin digits.dump 2>/dev/null || echo "(run `make` first)"

clean:
	rm -f digits.dump

distclean: clean
	rm -f digits.bin optdigits.tra optdigits.tes
