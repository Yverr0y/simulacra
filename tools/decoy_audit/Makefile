CC ?= cc
ROOT := ../..
# KEEP IN SYNC WITH run.ps1's cl invocation. This harness has TWO build recipes -- this Makefile
# (what CI runs) and the PowerShell one in run.ps1 (what the bench runs). Adding a source to only
# one produces a link failure the other build never sees, which is exactly how this file went stale
# on 2026-08-26. If you touch one, grep for the other.
SRC := synth_dump.c ble_hs_adv.c roster_stub.c \
       $(ROOT)/main/generate.c $(ROOT)/main/templates.c $(ROOT)/main/roster.c $(ROOT)/main/ble_devices.c \
       $(ROOT)/main/learn.c $(ROOT)/components/simulacra_radar/law3.c $(ROOT)/components/simulacra_radar/learn_wire.c \
       $(ROOT)/main/uniq_id.c $(ROOT)/main/phantom.c $(ROOT)/main/probe_agents.c $(ROOT)/main/ssid_pool.c $(ROOT)/main/probe_frame.c \
       $(ROOT)/main/fleet_pop.c $(ROOT)/main/fleet.c \
       $(ROOT)/main/rf_model.c \
       $(ROOT)/main/sig_store.c $(ROOT)/components/simulacra_radar/sig_match.c \
       $(ROOT)/components/simulacra_radar/sig_seed.c \
       $(ROOT)/components/simulacra_radar/radar_pad.c $(ROOT)/components/simulacra_radar/radar_retx.c
INC := -Ihost_stubs -I$(ROOT)/main -I$(ROOT)/components/simulacra_radar
# rf_model.c is linked WHOLE so the audit exercises the real rf_adstruct_sample/rf_tx_sample rather
# than a stand-in. SIMULACRA_HOST_NO_NVS drops its two NVS functions, which mean nothing on a host
# and would collide with roster_stub.c's versions.
CFLAGS ?= -O2 -Wall -Wno-unused-parameter -DSIMULACRA_HOST_NO_NVS
all: synth_dump
synth_dump: $(SRC)
	$(CC) $(CFLAGS) $(INC) $(SRC) -o synth_dump
clean:
	rm -f synth_dump synth_dump.exe
