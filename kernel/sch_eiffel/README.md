# Source code of Eiffel's kernel implementation
These are source code of Eiffel's kernel implementation.

The [Eiffel implementation provided by Saeed](https://github.com/saeed/eiffel_linux)
modifies the vanilla kernel source code.
As a result, enabling Eiffel needs to re-compile the kernel source,
which is a tedious work.

From Linux kernel >= 4.22, I find that it becomes unnecessary
with [Early Departure Time model](https://lwn.net/Articles/766564/).

Therefore, I adapt the origin source code,
so that Eiffel can be enabled by simply inserting a kernel module
without re-compiling kernel sources.

## Tested Environments
- Ubuntu 18.04.3 with Linux 5.3.0-24.
- Ubuntu 20.04.4 with Linux 5.4.0-135.

## Files
- `sch_eiffel.c`: The kernel module of Eiffel qdisc.
- `eiffel-linux-4.10.patch`: a patch file of [Eiffel implementation provided by Saeed](https://github.com/saeed/eiffel_linux). This patch excludes the `.config` file.
- `sch_gq_origin.c`: The source code of `gq` qdisc provided by Saeed.
