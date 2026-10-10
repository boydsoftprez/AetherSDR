CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror
all: ipv4_fixture libnereus_ipv6.so abi_checks extension_checks mixed_fixture
ipv4_fixture: ipv4_fixture.c
	$(CC) $(CFLAGS) -o $@ $<
libnereus_ipv6.so: ipv6_shim.c
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $< -ldl -pthread
clean:
	rm -f ipv4_fixture libnereus_ipv6.so abi_checks extension_checks mixed_fixture
.PHONY: all clean
abi_checks: abi_checks.c
	$(CC) $(CFLAGS) -o $@ $< -pthread

extension_checks: extension_checks.c
	$(CC) $(CFLAGS) -o $@ $<

mixed_fixture: mixed_fixture.c
	$(CC) $(CFLAGS) -o $@ $<

test: all
	python3 test_all.py
.PHONY: test
