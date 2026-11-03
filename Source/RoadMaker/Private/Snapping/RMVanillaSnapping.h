#pragma once

namespace RMVanillaSnapping
{
	/**
	 * Hooks game holograms so they cooperate with roads:
	 *  - power poles (all tiers) and street lights: with the snap key (Ctrl) held they slide along the
	 *    road's fence lines, like signals along a railway. Street lights face the road; scrolling
	 *    turns them to face away.
	 *  - railways and vehicle paths: their points snap to the centre of a road end.
	 * Works by handing the hologram an adjusted hit result, so the game's own placement logic
	 * (wires, clearance, construction) still runs.
	 */
	void InstallHooks();
}
