# Third-party components

The standalone firmware links Zephyr and its manifest-pinned modules. Zephyr and Arm CMSIS-DSP use Apache-2.0. Nordic nrfx source files carry Nordic’s BSD notices. Mbed TLS carries its upstream license. The upstream texts are included in the notices directory. Component revisions are resolved by the Zephyr release pinned in west.yml.

The G.722 source and reference data come from sippy/libg722 v1.2.8. See [codec provenance](codec/ORIGIN.md) and [codec license](codec/LICENSE).

The firmware also links Picolibc and the GCC runtime from the Zephyr SDK. Their notices and the GCC Runtime Library Exception are in the notices directory. Release downloads include a combined copy of the component notices alongside the firmware ZIPs.
