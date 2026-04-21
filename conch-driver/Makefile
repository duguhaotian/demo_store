.PHONY: all driver tools tests clean

all: driver tools tests

driver:
	$(MAKE) -C driver

tools:
	$(MAKE) -C tools

tests:
	$(MAKE) -C tests

clean:
	$(MAKE) -C driver clean
	$(MAKE) -C tools clean
	$(MAKE) -C tests clean