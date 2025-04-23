/**
 * =============================================================================
 * MultiAddonManager
 * Copyright (C) 2024 xen
 * =============================================================================
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3.0, as published by the
 * Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#define MULTIADDONMANAGER_INTERFACE "MultiAddonManager003"
class IMultiAddonManager
{
public:
	// These add/remove to the internal list without reloading anything
	// pszWorkshopID is the workshop ID in string form (e.g. "3157463861")
	virtual bool AddAddon(const char *pszWorkshopID, bool bRefresh = false) = 0;
	virtual bool RemoveAddon(const char *pszWorkshopID,  bool bRefresh = false) = 0;
	
	// Returns true if the given addon is mounted in the filesystem. 
	// Pass 'true' to bCheckWorkshopMap to check from the server mounted workshop map as well.
	virtual bool IsAddonMounted(const char *pszWorkshopID, bool bCheckWorkshopMap = false) = 0;

	// Start an addon download of the given workshop ID
	// Returns true if the download successfully started or the addon already exists, and false otherwise
	// bImportant: If set, the map will be reloaded once the download finishes 
	// bForce: If set, will start the download even if the addon already exists
	virtual bool DownloadAddon(const char *pszWorkshopID, bool bImportant = false, bool bForce = true) = 0;

	// Refresh addons, applying any changes from add/remove
	// This will trigger a map reload once all addons are updated and mounted
	virtual void RefreshAddons(bool bReloadMap = false) = 0;

	// Clear the internal list and unmount all addons excluding the current workshop map
	virtual void ClearAddons() = 0;
	
	// Check whether the server is connected to the game coordinator, and therefore is capable of downloading addons.
	// Should be called before calling DownloadAddon.
	virtual bool HasGCConnection() = 0;
	
	// Functions to manage addons to be loaded only by a client. 
	// Pass an xuid value of 0 to perform the operation on a global list instead, and bRefresh to 'true' to trigger a reconnect if necessary.
	virtual void AddClientAddon(const char *pszAddon, uint64 xuid = 0, bool bRefresh = false) = 0;
	virtual void RemoveClientAddon(const char *pszAddon, uint64 xuid = 0) = 0;
	virtual void ClearClientAddons(uint64 xuid = 0) = 0;
};
