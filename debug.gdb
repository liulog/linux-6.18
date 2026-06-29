# GDB script for debugging iee_gate
file vmlinux
target remote localhost:1234

# Set a breakpoint at iee_gate
break iee_gate

# Continue execution until the breakpoint is hit
# continue
