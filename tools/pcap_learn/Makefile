CC ?= cc
ROOT := ../..
# learn.c gained a sig_store dependency on 2026-08-26: learn_strip() now refuses to adopt any shape
# whose company id or service uuid matches a seeded THREAT signature, so a real AirTag/Tile/SmartTag
# in the room cannot be learned and then cloned. That pulls sig_store + sig_match + sig_seed into
# anything that links learn.c, including this harness.
SRC := harness.c \
       $(ROOT)/components/simulacra_radar/law3.c \
       $(ROOT)/components/simulacra_radar/learn_wire.c \
       $(ROOT)/main/learn.c \
       $(ROOT)/main/sig_store.c \
       $(ROOT)/components/simulacra_radar/sig_match.c \
       $(ROOT)/components/simulacra_radar/sig_seed.c
SCAN_SRC := sig_scan.c \
       $(ROOT)/components/simulacra_radar/sig_match.c \
       $(ROOT)/components/simulacra_radar/sig_seed.c
INC := -Ihost_stubs -I$(ROOT)/components/simulacra_radar -I$(ROOT)/main
CFLAGS ?= -O2 -Wall -Wno-unused-parameter

all: harness sig_scan

harness: $(SRC)
	$(CC) $(CFLAGS) $(INC) $(SRC) -o harness

sig_scan: $(SCAN_SRC)
	$(CC) $(CFLAGS) $(INC) $(SCAN_SRC) -o sig_scan

clean:
	rm -f harness harness.exe sig_scan sig_scan.exe
