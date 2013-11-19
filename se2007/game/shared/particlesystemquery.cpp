//===== Copyright � 1996-2006, Valve Corporation, All rights reserved. ======//
//
// Purpose: particle system definitions
//
//===========================================================================//

#include "cbase.h"
#include "particles/particles.h"
#include "baseparticleentity.h"
#include "entityparticletrail_shared.h"
#ifdef TF_CLIENT_DLL
#include "tf_shareddefs.h"
#endif

#ifdef GAME_DLL
#include "ai_utils.h"
#endif

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"


//-----------------------------------------------------------------------------
// Interface to allow the particle system to call back into the game code
//-----------------------------------------------------------------------------
class CParticleSystemQuery : public CBaseAppSystem< IParticleSystemQuery >
{
public:
	// Inherited from IParticleSystemQuery
	virtual void GetLightingAtPoint( const Vector& vecOrigin, Color &cTint );
	virtual void TraceLine( const Vector& vecAbsStart,
							const Vector& vecAbsEnd, unsigned int mask, 
							const IHandleEntity *ignore,
							int collisionGroup, CBaseTrace *ptr );

	virtual bool MovePointInsideControllingObject( CParticleCollection *pParticles,
												   void *pObject,
												   Vector *pPnt );
	virtual void GetRandomPointsOnControllingObjectHitBox( 
		CParticleCollection *pParticles,
		int nControlPointNumber, 
		int nNumPtsOut,
		float flBBoxScale,
		int nNumTrysToGetAPointInsideTheModel,
		Vector *pPntsOut,
		Vector vecDirectionalBias,
		Vector *pHitBoxRelativeCoordOut,
		int *pHitBoxIndexOut
		);

	virtual int GetCollisionGroupFromName( const char *pszCollisionGroupName );


	virtual int GetControllingObjectHitBoxInfo(
		CParticleCollection *pParticles,
		int nControlPointNumber,
		int nBufSize,										// # of output slots available
		ModelHitBoxInfo_t *pHitBoxOutputBuffer );

	virtual Vector GetLocalPlayerPos( void );
	virtual Vector GetLocalPlayerFacing( void );
};


static CParticleSystemQuery s_ParticleSystemQuery;
IParticleSystemQuery *g_pParticleSystemQuery = &s_ParticleSystemQuery;


//-----------------------------------------------------------------------------
// Exposes the interface (so tools can get at it)
//-----------------------------------------------------------------------------
#ifdef CLIENT_DLL
EXPOSE_SINGLE_INTERFACE_GLOBALVAR( CParticleSystemQuery, IParticleSystemQuery, PARTICLE_SYSTEM_QUERY_INTERFACE_VERSION, s_ParticleSystemQuery );
#endif

static CThreadFastMutex s_LightMutex;
static CThreadFastMutex s_BoneMutex;


//-----------------------------------------------------------------------------
// Inherited from IParticleSystemQuery
//-----------------------------------------------------------------------------
void CParticleSystemQuery::GetLightingAtPoint( const Vector& vecOrigin, Color &cTint )
{
#ifdef GAME_DLL

	// FIXME: Go through to the engine from the server to get these values
	cTint.SetColor( 255, 255, 255, 255 );

#else

	if ( engine->IsInGame() )
	{
		s_LightMutex.Lock();
		// Compute our lighting at our position
		Vector totalColor = engine->GetLightForPoint( vecOrigin, true );
		s_LightMutex.Unlock();

		// Get our lighting information
		cTint.SetColor( totalColor.x*255, totalColor.y*255, totalColor.z*255, 0 );
	}
	else
	{
		// FIXME: Go through to the engine from the server to get these values
		cTint.SetColor( 255, 255, 255, 255 );
 	}

#endif
}

void CParticleSystemQuery::TraceLine( const Vector& vecAbsStart,
									  const Vector& vecAbsEnd, unsigned int mask, 
									  const IHandleEntity *ignore,
									  int collisionGroup, CBaseTrace *ptr )
{
	bool bDoTrace = false;
#ifndef GAME_DLL
	bDoTrace = engine->IsInGame();
#endif
	if ( bDoTrace )
	{
		trace_t tempTrace;
		UTIL_TraceLine( vecAbsStart, vecAbsEnd, mask, ignore, collisionGroup, &tempTrace );
		memcpy( ptr, &tempTrace, sizeof ( CBaseTrace ) );
	}
	else
	{
		ptr->startsolid = 0;
		ptr->fraction = 1.0;
	}

}

bool CParticleSystemQuery::MovePointInsideControllingObject( 
	CParticleCollection *pParticles, void *pObject, Vector *pPnt )
{
#ifdef GAME_DLL
	return true;
#else
	if (! pObject )
		return true;										// accept the input point unmodified

	Ray_t ray;
	trace_t tr;
	ray.Init( *pPnt, *pPnt );
	enginetrace->ClipRayToEntity( ray, MASK_ALL, (CBaseEntity *) pObject, &tr );
	
	return ( tr.startsolid );
#endif
}

static float GetSurfaceCoord( float flRand, float flMinX, float flMaxX )
{
	return Lerp( flRand, flMinX, flMaxX );

}


void CParticleSystemQuery::GetRandomPointsOnControllingObjectHitBox( 
	CParticleCollection *pParticles,
	int nControlPointNumber, 
	int nNumPtsOut,
	float flBBoxScale,
	int nNumTrysToGetAPointInsideTheModel,
	Vector *pPntsOut,
	Vector vecDirectionalBias,
	Vector *pHitBoxRelativeCoordOut,
	int *pHitBoxIndexOut
	)
{

	bool bSucesss = false;


#ifndef GAME_DLL

	EHANDLE *phMoveParent = reinterpret_cast<EHANDLE *> ( pParticles->m_ControlPoints[nControlPointNumber].m_pObject );
	CBaseEntity *pMoveParent = NULL;
	if ( phMoveParent )
	{
		pMoveParent = *( phMoveParent );
	}
	if ( pMoveParent )
	{
		float flRandMax = flBBoxScale;
		float flRandMin = 1.0 - flBBoxScale;
		Vector vecBasePos;
		pParticles->GetControlPointAtTime( nControlPointNumber, pParticles->m_flCurTime, &vecBasePos );

		s_BoneMutex.Lock();
		C_BaseAnimating *pAnimating = pMoveParent->GetBaseAnimating();
		if ( pAnimating )
		{
			
			matrix3x4_t	*hitboxbones[MAXSTUDIOBONES];
			
			if ( pAnimating->HitboxToWorldTransforms( hitboxbones ) )
			{
		
				studiohdr_t *pStudioHdr = modelinfo->GetStudiomodel( pAnimating->GetModel() );
				
				if ( pStudioHdr )
				{
					mstudiohitboxset_t *set = pStudioHdr->pHitboxSet( pAnimating->GetHitboxSet() );
					
					if ( set )
					{
						bSucesss = true;
						
						Vector vecWorldPosition;
						float u = 0, v = 0, w = 0;
						int nHitbox = 0;
						int nNumIters = nNumTrysToGetAPointInsideTheModel;
						if (! vecDirectionalBias.IsZero( 0.0001 ) )
							nNumIters = max( nNumIters, 5 );

						for( int i=0 ; i < nNumPtsOut; i++)
						{
							int nTryCnt = nNumIters;
							float flBestPointGoodness = -1.0e20;
							do
							{
								int nTryHitbox = pParticles->RandomInt( 0, set->numhitboxes - 1 );
								mstudiobbox_t *pBox = set->pHitbox(nTryHitbox);
								
								float flTryU = pParticles->RandomFloat( flRandMin, flRandMax );
								float flTryV = pParticles->RandomFloat( flRandMin, flRandMax );
								float flTryW = pParticles->RandomFloat( flRandMin, flRandMax );

								Vector vecLocalPosition;
								vecLocalPosition.x = GetSurfaceCoord( flTryU, pBox->bbmin.x, pBox->bbmax.x );
								vecLocalPosition.y = GetSurfaceCoord( flTryV, pBox->bbmin.y, pBox->bbmax.y );
								vecLocalPosition.z = GetSurfaceCoord( flTryW, pBox->bbmin.z, pBox->bbmax.z );

								Vector vecTryWorldPosition;

								VectorTransform( vecLocalPosition, *hitboxbones[pBox->bone], vecTryWorldPosition );
								
								
								float flPointGoodness = pParticles->RandomFloat( 0, 72 )
									+ DotProduct( vecTryWorldPosition - vecBasePos, 
												  vecDirectionalBias );

								if ( nNumTrysToGetAPointInsideTheModel )
								{
									// do a point in solid test
									Ray_t ray;
									trace_t tr;
									ray.Init( vecTryWorldPosition, vecTryWorldPosition );
									enginetrace->ClipRayToEntity( ray, MASK_ALL, pMoveParent, &tr );
									if ( tr.startsolid )
										flPointGoodness += 1000.; // got a point inside!
								}
								if ( flPointGoodness > flBestPointGoodness )
								{
									u = flTryU;
									v = flTryV;
									w = flTryW;
									vecWorldPosition = vecTryWorldPosition;
									nHitbox = nTryHitbox;
									flBestPointGoodness = flPointGoodness;
								}
							} while ( nTryCnt-- );
							*( pPntsOut++ ) = vecWorldPosition;
							if ( pHitBoxRelativeCoordOut )
								( pHitBoxRelativeCoordOut++ )->Init( u, v, w );
							if ( pHitBoxIndexOut )
								*( pHitBoxIndexOut++ ) = nHitbox;
						}
					}
				}
			}
		}
		s_BoneMutex.Unlock();
	}
#endif
	if (! bSucesss )
	{
		// don't have a model or am in editor or something - fill return with control point
		for( int i=0 ; i < nNumPtsOut; i++)
		{
			pPntsOut[i] = pParticles->m_ControlPoints[nControlPointNumber].m_Position; // fallback if anything goes wrong
			
			if ( pHitBoxIndexOut )
				pHitBoxIndexOut[i] = 0;
			
			if ( pHitBoxRelativeCoordOut )
				pHitBoxRelativeCoordOut[i].Init();
		}
	}
}


int CParticleSystemQuery::GetControllingObjectHitBoxInfo(
	CParticleCollection *pParticles,
	int nControlPointNumber,
	int nBufSize,										// # of output slots available
	ModelHitBoxInfo_t *pHitBoxOutputBuffer )
{
	int nRet = 0;

#ifndef GAME_DLL
	s_BoneMutex.Lock();

	EHANDLE *phMoveParent = reinterpret_cast<EHANDLE *> ( pParticles->m_ControlPoints[nControlPointNumber].m_pObject );
	CBaseEntity *pMoveParent = NULL;
	if ( phMoveParent )
	{
		pMoveParent = *( phMoveParent );
	}

	if ( pMoveParent )
	{
		C_BaseAnimating *pAnimating = pMoveParent->GetBaseAnimating();
		if ( pAnimating )
		{
			matrix3x4_t	*hitboxbones[MAXSTUDIOBONES];
			
			if ( pAnimating->HitboxToWorldTransforms( hitboxbones ) )
			{
		
				studiohdr_t *pStudioHdr = modelinfo->GetStudiomodel( pAnimating->GetModel() );
				
				if ( pStudioHdr )
				{
					mstudiohitboxset_t *set = pStudioHdr->pHitboxSet( pAnimating->GetHitboxSet() );
					
					if ( set )
					{
						nRet = min( nBufSize, set->numhitboxes );
						for( int i=0 ; i < nRet; i++ )
						{
							mstudiobbox_t *pBox = set->pHitbox( i );
							pHitBoxOutputBuffer[i].m_vecBoxMins.x = pBox->bbmin.x;
							pHitBoxOutputBuffer[i].m_vecBoxMins.y = pBox->bbmin.y;
							pHitBoxOutputBuffer[i].m_vecBoxMins.z = pBox->bbmin.z;

							pHitBoxOutputBuffer[i].m_vecBoxMaxes.x = pBox->bbmax.x;
							pHitBoxOutputBuffer[i].m_vecBoxMaxes.y = pBox->bbmax.y;
							pHitBoxOutputBuffer[i].m_vecBoxMaxes.z = pBox->bbmax.z;

							pHitBoxOutputBuffer[i].m_Transform = *hitboxbones[pBox->bone];
						}
					}
				}
			}
		}
	}
	s_BoneMutex.Unlock();
#endif
	return nRet;
}

struct CollisionGroupNameRecord_t
{
	const char *m_pszGroupName;
	int m_nGroupID;
};


static CollisionGroupNameRecord_t s_NameMap[]={
	{ "NONE", COLLISION_GROUP_NONE },
	{ "DEBRIS", COLLISION_GROUP_DEBRIS },
	{ "INTERACTIVE", COLLISION_GROUP_INTERACTIVE },
	{ "NPC", COLLISION_GROUP_NPC },
	{ "ACTOR", COLLISION_GROUP_NPC_ACTOR },
	{ "PASSABLE", COLLISION_GROUP_PASSABLE_DOOR },	
#if defined( TF_CLIENT_DLL )
	{ "ROCKETS", TFCOLLISION_GROUP_ROCKETS },
#endif
};


int CParticleSystemQuery::GetCollisionGroupFromName( const char *pszCollisionGroupName )
{
	for(int i = 0; i < ARRAYSIZE( s_NameMap ); i++ )
	{
		if ( ! stricmp( s_NameMap[i].m_pszGroupName, pszCollisionGroupName ) )
			return s_NameMap[i].m_nGroupID;
	}
	return COLLISION_GROUP_NONE;
}

Vector CParticleSystemQuery::GetLocalPlayerPos( void )
{
#ifdef CLIENT_DLL
	C_BasePlayer *pPlayer = C_BasePlayer::GetLocalPlayer();
	if ( !pPlayer )
		return vec3_origin;
	return pPlayer->WorldSpaceCenter();
#else
	CBasePlayer *pPlayer = AI_GetSinglePlayer();	
	if ( !pPlayer )
		return vec3_origin;
	return pPlayer->WorldSpaceCenter();
#endif
}

Vector CParticleSystemQuery::GetLocalPlayerFacing( void )
{
#ifdef CLIENT_DLL
	C_BasePlayer *pPlayer = C_BasePlayer::GetLocalPlayer();
	if ( !pPlayer )
		return vec3_origin;
	Vector	vecForward;
	AngleVectors( pPlayer->LocalEyeAngles(), &vecForward );
	return vecForward;
#else
	CBasePlayer *pPlayer = AI_GetSinglePlayer();	
	if ( !pPlayer )
		return vec3_origin;
	Vector	vecForward;
	AngleVectors( pPlayer->LocalEyeAngles(), &vecForward );
	return vecForward;
#endif
}