# Kernel source code of eiffel
These are source code of eiffel.

The [eiffel implementation provided by Saeed](https://github.com/saeed/eiffel_linux)
modifies the kernel source code.
Enabling Eiffel needs to re-compile the kernel source,
which is a tedious work.

For Linux kernel >= 4.22, it is unnecessary.
With the [Early Departure Time module](https://lwn.net/Articles/766564/),
we only need to add a new qdisc.

Therefore, I adapt the source code 

## Environments
- Ubuntu 18.04.3 with Linux 5.3.0-24.

## Files
- `sch_gq.c`: The qdisc to enable eiffel.
- `eiffel-linux-4.10.patch`: a patch file of [eiffel implementation provided by Saeed](https://github.com/saeed/eiffel_linux). This patch excludes the `.config` file.
- `sch_gq_origin.c`: The source code of `gq` qdisc provided by Saeed.