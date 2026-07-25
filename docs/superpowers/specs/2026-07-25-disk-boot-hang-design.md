# Disk boot hang diagnosis and repair design

## Problem

The kernel boots without a hard disk, but hangs after the sysfs registration
message whenever an empty IDE or AHCI hard disk is attached. Optical media alone
does not trigger the hang. The disk contents are therefore not a required part of
the reproduction.

## Scope

Work only from the current working tree. Do not inspect or bisect historical
commits. Preserve the user's existing changes in `Makefile` and `init/main.c`.

The repair must keep whole-disk device support: an IDE disk must remain available
as `/dev/hdX`, and an AHCI disk as `/dev/sdX`. Early-boot partition-node discovery
is not required for this repair.

## Diagnosis

Add temporary boundary logs around the current startup stages following
`sysfs_regist()`. Reproduce with three fixed configurations:

1. no hard disk;
2. one empty IDE hard disk;
3. one empty AHCI hard disk.

Use the first boundary whose exit log is absent to narrow the failure. Add a
second, function-local set of logs only inside that boundary, then trace the
shared state and data flow backward until one root cause explains both IDE and
AHCI behavior. Do not make speculative driver changes while collecting evidence.

## Repair

Make one minimal source change at the root cause. Do not disable disk drivers,
skip whole-disk node creation, or bundle unrelated cleanup. Remove all temporary
diagnostic logs once the fix is confirmed.

## Verification

- Build the kernel and boot image successfully.
- Boot to the normal post-filesystem initialization path with no hard disk.
- Boot to the same point with an empty IDE disk and confirm `/dev/hdX` creation.
- Boot to the same point with an empty AHCI disk and confirm `/dev/sdX` creation.
- Confirm the optical-device path still initializes.
- Confirm the final diff preserves unrelated working-tree changes and contains no
  temporary instrumentation.
