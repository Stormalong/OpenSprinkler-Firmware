#pragma once

#include <cstdint>

// Revision of the stored site map; increments on every change and is 0 when no map has been saved.
// Reported as "smap" in /jo and /ja so apps can detect support and notice changes.
uint32_t sitemap_revision();
