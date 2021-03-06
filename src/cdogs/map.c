/*
    C-Dogs SDL
    A port of the legendary (and fun) action/arcade cdogs.
    Copyright (C) 1995 Ronny Wester
    Copyright (C) 2003 Jeremy Chin
    Copyright (C) 2003-2007 Lucas Martin-King

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

    This file incorporates work covered by the following copyright and
    permission notice:

    Copyright (c) 2013-2017 Cong Xu
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:

    Redistributions of source code must retain the above copyright notice, this
    list of conditions and the following disclaimer.
    Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions and the following disclaimer in the documentation
    and/or other materials provided with the distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
    AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
    ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
    LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
    CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
    SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
    INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
    CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
    ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
    POSSIBILITY OF SUCH DAMAGE.
*/
#include "map.h"

#include <assert.h>
#include <string.h>
#include <stdlib.h>

#include "algorithms.h"
#include "ammo.h"
#include "collision/collision.h"
#include "config.h"
#include "door.h"
#include "game_events.h"
#include "gamedata.h"
#include "log.h"
#include "los.h"
#include "map_build.h"
#include "map_cave.h"
#include "map_classic.h"
#include "map_static.h"
#include "net_util.h"
#include "pic_manager.h"
#include "pickup.h"
#include "objs.h"
#include "sounds.h"
#include "actors.h"
#include "mission.h"
#include "utils.h"

#define KEY_W 9
#define KEY_H 5
#define COLLECTABLE_W 4
#define COLLECTABLE_H 3

Map gMap;


const char *IMapTypeStr(IMapType t)
{
	switch (t)
	{
		T2S(MAP_FLOOR, "Floor");
		T2S(MAP_WALL, "Wall");
		T2S(MAP_DOOR, "Door");
		T2S(MAP_ROOM, "Room");
		T2S(MAP_NOTHING, "Nothing");
		T2S(MAP_SQUARE, "Square");
	default:
		return "";
	}
}
IMapType StrIMapType(const char *s)
{
	S2T(MAP_FLOOR, "Floor");
	S2T(MAP_WALL, "Wall");
	S2T(MAP_DOOR, "Door");
	S2T(MAP_ROOM, "Room");
	S2T(MAP_NOTHING, "Nothing");
	S2T(MAP_SQUARE, "Square");
	return MAP_FLOOR;
}


unsigned short GetAccessMask(int k)
{
	if (k == -1)
	{
		return 0;
	}
	return MAP_ACCESS_YELLOW << k;
}

Tile *MapGetTile(const Map *map, const struct vec2i pos)
{
	if (pos.x < 0 || pos.x >= map->Size.x || pos.y < 0 || pos.y >= map->Size.y)
	{
		return NULL;
	}
	return CArrayGet(&map->Tiles, pos.y * map->Size.x + pos.x);
}

bool MapIsTileIn(const Map *map, const struct vec2i pos)
{
	// Check that the tile pos is within the interior of the map
	return !(pos.x < 0 || pos.y < 0 ||
		pos.x > map->Size.x - 1 || pos.y > map->Size.y - 1);
}
static bool MapIsPosIn(const Map *map, const struct vec2 pos)
{
	// Check that the pos is within the interior of the map
	return MapIsTileIn(map, Vec2ToTile(pos));
}

bool MapIsTileInExit(const Map *map, const TTileItem *ti)
{
	const struct vec2i tilePos = Vec2ToTile(ti->Pos);
	return
		tilePos.x >= map->ExitStart.x && tilePos.x <= map->ExitEnd.x &&
		tilePos.y >= map->ExitStart.y && tilePos.y <= map->ExitEnd.y;
}

static Tile *MapGetTileOfItem(Map *map, TTileItem *t)
{
	const struct vec2i pos = Vec2ToTile(t->Pos);
	return MapGetTile(map, pos);
}

static void AddItemToTile(TTileItem *t, Tile *tile);
bool MapTryMoveTileItem(Map *map, TTileItem *t, const struct vec2 pos)
{
	// Check if we can move to new position
	if (!MapIsPosIn(map, pos))
	{
		return false;
	}
	// When first initialised, position is -1
	const bool doRemove = t->Pos.x >= 0 && t->Pos.y >= 0;
	const struct vec2i t1 = Vec2ToTile(t->Pos);
	const struct vec2i t2 = Vec2ToTile(pos);
	// If we'll be in the same tile, do nothing
	if (svec2i_is_equal(t1, t2) && doRemove)
	{
		t->Pos = pos;
		return true;
	}
	// Moving; remove from old tile...
	if (doRemove)
	{
		MapRemoveTileItem(map, t);
	}
	// ...move and add to new tile
	t->Pos = pos;
	AddItemToTile(t, MapGetTile(map, t2));
	return true;
}
static void AddItemToTile(TTileItem *t, Tile *tile)
{
	ThingId tid;
	tid.Id = t->id;
	tid.Kind = t->kind;
	CASSERT(tid.Id >= 0, "invalid ThingId");
	CASSERT(tid.Kind >= 0 && tid.Kind <= KIND_PICKUP, "unknown thing kind");
	CArrayPushBack(&tile->things, &tid);
}

void MapRemoveTileItem(Map *map, TTileItem *t)
{
	if (!MapIsPosIn(map, t->Pos))
	{
		return;
	}
	Tile *tile = MapGetTileOfItem(map, t);
	CA_FOREACH(ThingId, tid, tile->things)
		if (tid->Id == t->id && tid->Kind == t->kind)
		{
			CArrayDelete(&tile->things, _ca_index);
			return;
		}
	CA_FOREACH_END()
	CASSERT(false, "Did not find element to delete");
}

struct vec2i MapGetRandomTile(const Map *map)
{
	return svec2i(rand() % map->Size.x, rand() % map->Size.y);
}

struct vec2 MapGetRandomPos(const Map *map)
{
	return svec2(
		RAND_FLOAT(0, map->Size.x * TILE_WIDTH),
		RAND_FLOAT(0, map->Size.y * TILE_HEIGHT));
}

unsigned short IMapGet(const Map *map, const struct vec2i pos)
{
	if (pos.x < 0 || pos.x >= map->Size.x || pos.y < 0 || pos.y >= map->Size.y)
	{
		return MAP_NOTHING;
	}
	return *(unsigned short *)CArrayGet(
		&map->iMap, pos.y * map->Size.x + pos.x);
}
void IMapSet(Map *map, struct vec2i pos, unsigned short v)
{
	*(unsigned short *)CArrayGet(&map->iMap, pos.y * map->Size.x + pos.x) = v;
}

void MapChangeFloor(
	Map *map, const struct vec2i pos, NamedPic *normal, NamedPic *shadow)
{
	Tile *tAbove = MapGetTile(map, svec2i(pos.x, pos.y - 1));
	int canSeeTileAbove = !(pos.y > 0 && !TileCanSee(tAbove));
	Tile *t = MapGetTile(map, pos);
	switch (IMapGet(map, pos) & MAP_MASKACCESS)
	{
	case MAP_FLOOR:
	case MAP_SQUARE:
	case MAP_ROOM:
		if (!canSeeTileAbove)
		{
			t->pic = shadow;
		}
		else
		{
			t->pic = normal;
		}
		break;
	default:
		// do nothing
		break;
	}
}

void MapShowExitArea(Map *map, const struct vec2i exitStart, const struct vec2i exitEnd)
{
	const int left = exitStart.x;
	const int right = exitEnd.x;
	const int top = exitStart.y;
	const int bottom = exitEnd.y;

	NamedPic *exitPic = PicManagerGetExitPic(
		&gPicManager, gMission.missionData->ExitStyle, false);
	NamedPic *exitShadowPic = PicManagerGetExitPic(
		&gPicManager, gMission.missionData->ExitStyle, true);

	struct vec2i v;
	v.y = top;
	for (v.x = left; v.x <= right; v.x++)
	{
		MapChangeFloor(map, v, exitPic, exitShadowPic);
	}
	v.y = bottom;
	for (v.x = left; v.x <= right; v.x++)
	{
		MapChangeFloor(map, v, exitPic, exitShadowPic);
	}
	v.x = left;
	for (v.y = top + 1; v.y < bottom; v.y++)
	{
		MapChangeFloor(map, v, exitPic, exitShadowPic);
	}
	v.x = right;
	for (v.y = top + 1; v.y < bottom; v.y++)
	{
		MapChangeFloor(map, v, exitPic, exitShadowPic);
	}
}

struct vec2 MapGetExitPos(const Map *m)
{
	return svec2_assign_vec2i(Vec2iCenterOfTile(
		svec2i_scale_divide(svec2i_add(m->ExitStart, m->ExitEnd), 2)));
}

// Adjacent means to the left, right, above or below
static int MapGetNumWallsAdjacentTile(Map *map, struct vec2i v)
{
	int count = 0;
	if (v.x > 0 && v.y > 0 && v.x < map->Size.x - 1 && v.y < map->Size.y - 1)
	{
		if (!TileCanWalk(MapGetTile(map, svec2i(v.x - 1, v.y))))
		{
			count++;
		}
		if (!TileCanWalk(MapGetTile(map, svec2i(v.x + 1, v.y))))
		{
			count++;
		}
		if (!TileCanWalk(MapGetTile(map, svec2i(v.x, v.y - 1))))
		{
			count++;
		}
		if (!TileCanWalk(MapGetTile(map, svec2i(v.x, v.y + 1))))
		{
			count++;
		}
	}
	return count;
}

// Around means the 8 tiles surrounding the tile
static int MapGetNumWallsAroundTile(Map *map, struct vec2i v)
{
	int count = MapGetNumWallsAdjacentTile(map, v);
	if (v.x > 0 && v.y > 0 && v.x < map->Size.x - 1 && v.y < map->Size.y - 1)
	{
		// Having checked the adjacencies, check the diagonals
		if (!TileCanWalk(MapGetTile(map, svec2i(v.x - 1, v.y - 1))))
		{
			count++;
		}
		if (!TileCanWalk(MapGetTile(map, svec2i(v.x + 1, v.y + 1))))
		{
			count++;
		}
		if (!TileCanWalk(MapGetTile(map, svec2i(v.x + 1, v.y - 1))))
		{
			count++;
		}
		if (!TileCanWalk(MapGetTile(map, svec2i(v.x - 1, v.y + 1))))
		{
			count++;
		}
	}
	return count;
}

bool MapTryPlaceOneObject(
	Map *map, const struct vec2i v, const MapObject *mo, const int extraFlags,
	const bool isStrictMode)
{
	// Don't place ammo spawners if ammo is disabled
	if (!ConfigGetBool(&gConfig, "Game.Ammo") &&
		mo->Type == MAP_OBJECT_TYPE_PICKUP_SPAWNER &&
		mo->u.PickupClass->Type == PICKUP_AMMO)
	{
		return false;
	}
	const Tile *t = MapGetTile(map, v);
	unsigned short iMap = IMapGet(map, v);

	const bool isEmpty = TileIsClear(t);
	if (isStrictMode && !MapObjectIsTileOKStrict(
			mo, iMap, isEmpty,
			IMapGet(map, svec2i(v.x, v.y - 1)),
			IMapGet(map, svec2i(v.x, v.y + 1)),
			MapGetNumWallsAdjacentTile(map, v),
			MapGetNumWallsAroundTile(map, v)))
	{
		return 0;
	}
	else if (!MapObjectIsTileOK(
		mo, iMap, isEmpty, IMapGet(map, svec2i(v.x, v.y - 1))))
	{
		return 0;
	}

	if (mo->Flags & (1 << PLACEMENT_FREE_IN_FRONT))
	{
		IMapSet(map, svec2i(v.x, v.y + 1), IMapGet(map, svec2i(v.x, v.y + 1)) | MAP_LEAVEFREE);
	}

	NMapObjectAdd amo = NMapObjectAdd_init_default;
	amo.UID = ObjsGetNextUID();
	strcpy(amo.MapObjectClass, mo->Name);
	amo.Pos = Vec2ToNet(MapObjectGetPlacementPos(mo, v));
	amo.TileItemFlags = MapObjectGetFlags(mo) | extraFlags;
	amo.Health = mo->Health;
	ObjAdd(amo);
	return true;
}

bool MapHasLockedRooms(const Map *map)
{
	return map->keyAccessCount > 1;
}

static bool MapTileIsInLockedRoom(const Map *map, const struct vec2i tilePos)
{
	return IMapGet(map, tilePos) & MAP_ACCESSBITS;
}

bool MapPosIsInLockedRoom(const Map *map, const struct vec2 pos)
{
	const struct vec2i tilePos = Vec2ToTile(pos);
	return MapTileIsInLockedRoom(map, tilePos);
}

void MapPlaceCollectible(
	const struct MissionOptions *mo, const int objective, const struct vec2 pos)
{
	const Objective *o = CArrayGet(&mo->missionData->Objectives, objective);
	GameEvent e = GameEventNew(GAME_EVENT_ADD_PICKUP);
	e.u.AddPickup.UID = PickupsGetNextUID();
	strcpy(e.u.AddPickup.PickupClass, o->u.Pickup->Name);
	e.u.AddPickup.IsRandomSpawned = false;
	e.u.AddPickup.SpawnerUID = -1;
	e.u.AddPickup.TileItemFlags = ObjectiveToTileItem(objective);
	e.u.AddPickup.Pos = Vec2ToNet(pos);
	GameEventsEnqueue(&gGameEvents, e);
}
static int MapTryPlaceCollectible(
	Map *map, const Mission *mission, const struct MissionOptions *mo,
	const int objective)
{
	const Objective *o = CArrayGet(&mission->Objectives, objective);
	const bool hasLockedRooms =
		(o->Flags & OBJECTIVE_HIACCESS) && MapHasLockedRooms(map);
	const bool noaccess = o->Flags & OBJECTIVE_NOACCESS;
	int i = (noaccess || hasLockedRooms) ? 1000 : 100;

	while (i)
	{
		const struct vec2 v = MapGetRandomPos(map);
		const struct vec2i size = svec2i(COLLECTABLE_W, COLLECTABLE_H);
		if (!IsCollisionWithWall(v, size))
		{
			if ((!hasLockedRooms || MapPosIsInLockedRoom(map, v)) &&
				(!noaccess || !MapPosIsInLockedRoom(map, v)))
			{
				MapPlaceCollectible(mo, objective, v);
				return 1;
			}
		}
		i--;
	}
	return 0;
}

struct vec2 MapGenerateFreePosition(Map *map, const struct vec2i size)
{
	for (int i = 0; i < 100; i++)
	{
		const struct vec2 v = MapGetRandomPos(map);
		if (!IsCollisionWithWall(v, size))
		{
			return v;
		}
	}
	return svec2_zero();
}

typedef struct
{
	const Objective *o;
	int objective;
} TryPlaceOneBlowupData;
static bool TryPlaceOneBlowup(Map *map, const struct vec2i tilePos, void *data);
static bool MapTryPlaceBlowup(
	Map *map, const Mission *mission, const int objective)
{
	TryPlaceOneBlowupData data;
	data.o = CArrayGet(&mission->Objectives, objective);
	const PlacementAccessFlags paFlags =
		ObjectiveGetPlacementAccessFlags(data.o);
	data.objective = objective;
	return MapPlaceRandomTile(map, paFlags, TryPlaceOneBlowup, &data);
}
static bool TryPlaceOneBlowup(Map *map, const struct vec2i tilePos, void *data)
{
	const TryPlaceOneBlowupData *pData = data;
	return MapTryPlaceOneObject(
		map, tilePos, pData->o->u.MapObject,
		ObjectiveToTileItem(pData->objective), true);
}

void MapPlaceKey(
	Map *map, const struct MissionOptions *mo, const struct vec2i tilePos,
	const int keyIndex)
{
	UNUSED(map);
	GameEvent e = GameEventNew(GAME_EVENT_ADD_PICKUP);
	e.u.AddPickup.UID = PickupsGetNextUID();
	strcpy(
		e.u.AddPickup.PickupClass,
		KeyPickupClass(mo->missionData->KeyStyle, keyIndex)->Name);
	e.u.AddPickup.IsRandomSpawned = false;
	e.u.AddPickup.SpawnerUID = -1;
	e.u.AddPickup.TileItemFlags = 0;
	e.u.AddPickup.Pos = Vec2ToNet(Vec2CenterOfTile(tilePos));
	GameEventsEnqueue(&gGameEvents, e);
}

static int GetPlacementRetries(
	const Map *map, const PlacementAccessFlags paFlags,
	bool *locked, bool *unlocked)
{
	// Try more times if we need to place in a locked room or unlocked place
	*locked = paFlags == PLACEMENT_ACCESS_LOCKED && MapHasLockedRooms(map);
	*unlocked = paFlags == PLACEMENT_ACCESS_NOT_LOCKED;
	return (*locked || *unlocked) ? 1000 : 100;
}

bool MapPlaceRandomTile(
	Map *map, const PlacementAccessFlags paFlags,
	bool (*tryPlaceFunc)(Map *, const struct vec2i, void *), void *data)
{
	// Try a bunch of times to place something on a random tile
	bool locked, unlocked;
	const int retries = GetPlacementRetries(map, paFlags, &locked, &unlocked);
	for (int i = 0; i < retries; i++)
	{
		const struct vec2i tilePos = MapGetRandomTile(map);
		const bool isInLocked = MapTileIsInLockedRoom(map, tilePos);
		if ((!locked || isInLocked) && (!unlocked || !isInLocked))
		{
			if (tryPlaceFunc(map, tilePos, data))
			{
				return true;
			}
		}
	}
	return false;
}
bool MapPlaceRandomPos(
	Map *map, const PlacementAccessFlags paFlags,
	bool (*tryPlaceFunc)(Map *, const struct vec2, void *), void *data)
{
	// Try a bunch of times to place something at a random location
	bool locked, unlocked;
	const int retries = GetPlacementRetries(map, paFlags, &locked, &unlocked);
	for (int i = 0; i < retries; i++)
	{
		const struct vec2 v = MapGetRandomPos(map);
		const bool isInLocked = MapPosIsInLockedRoom(map, v);
		if ((!locked || isInLocked) && (!unlocked || !isInLocked))
		{
			if (tryPlaceFunc(map, v, data))
			{
				return true;
			}
		}
	}
	return false;
}

static void MapPlaceCard(Map *map, int keyIndex, int map_access)
{
	for (;;)
	{
		const struct vec2i v = MapGetRandomTile(map);
		Tile *t;
		Tile *tBelow;
		unsigned short iMap;
		t = MapGetTile(map, v);
		iMap = IMapGet(map, v);
		tBelow = MapGetTile(map, svec2i(v.x, v.y + 1));
		if (TileIsClear(t) &&
			(iMap & 0xF00) == map_access &&
			(iMap & MAP_MASKACCESS) == MAP_ROOM &&
			TileIsClear(tBelow))
		{
			MapPlaceKey(map, &gMission, v, keyIndex);
			return;
		}
	}
}

static int AccessCodeToFlags(int code)
{
	if (code & MAP_ACCESS_RED)
		return FLAGS_KEYCARD_RED;
	if (code & MAP_ACCESS_BLUE)
		return FLAGS_KEYCARD_BLUE;
	if (code & MAP_ACCESS_GREEN)
		return FLAGS_KEYCARD_GREEN;
	if (code & MAP_ACCESS_YELLOW)
		return FLAGS_KEYCARD_YELLOW;
	return 0;
}

static int MapGetAccessLevel(Map *map, int x, int y)
{
	return AccessCodeToFlags(IMapGet(map, svec2i(x, y)));
}

// Need to check the flags around the door tile because it's the
// triggers that contain the right flags
// TODO: refactor door
int MapGetDoorKeycardFlag(Map *map, struct vec2i pos)
{
	int l = MapGetAccessLevel(map, pos.x, pos.y);
	if (l) return l;
	l = MapGetAccessLevel(map, pos.x - 1, pos.y);
	if (l) return l;
	l = MapGetAccessLevel(map, pos.x + 1, pos.y);
	if (l) return l;
	l = MapGetAccessLevel(map, pos.x, pos.y - 1);
	if (l) return l;
	return MapGetAccessLevel(map, pos.x, pos.y + 1);
}

static int MapGetAccessFlags(Map *map, int x, int y)
{
	int flags = 0;
	flags = MAX(flags, AccessCodeToFlags(IMapGet(map, svec2i(x, y))));
	flags = MAX(flags, AccessCodeToFlags(IMapGet(map, svec2i(x - 1, y))));
	flags = MAX(flags, AccessCodeToFlags(IMapGet(map, svec2i(x + 1, y))));
	flags = MAX(flags, AccessCodeToFlags(IMapGet(map, svec2i(x, y - 1))));
	flags = MAX(flags, AccessCodeToFlags(IMapGet(map, svec2i(x, y + 1))));
	return flags;
}

static void MapSetupDoors(Map *map, const Mission *m)
{
	struct vec2i v;
	for (v.x = 0; v.x < map->Size.x; v.x++)
	{
		for (v.y = 0; v.y < map->Size.y; v.y++)
		{
			// Check if this is the start of a door group
			// Top or left-most door
			if ((IMapGet(map, v) & MAP_MASKACCESS) == MAP_DOOR &&
				(IMapGet(map, svec2i(v.x - 1, v.y)) & MAP_MASKACCESS) != MAP_DOOR &&
				(IMapGet(map, svec2i(v.x, v.y - 1)) & MAP_MASKACCESS) != MAP_DOOR)
			{
				MapAddDoorGroup(map, m, v, MapGetAccessFlags(map, v.x, v.y));
			}
		}
	}
}

void MapTerminate(Map *map)
{
	CA_FOREACH(Trigger *, t, map->triggers)
		TriggerTerminate(*t);
	CA_FOREACH_END()
	CArrayTerminate(&map->triggers);
	struct vec2i v;
	for (v.y = 0; v.y < map->Size.y; v.y++)
	{
		for (v.x = 0; v.x < map->Size.x; v.x++)
		{
			Tile *t = MapGetTile(map, v);
			TileDestroy(t);
		}
	}
	CArrayTerminate(&map->Tiles);
	CArrayTerminate(&map->iMap);
	LOSTerminate(&map->LOS);
	PathCacheTerminate(&gPathCache);
}

static void DebugPrintMap(const Map *map);
void MapLoad(
	Map *map, const struct MissionOptions *mo, const CampaignOptions *co)
{
	MapTerminate(map);

	// Init map
	memset(map, 0, sizeof *map);
	CArrayInit(&map->Tiles, sizeof(Tile));
	CArrayInit(&map->iMap, sizeof(unsigned short));
	const Mission *mission = mo->missionData;
	map->Size = mission->Size;
	LOSInit(map, map->Size);
	CArrayInit(&map->triggers, sizeof(Trigger *));
	PathCacheInit(&gPathCache, map);

	struct vec2i v;
	for (v.y = 0; v.y < map->Size.y; v.y++)
	{
		for (v.x = 0; v.x < map->Size.x; v.x++)
		{
			Tile t;
			unsigned short tI = MAP_FLOOR;
			TileInit(&t);
			CArrayPushBack(&map->Tiles, &t);
			CArrayPushBack(&map->iMap, &tI);
		}
	}

	switch (mission->Type)
	{
	case MAPTYPE_CLASSIC:
		MapClassicLoad(map, mission, co);
		break;
	case MAPTYPE_STATIC:
		MapStaticLoad(map, mo);
		break;
	case MAPTYPE_CAVE:
		MapCaveLoad(map, mo, co);
		break;
	default:
		CASSERT(false, "unknown map type");
		break;
	}

	DebugPrintMap(map);

	MapSetupTilesAndWalls(map, mission);
	MapSetupDoors(map, mission);

	if (mission->Type == MAPTYPE_CLASSIC)
	{
		// Randomly add drainage tiles for classic map type;
		// For other map types drains are regular map objects
		const MapObject *drain = StrMapObject("drain0");
		for (int i = 0; i < map->Size.x*map->Size.y / 45; i++)
		{
			// Make sure drain tiles aren't next to each other
			v = MapGetRandomTile(map);
			v.x &= 0xFFFFFE;
			v.y &= 0xFFFFFE;
			const Tile *t = MapGetTile(map, v);
			if (TileIsNormalFloor(t))
			{
				MapTryPlaceOneObject(map, v, drain, 0, false);
			}
		}
	}

	// Set exit now since we have set up all the tiles
	if (svec2i_is_zero(map->ExitStart) && svec2i_is_zero(map->ExitEnd))
	{
		MapGenerateRandomExitArea(map);
	}

	// Count total number of reachable tiles, for explored %
	map->NumExplorableTiles = 0;
	for (v.y = 0; v.y < map->Size.y; v.y++)
	{
		for (v.x = 0; v.x < map->Size.x; v.x++)
		{
			if (!(MapGetTile(map, v)->flags & MAPTILE_NO_WALK))
			{
				map->NumExplorableTiles++;
			}
		}
	}
}
static void DebugPrintMap(const Map *map)
{
	if (LogModuleGetLevel(LM_MAP) > LL_TRACE)
	{
		return;
	}
	char *buf;
	CCALLOC(buf, map->Size.x + 1);
	char *bufP = buf;
	struct vec2i v;
	for (v.y = 0; v.y < map->Size.y; v.y++)
	{
		for (v.x = 0; v.x < map->Size.x; v.x++)
		{
			switch (IMapGet(map, v) & MAP_MASKACCESS)
			{
			case MAP_FLOOR:
				*bufP++ = '.';
				break;
			case MAP_WALL:
				*bufP++ = '#';
				break;
			case MAP_DOOR:
				*bufP++ = '+';
				break;
			case MAP_ROOM:
				*bufP++ = '-';
				break;
			case MAP_NOTHING:
				*bufP++ = ' ';
				break;
			case MAP_SQUARE:
				*bufP++ = '_';
				break;
			default:
				*bufP++ = '?';
				break;
			}
		}
		LOG(LM_MAP, LL_TRACE, buf);
		*buf = '\0';
		bufP = buf;
	}
	CFREE(buf);
}

static void AddObjectives(Map *map, const struct MissionOptions *mo);
static void AddKeys(Map *map);
void MapLoadDynamic(
	Map *map, const struct MissionOptions *mo, const CharacterStore *store)
{
	const Mission *mission = mo->missionData;

	if (mission->Type == MAPTYPE_STATIC)
	{
		MapStaticLoadDynamic(map, mo, store);
	}

	// Add map objects
	CA_FOREACH(const MapObjectDensity, mod, mission->MapObjectDensities)
		for (int j = 0;
			j < (mod->Density * map->Size.x * map->Size.y) / 1000;
			j++)
		{
			MapTryPlaceOneObject(map, MapGetRandomTile(map), mod->M, 0, true);
		}
	CA_FOREACH_END()

	if (HasObjectives(gCampaign.Entry.Mode))
	{
		AddObjectives(map, mo);
	}

	if (AreKeysAllowed(gCampaign.Entry.Mode))
	{
		AddKeys(map);
	}
}
static void AddObjectives(Map *map, const struct MissionOptions *mo)
{
	// Try to add the objectives
	// If we are unable to place them all, make sure to reduce the totals
	// in case we create missions that are impossible to complete
	CA_FOREACH(Objective, o, mo->missionData->Objectives)
		if (o->Type != OBJECTIVE_COLLECT && o->Type != OBJECTIVE_DESTROY)
		{
			continue;
		}
		if (o->Type == OBJECTIVE_COLLECT)
		{
			for (int i = o->placed; i < o->Count; i++)
			{
				if (MapTryPlaceCollectible(
					map, mo->missionData, mo, _ca_index))
				{
					o->placed++;
				}
			}
		}
		else if (o->Type == OBJECTIVE_DESTROY)
		{
			for (int i = o->placed; i < o->Count; i++)
			{
				if (MapTryPlaceBlowup(map, mo->missionData, _ca_index))
				{
					o->placed++;
				}
			}
		}
		o->Count = o->placed;
		if (o->Count < o->Required)
		{
			o->Required = o->Count;
		}
	CA_FOREACH_END()
}
static void AddKeys(Map *map)
{
	if (map->keyAccessCount >= 5)
	{
		MapPlaceCard(map, 3, MAP_ACCESS_BLUE);
	}
	if (map->keyAccessCount >= 4)
	{
		MapPlaceCard(map, 2, MAP_ACCESS_GREEN);
	}
	if (map->keyAccessCount >= 3)
	{
		MapPlaceCard(map, 1, MAP_ACCESS_YELLOW);
	}
	if (map->keyAccessCount >= 2)
	{
		MapPlaceCard(map, 0, 0);
	}
}

bool MapIsPosOKForPlayer(
	const Map *map, const struct vec2 pos, const bool allowAllTiles)
{
	const struct vec2i tilePos = Vec2ToTile(pos);
	unsigned short tile = IMapGet(map, tilePos);
	if (tile == MAP_FLOOR)
	{
		return true;
	}
	else if (allowAllTiles)
	{
		return tile == MAP_SQUARE || tile == MAP_ROOM;
	}
	return false;
}

// Check if the target position is completely clear
// This includes collisions that make the target illegal, such as walls
// But it also includes item collisions, whether or not the collisions
// are legal, e.g. item pickups, friendly collisions
bool MapIsTileAreaClear(Map *map, const struct vec2 pos, const struct vec2i size)
{
	// Wall collision
	if (IsCollisionWithWall(pos, size))
	{
		return false;
	}

	// Item collision
	const struct vec2i tv = Vec2ToTile(pos);
	struct vec2i dv;
	// Check collisions with all other items on this tile, in all 8 directions
	for (dv.y = -1; dv.y <= 1; dv.y++)
	{
		for (dv.x = -1; dv.x <= 1; dv.x++)
		{
			const struct vec2i dtv = svec2i_add(tv, dv);
			if (!MapIsTileIn(map, dtv))
			{
				continue;
			}
			const CArray *tileThings = &MapGetTile(map, dtv)->things;
			if (tileThings == NULL)
			{
				continue;
			}
			for (int i = 0; i < (int)tileThings->size; i++)
			{
				const TTileItem *ti =
					ThingIdGetTileItem(CArrayGet(tileThings, i));
				if (AABBOverlap(pos, ti->Pos, size, ti->size))
				{
					return false;
				}
			}
		}
	}

	return true;
}

void MapMarkAsVisited(Map *map, struct vec2i pos)
{
	Tile *t = MapGetTile(map, pos);
	if (!t->isVisited && !(t->flags & MAPTILE_NO_WALK))
	{
		map->tilesSeen++;
	}
	t->isVisited = true;
}

void MapMarkAllAsVisited(Map *map)
{
	struct vec2i pos;
	for (pos.y = 0; pos.y < map->Size.y; pos.y++)
	{
		for (pos.x = 0; pos.x < map->Size.x; pos.x++)
		{
			MapMarkAsVisited(map, pos);
		}
	}
}

int MapGetExploredPercentage(Map *map)
{
	return (100 * map->tilesSeen) / map->NumExplorableTiles;
}

struct vec2i MapSearchTileAround(Map *map, struct vec2i start, TileSelectFunc func)
{
	if (func(map, start))
	{
		return start;
	}
	// Search using an expanding box pattern around the goal
	for (int radius = 1; radius < MAX(map->Size.x, map->Size.y); radius++)
	{
		struct vec2i tile;
		for (tile.x = start.x - radius;
			tile.x <= start.x + radius;
			tile.x++)
		{
			if (tile.x < 0) continue;
			if (tile.x >= map->Size.x) break;
			for (tile.y = start.y - radius;
				tile.y <= start.y + radius;
				tile.y++)
			{
				if (tile.y < 0) continue;
				if (tile.y >= map->Size.y) break;
				// Check box; don't check inside
				if (tile.x != start.x - radius &&
					tile.x != start.x + radius &&
					tile.y != start.y - radius &&
					tile.y != start.y + radius)
				{
					continue;
				}
				if (func(map, tile))
				{
					return tile;
				}
			}
		}
	}
	// Should never reach this point; something is very wrong
	CASSERT(false, "failed to find tile around tile");
	return svec2i_zero();
}
bool MapTileIsUnexplored(Map *map, struct vec2i tile)
{
	const Tile *t = MapGetTile(map, tile);
	return !t->isVisited && !(t->flags & MAPTILE_NO_WALK);
}

// Only creates the trigger, but does not place it
Trigger *MapNewTrigger(Map *map)
{
	Trigger *t = TriggerNew();
	CArrayPushBack(&map->triggers, &t);
	t->id = map->triggerId++;
	return t;
}
