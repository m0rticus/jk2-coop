/*
===========================================================================
Copyright (C) 2026 OpenJK contributors

This file is part of the OpenJK source code.

OpenJK is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.
===========================================================================
*/

#include "g_local.h"
#include "g_coop.h"

extern char *ClientConnect( int clientNum, qboolean firstTime, SavedGameJustLoaded_e eSavedGameJustLoaded );
extern void ClientBegin( int clientNum, usercmd_t *cmd, SavedGameJustLoaded_e eSavedGameJustLoaded );

static qboolean s_coopEnabled = qfalse;
static qboolean s_peerClientStarted = qfalse;

void G_Coop_Init( void )
{
	s_coopEnabled = gi.Cvar_VariableIntegerValue( "cl_coopEnabled" ) ? qtrue : qfalse;
	s_peerClientStarted = qfalse;

	if ( s_coopEnabled )
	{
		gi.Printf( "Co-op: enabling second SP client slot\n" );
	}
}

qboolean G_Coop_IsEnabled( void )
{
	return s_coopEnabled;
}

int G_Coop_MaxClients( void )
{
	return s_coopEnabled ? MAX_CLIENTS : 1;
}

qboolean G_Coop_IsPeerClient( int clientNum )
{
	return ( s_coopEnabled && clientNum == 1 ) ? qtrue : qfalse;
}

static qboolean G_Coop_IsSpawnOriginClear( gentity_t *ent, const vec3_t origin )
{
	trace_t tr;

	gi.trace( &tr, origin, ent->mins, ent->maxs, origin, ent->s.number, MASK_PLAYERSOLID, G2_NOCOLLIDE, 0 );
	return ( !tr.startsolid && !tr.allsolid ) ? qtrue : qfalse;
}

void G_Coop_AdjustPeerSpawn( gentity_t *ent, vec3_t spawnOrigin )
{
	if ( !ent || !ent->client || !G_Coop_IsPeerClient( ent - g_entities ) )
	{
		return;
	}

	gentity_t *host = &g_entities[0];
	if ( !host->inuse || !host->client )
	{
		return;
	}

	vec3_t delta;
	VectorSubtract( spawnOrigin, host->client->ps.origin, delta );
	delta[2] = 0.0f;
	if ( VectorLength( delta ) >= 40.0f && G_Coop_IsSpawnOriginClear( ent, spawnOrigin ) )
	{
		return;
	}

	static const float yawOffsets[] = { 90.0f, -90.0f, 180.0f, 0.0f, 45.0f, -45.0f, 135.0f, -135.0f };
	static const float distances[] = { 48.0f, 64.0f, 96.0f, 128.0f };

	for ( int distanceIndex = 0; distanceIndex < ARRAY_LEN( distances ); distanceIndex++ )
	{
		for ( int yawIndex = 0; yawIndex < ARRAY_LEN( yawOffsets ); yawIndex++ )
		{
			vec3_t angles;
			vec3_t direction;
			vec3_t candidate;

			VectorSet( angles, 0.0f, host->client->ps.viewangles[YAW] + yawOffsets[yawIndex], 0.0f );
			AngleVectors( angles, direction, NULL, NULL );
			VectorMA( host->client->ps.origin, distances[distanceIndex], direction, candidate );

			if ( G_Coop_IsSpawnOriginClear( ent, candidate ) )
			{
				VectorCopy( candidate, spawnOrigin );
				gi.Printf( "Co-op: moved peer spawn beside local player\n" );
				return;
			}
		}
	}

	gi.Printf( S_COLOR_YELLOW "Co-op: could not find a clear peer spawn near the local player\n" );
}

static void G_Coop_SetPeerUserinfo( void )
{
	char userinfo[MAX_INFO_STRING] = {0};

	Info_SetValueForKey( userinfo, "name", "Co-op Player" );
	Info_SetValueForKey( userinfo, "handicap", "100" );
	Info_SetValueForKey( userinfo, "sex", "m" );
	gi.SetUserinfo( 1, userinfo );
}

static void G_Coop_StartPeerClient( void )
{
	if ( !s_coopEnabled || s_peerClientStarted || level.maxclients < MAX_CLIENTS )
	{
		return;
	}
	if ( !level.clients || level.clients[0].pers.connected != CON_CONNECTED )
	{
		return;
	}
	if ( level.clients[1].pers.connected == CON_CONNECTED )
	{
		s_peerClientStarted = qtrue;
		return;
	}

	G_Coop_SetPeerUserinfo();

	char *denied = ClientConnect( 1, qtrue, eNO );
	if ( denied )
	{
		gi.Printf( S_COLOR_YELLOW "Co-op: peer client rejected: %s\n", denied );
		s_peerClientStarted = qtrue;
		return;
	}

	usercmd_t cmd;
	memset( &cmd, 0, sizeof( cmd ) );
	cmd.serverTime = level.time;
	ClientBegin( 1, &cmd, eNO );
	s_peerClientStarted = qtrue;
	gi.Printf( "Co-op: peer client slot active\n" );
}

void G_Coop_RunFrame( void )
{
	G_Coop_StartPeerClient();
}
