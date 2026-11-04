#pragma once
// Runtime Virtual Textures (Thema 333, docs/rvt-design-2026-11-04.md): the CPU side.
//
// A landscape material is expensive per pixel (layer blend, auto rules, texture bombing). An RVT paints
// it ONCE, top-down, into small "pages" of a shared pool and the landscape then only reads the pages
// through a page table. This file is the bookkeeping, with no GPU in it:
//
//   VolumeLayout   the geometry of one volume (a square world rect cut into pages, with a mip chain)
//   RvtManager     many volumes sharing one page pool: which pages the camera needs, which of them must
//                  be (re)painted THIS frame (and nothing else), LRU eviction, dirty rects, and the
//                  page-table texels the shader samples
//
// Segmenting a landscape = one volume per tile or region, all in the same pool (global budget).
// The producer (renderer, step 2) paints exactly the PageRender list update() returns; the consumer
// (landscape shader, step 3) samples the table from pageTable().
#include <glm/vec2.hpp>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace HE::rvt
{

struct VolumeDesc
{
	glm::vec2 origin{0.0f};           // world XZ of the min corner
	float     sizeM          = 512.0f;   // edge of the square the volume must cover (metres)
	int       pageSize       = 128;      // payload texels per page side
	int       border         = 4;        // extra texels on every side, painted too, so bilinear/aniso never leave the page
	float     finestMetersPerTexel = 0.25f;   // mip 0
};

struct PageKey
{
	uint16_t volume = 0;
	uint8_t  mip    = 0;
	uint16_t x      = 0;
	uint16_t y      = 0;
	uint64_t pack() const { return (uint64_t(volume) << 40) | (uint64_t(mip) << 32) | (uint64_t(x) << 16) | y; }
	bool operator==(const PageKey& o) const { return pack() == o.pack(); }
};

// Geometry of one volume. mip 0 is the finest; the top mip is a single page for the whole volume.
class VolumeLayout
{
public:
	VolumeLayout() = default;
	explicit VolumeLayout(const VolumeDesc& d);

	const VolumeDesc& desc() const { return m_desc; }
	int      mipCount() const { return m_mipCount; }
	int      pagesPerSide(int mip) const { return m_pages0 >> mip; }
	uint64_t pageCount(int mip) const { return uint64_t(pagesPerSide(mip)) * uint64_t(pagesPerSide(mip)); }
	float    extentM() const { return m_extentM; }                 // >= desc.sizeM: pages0 is a power of two
	float    texelMeters(int mip) const { return m_desc.finestMetersPerTexel * float(1 << mip); }
	float    pageMeters(int mip) const { return texelMeters(mip) * float(m_desc.pageSize); }
	int      physicalPageSize() const { return m_desc.pageSize + 2 * m_desc.border; }

	// World rect of a page's payload / of the part that is painted (payload + border).
	void pageRect(int mip, int x, int y, glm::vec2& mn, glm::vec2& mx, bool withBorder) const;
	// The page of `mip` that contains a world point (clamped to the volume).
	void pageAt(int mip, glm::vec2 world, int& x, int& y) const;
	// The sharpest mip whose texel is not finer than `metersPerPixel` (clamped).
	int  mipForMetersPerPixel(float metersPerPixel) const;

private:
	VolumeDesc m_desc;
	int        m_pages0   = 1;
	int        m_mipCount = 1;
	float      m_extentM  = 0.0f;
};

struct ViewDesc
{
	glm::vec2 cameraXZ{0.0f};
	// Half extent (metres) around the camera that is served by mip 0. Mip m serves the square of half
	// extent nearRadiusM * 2^m, so every mip needs about the same number of pages.
	float     nearRadiusM = 64.0f;
};

// One page the producer has to paint now.
struct PageRender
{
	PageKey   key;
	uint32_t  slot = 0;              // index into the pool (array slice)
	glm::vec2 rectMin{0.0f};         // world rect to paint, border included
	glm::vec2 rectMax{0.0f};
	bool      repaint = false;       // false: first paint; true: it was resident but dirty
};

struct Stats
{
	uint32_t poolSlots     = 0;
	uint32_t resident      = 0;
	uint32_t dirty         = 0;
	uint32_t pinned        = 0;
	uint32_t renderedLastUpdate = 0;
	uint32_t wantedMissing = 0;      // still missing after the per-frame budget
	uint32_t evictions     = 0;      // total
	uint32_t starved       = 0;      // requests that found no evictable slot (pool too small for the view)
	uint64_t totalRendered = 0;
};

// No resident page / entry value: 0xFFFFFFFF. Otherwise slot | effectiveMip << 24 (slot < 2^24, mip < 32).
constexpr uint32_t kPageTableInvalid = 0xFFFFFFFFu;
inline uint32_t packPageTableEntry(uint32_t slot, int effectiveMip) { return (slot & 0xFFFFFFu) | (uint32_t(effectiveMip) << 24); }
inline uint32_t pageTableSlot(uint32_t e) { return e & 0xFFFFFFu; }
inline int      pageTableMip(uint32_t e) { return int(e >> 24); }

class RvtManager
{
public:
	// poolSlots = how many physical pages exist (budget / bytes per page). maxRendersPerFrame caps update().
	// pinnedMips: the coarsest mips of every volume that never get evicted (whole-volume fallback).
	RvtManager(uint32_t poolSlots, uint32_t maxRendersPerFrame, int pinnedMips = 2);

	uint16_t addVolume(const VolumeDesc& d);
	void     removeVolume(uint16_t id);               // frees its pages
	bool     hasVolume(uint16_t id) const { return id < m_volumes.size() && m_volumes[id].live; }
	const VolumeLayout& layout(uint16_t id) const { return m_volumes[id].layout; }

	// Everything the camera needs at this view; returns what must be painted now (<= maxRendersPerFrame,
	// no fallback chain first, then dirty/missing nearest the camera). Resident, clean, in-view pages cost nothing:
	// a frame in which nothing changed returns an empty list. The pages are assumed painted by the caller.
	std::vector<PageRender> update(uint16_t volume, const ViewDesc& view);
	void beginFrame() { ++m_frame; m_stats.renderedLastUpdate = 0; }

	// A content change in a world rect (paint stroke, sculpt, parameter): resident pages that overlap it
	// (border included) become dirty. Returns how many.
	uint32_t invalidateRect(uint16_t volume, glm::vec2 mn, glm::vec2 mx);
	uint32_t invalidateVolume(uint16_t volume);        // material recompiled, switch/parameter changed
	void     dropVolumePages(uint16_t volume);         // tile unloaded: pages are free again at once

	bool     isResident(const PageKey& k) const { return m_pages.count(k.pack()) != 0; }
	bool     isDirty(const PageKey& k) const;
	// Best resident page for (mip,x,y): the page itself or its nearest resident coarser ancestor.
	uint32_t resolve(uint16_t volume, int mip, int x, int y) const;
	// The page table of one mip, row-major (y * pagesPerSide + x), with the fallback filled in.
	std::vector<uint32_t> pageTable(uint16_t volume, int mip) const;
	// Bumped whenever residency changes (a page appeared/vanished): the GPU copy of the table is stale.
	uint64_t tableVersion(uint16_t volume) const { return m_volumes[volume].tableVersion; }

	Stats stats() const;
	uint32_t freeSlots() const { return uint32_t(m_freeSlots.size()); }

private:
	struct Page { uint32_t slot = 0; uint64_t lastUsed = 0; bool dirty = false; bool pinned = false; };
	struct Volume { VolumeLayout layout; bool live = false; uint64_t tableVersion = 0; };

	bool acquireSlot(uint32_t& slot);                 // free slot, else evict the least recently used
	bool isPinnedMip(const Volume& v, int mip) const { return mip >= v.layout.mipCount() - m_pinnedMips; }

	std::vector<Volume>                     m_volumes;
	std::unordered_map<uint64_t, Page>      m_pages;
	std::vector<uint32_t>                   m_freeSlots;
	uint32_t m_poolSlots, m_maxRenders;
	int      m_pinnedMips;
	uint64_t m_frame = 1;
	mutable Stats m_stats;
};

} // namespace HE::rvt
