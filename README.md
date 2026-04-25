# ask-cmm

This repository owns the standalone source tree for the NXP ASK Connection
Manager daemon (`ask-cmm`).

The OpenWrt integration repository consumes this source through the normal
package fetch path in `openwrt/package/network/ask-cmm/Makefile` using a pinned
`PKG_SOURCE_VERSION` commit and matching `PKG_MIRROR_HASH`. OpenWrt downloads
the pinned source, unpacks it into `build_dir`, applies any package-local
patches from `openwrt/package/network/ask-cmm/patches/` if they exist, and then
builds the package through the standard package workflow.

The following remain owned by the OpenWrt integration package directory rather
than this repository:

- package metadata and dependency declarations
- OpenWrt init and config files under `files/`
- any OpenWrt-specific patch queue under `patches/`
- OpenWrt package release bumps and integration-only build flags

For reproducible packaging, update the OpenWrt package to a specific commit from
this repository and refresh `PKG_MIRROR_HASH` for that exact source archive.
Tags may be added for release landmarks, but packaging must stay pinned to an
exact commit or exact immutable tag target.
