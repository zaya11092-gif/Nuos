# nuos_posed Monado patches

`nuos_posed-drivers-cmake.patch` — minimal diff against Monado's
`src/xrt/drivers/CMakeLists.txt`:

1. `add_subdirectory(nuos_posed)` gated on the new option
   `XRT_BUILD_DRIVER_NUOS_POSED`.
2. Links `drv_nuos_posed` into the composite target list so its
   `nuos_posed_create()` factory is visible to `target_factory.c`
   registration (one `extern struct xrt_device *nuos_posed_create(int);`
   declaration plus a guarded call in the driver-probe table is also
   required — Monado master's auto-registration mechanism varies, so the
   patch keeps the integration to the two smallest hooks; adjust if a
   newer Monado renamed them).

The patch is applied idempotently by `build-monado.sh` only when the target
file doesn't already reference `drv_nuos_posed`. Monado master is a moving
target: if `git apply` fails, regenerate the patch with

    git -C build/monado-src diff src/xrt/drivers/CMakeLists.txt \
        > patches/nuos_posed-drivers-cmake.patch

after hand-applying the hooks.
