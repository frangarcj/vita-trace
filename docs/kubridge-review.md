# kubridge comparison and IRQ entry contract

## Inspected sources

The comparison uses these exact Git revisions, cloned read-only under the
ignored `build-kubridge-review/` directory:

- TheOfficialFloW/kubridge: `40bffc3b4a98a6027617a353231308e8bba74305`.
- bythos14/kubridge: `417ddde9a744eba98d769382c1c1372b8e119139`.

Primary references:

- https://github.com/TheOfficialFloW/kubridge/blob/40bffc3b4a98a6027617a353231308e8bba74305/main.c
- https://github.com/bythos14/kubridge/blob/417ddde9a744eba98d769382c1c1372b8e119139/src/exceptions.c
- https://github.com/bythos14/kubridge/blob/417ddde9a744eba98d769382c1c1372b8e119139/src/exceptions.S
- https://github.com/bythos14/kubridge/blob/417ddde9a744eba98d769382c1c1372b8e119139/src/main.c
- https://docs.vitasdk.org/group__SceExcpmgrKernel.html

The original tree supplies syscall wrapping and runtime export resolution.
The bythos tree adds explicit exception entry/exit assembly, process-local
handler state and short per-core initialization jobs. This comparison is not
a claim about the latest release or validation of these sources on our console.

## The important distinction

`exceptions.c:193-195` registers priority-zero **raw** abort/undefined handlers.
Each node in `exceptions.S` begins with two words, preserves the registers it
uses, captures user-bank SP/LR with a privileged register transfer, and chains
to the next node's code at `next + 8`. Its saved context is built by its own
assembly, not passed by an ordinary C caller. Its PC adjustment is specific to
the exception kind; its abort corrections are not an IRQ correction recipe.

Our former priority-seven stub was only two words and a branch to C. Its tests
called that C function with an already constructed SceExcpmgrExceptionContext.
That established neither the incoming machine state nor that Sony called it
with that C signature, and did not exercise preservation or onward chaining.
The existence of a C typedef in a header was insufficient evidence.

`kernel/irq_entry.S` now implements a separate priority-zero raw IRQ entry.
It constructs the private `VitaTracyIrqFrame`, saves r0-r12, LR_irq, user SP/LR,
SPSR and entry flags, aligns SP to 8 bytes before calling C, restores the saved
state, and always tail-chains to the next node. It never returns directly to
LR_irq, changes the interrupted PC, handles an abort, or acknowledges the GIC.
The C consumer derives an IRQ resume PC as `LR_irq - 4` for ARM and Thumb,
rejects malformed instruction state/alignment, and still filters non-user and
other-process observations. No user memory is dereferenced by the new entry.

This is an independent, reduced IRQ implementation; kubridge's user-space
exception redirection, NEON save/restore, SWP enablement and memory-permission
patches have not been copied or enabled. Our entry and C build use no VFP/NEON
instructions. Imported kernel functions still need real preservation testing.

A successful registration without a next link refuses PMU activation and
continues to pin the plugin resident. There is no known public unregister
operation; kubridge's release of a *user* exception callback is not removal of
its kernel exception-chain node. We do not unlink undocumented firmware lists.

## What is tested, and what is not

`tools/test_irq_entry.py` compiles the actual assembly with VitaSDK and executes
it on Unicorn's Cortex-A9 model. It checks ARM and Thumb C targets, user ARM and
Thumb state, distinct banked LRs, a four-byte-aligned incoming IRQ stack,
caller-saved register/flag clobbering, and arrival at the next raw handler with
the original registers restored. Under this raw-entry model the former
branch-only stub fails all five cases; the replacement passes. C unit tests
separately cover frame conversion, invalid PCs and missing next links.

Unicorn is optional (`unicorn==2.1.4` was used for this run). Without it or the
Vita compiler these five Python cases explicitly skip. Select an interpreter
with Unicorn using CMake's `Python3_EXECUTABLE`, or run directly:

```sh
python -B -m unittest discover -s tools -p test_irq_entry.py -v
```

These are **not** Sony firmware tests. Still unverified: whether PMUIRQ reaches
the priority-zero IRQ chain on each selected core; which PMUIRQ interrupt lines
Sony enables/dispatches; coexistence with its GIC handler after our PMOVSR clear;
available IRQ stack space; exception safety of the invoked kernel routines;
configuration persistence, latency and overhead. kubridge demonstrates abort
and undefined handling, not these IRQ-specific facts. Do not describe this
backend as hardware-validated or remove its explicit experimental opt-in.

The writable chain header still shares an executable section, so the ELF linker
reports RWX. This change does not claim to solve that mapping constraint. Use a
recoverable setup and start with one core and a low rate before expanding.
