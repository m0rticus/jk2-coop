/*
===========================================================================
Copyright (C) 2026 OpenJK contributors

This file is part of the OpenJK source code.

OpenJK is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.
===========================================================================
*/

#pragma once

void G_Coop_Init( void );
void G_Coop_RunFrame( void );
qboolean G_Coop_IsEnabled( void );
int G_Coop_MaxClients( void );
qboolean G_Coop_IsPeerClient( int clientNum );
void G_Coop_AdjustPeerSpawn( gentity_t *ent, vec3_t spawnOrigin );
