# Builds the screen for the host, for design/tools/compare-c.js.
CC ?= cc
CFLAGS ?= -O2 -std=c99 -Wall -Wextra -Wdouble-promotion -Wno-unused-parameter -D_DEFAULT_SOURCE
SRC := ../src/screen/babel_screen.c host_render.c

host_render: $(SRC) ../src/screen/babel_screen.h ../src/screen/bs_tables.h
	$(CC) $(CFLAGS) -I../src/screen -o $@ $(SRC) -lm

clean:
	rm -f host_render mtp_output_test mtp_output_extract.inc

# Pulls babel_build_output() out of the MTP implementation.
mtp_output_extract.inc: ../src/monolith/usb_mtp_impl.c.h
	sed -n '/^static void babel_build_output/,/^}/p' $< > $@

mtp_output_test: mtp_output_test.c mtp_output_extract.inc ../src/screen/babel_screen.c
	$(CC) $(CFLAGS) -I../src/screen -I. -o $@ mtp_output_test.c ../src/screen/babel_screen.c -lm

test: host_render mtp_output_test
	./mtp_output_test
	cd ../../design && node tools/verify.js
