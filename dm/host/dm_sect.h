// Registration table sections (HLE / forced takeover / NVN implementations): each source file puts its entries into the same-named section with DM_SECT, and the host takes the whole section with DM_SECT_GET.
// Mach-O: section name "__DATA,name", fetched at run time with getsectiondata.
#pragma once
// 地址消毒器（dmhost_asan）给每个全局变量前后加红区，段里的表项就不再紧挨着、按数组取会读进红区：消毒构建下这些表项不加红区。
// AddressSanitizer (dmhost_asan) pads every global with redzones, so the entries in a section are no longer contiguous and reading them as an array runs into a redzone: in sanitizer builds these entries get no redzones.
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define DM_SECT_NOASAN_ no_sanitize("address"),
#endif
#endif
#ifndef DM_SECT_NOASAN_
#define DM_SECT_NOASAN_
#endif
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#define DM_SECT(name) __attribute__((DM_SECT_NOASAN_ used, section("__DATA," #name)))
// 取段 name 的首址与字节数（找不到段时 p = NULL、bytes = 0）
// Fetch the start address and byte count of section name (p = NULL, bytes = 0 when the section is missing)
#define DM_SECT_GET(name, type, p, bytes) do { unsigned long dm_sz_ = 0; \
    (p) = (type *)getsectiondata(&_mh_execute_header, "__DATA", #name, &dm_sz_); (bytes) = (p) ? dm_sz_ : 0; } while (0)
