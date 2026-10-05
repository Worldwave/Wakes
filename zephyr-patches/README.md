# zephyr-patches

Local patches to the pinned Zephyr v4.3.1 checkout. These live outside `west`'s
manifest and do **not** survive `west update` — reapply them (`git apply` from
the `zephyr/` checkout root) whenever the Zephyr tree is re-cloned or reset.

- `nordic-cmsis-system-core-clock.patch` — `soc/nordic/Kconfig`'s
  `SOC_FAMILY_NORDIC_NRF` unconditionally selects
  `CMSIS_CORE_HAS_SYSTEM_CORE_CLOCK`, but `modules/cmsis_6/Kconfig` only
  allows that symbol for `SOC_SERIES_IMXRT6XX` (NXP). That makes the symbol
  unreachable for any Nordic build, which Zephyr's `kconfig.py` treats as a
  fatal error on a fresh (non-incremental) configure — this breaks *any*
  from-scratch nRF52 build on this exact Zephyr tag, not just ours (confirmed
  against the stock `samples/hello_world` on `nrf52840dk/nrf52840`). Verified
  against Zephyr's `main` branch on 2026-09-14: both `soc/nordic/Kconfig` and
  `modules/cmsis_6/Kconfig` are unchanged there too, so this is not yet fixed
  upstream. The patch adds a `default y` for the symbol scoped to
  `SOC_FAMILY_NORDIC_NRF`, mirroring the existing `SOC_SERIES_IMXRT6XX` block
  in `soc/nxp/imxrt/imxrt6xx/Kconfig.defconfig`.

- `udc_nrf-fast-paths.patch` (#32) — serves USB endpoints from the nRF USB
  driver's interrupt instead of its threads. In stock Zephyr every packet on a
  non-control endpoint costs an interrupt, a wake of the driver thread, a
  message to the usbd thread, a net_buf free and alloc and another wake to
  re-arm: four thread switches, in threads that run above our audio thread.
  Measured on the SP-1, ~110 µs per USB-MIDI message, ~9 points of the audio
  budget under a DAW's CC stream. Adds `include/zephyr/drivers/usb/udc_nrf_fast.h`
  and two options in `drivers/usb/udc/Kconfig.nrf`, both off unless set:
  - `CONFIG_UDC_NRF_OUT_FAST` — one bulk OUT endpoint's packets are DMA'd into
    a static buffer and handed to a callback in the interrupt
    (`udc_nrf_out_fast_start()` / `_stop()`). Wakes' MIDI input uses it
    (`firmware/prj.conf`; feldd's class calls it from its enable/disable).
  - `CONFIG_UDC_NRF_ISO_IN_FAST` — Ryan Gilmore's ISO IN fast path from
    sp1-usb-audio, for USB audio out (M5c), with the two guards it relies on
    from nRF Connect SDK's later driver (a DMA left running across SOF; the
    `dma_ep` reset). Compiles clean; untested on hardware until M5c.
    Reviewed against Ryan's base driver (nRF Connect SDK `sdk-zephyr` `fd9204a0`,
    kept in `third_party/sp1-usb-audio/upstream/module/nrf-usbd-isofast/patches/`):
    the code the fast path hooks into (`ev_sof_handler`, `usbd_dmareq_process`,
    `nrf_usbd_dma_finished`) is the same in 4.3.1 apart from those two guards, and the
    port matches Ryan's patch hunk for hunk. 4.3.1's other differences are in control
    transfers (EP0) and in where an endpoint dequeue runs; neither touches ISO IN.
    Endpoint abort and disable wait for any running DMA (`dma_available`) before
    clearing the endpoint, so closing the stream cannot cut a fast-path DMA short.

  With neither option set the driver is unchanged. The two paths serve
  different endpoints through the driver's own DMA scheduler, which serves IN
  endpoints before OUT ones, so an ISO IN packet is never queued behind a MIDI
  packet.
