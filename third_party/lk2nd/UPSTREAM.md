# lk2nd for MSM8916

The loader SOS2 boots from on the Redmi 2: the unmodified
[lk2nd 23.1 release](https://github.com/msm8916-mainline/lk2nd/releases/tag/23.1)
image for MSM8916 (tag 23.1, commit `e9c8b21735c13dd7ace808b1e35451638f0a0c6d`),
an Android boot image that the stock bootloader starts from the `boot`
partition. Its SHA-256 equals the digest GitHub publishes for the release
asset. It is tracked so the phone's loader can be reinstalled exactly; SOS2's
build does not use it.

The source is at that tag. `LICENSE` is lk2nd's own, from the same tag: Little
Kernel's MIT license and the terms of the code added since.

[Redmi 2](../../docs/redmi2.md#loader-and-hardware-contract) cites lk2nd's
source at `3e86db90765468c55e1be507cb897efd20876b71`, 27 commits after 23.1,
for the loader behaviour SOS2 relies on.

| File | SHA-256 |
| --- | --- |
| `lk2nd-msm8916.img` | `8f5f788b45b96447517ff121f20841e6bfc95a5d7c4b48fdedb855d90f593144` |
| `LICENSE` | `8ad52655e145f95f04b35908c8f8f1853c69f58321c587be6daccacb6a121b62` |
