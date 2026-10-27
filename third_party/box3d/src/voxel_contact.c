// mcc extension: voxel shape contacts.
// The geometry lives in mcc. Box3D asks the world's voxel callback for manifolds and stores
// them in the contact, reusing the multi-manifold (mesh) solver path.
// SPDX-License-Identifier: MIT

#include "arena_allocator.h"
#include "body.h"
#include "contact.h"
#include "core.h"
#include "physics_world.h"
#include "shape.h"

#include "box3d/collision.h"

#include <string.h>

static inline uint32_t b3VoxelKeyHash( uint64_t key )
{
	key ^= key >> 33;
	key *= 0xff51afd7ed558ccdULL;
	key ^= key >> 33;
	return (uint32_t)key;
}

bool b3ComputeVoxelManifolds( b3World* world, int workerIndex, b3Contact* contact, const b3Shape* shapeA,
							  b3WorldTransform xfA, const b3Shape* shapeB, b3WorldTransform xfB, b3Arena arena )
{
	B3_ASSERT( shapeA->type == b3_voxelShape );

	int count = 0;
	const b3VoxelManifold* source = NULL;

	if ( world->voxelManifoldFcn != NULL )
	{
		b3VoxelQuery query = { 0 };
		query.userA = shapeA->voxel.userData;
		query.transformA = xfA;
		query.typeB = shapeB->type;
		query.transformB = xfB;
		query.speculativeDistance = B3_SPECULATIVE_DISTANCE;
		query.workerIndex = workerIndex;

		switch ( shapeB->type )
		{
			case b3_voxelShape:
				query.userB = shapeB->voxel.userData;
				break;
			case b3_sphereShape:
				query.sphereB = shapeB->sphere;
				break;
			case b3_capsuleShape:
				query.capsuleB = shapeB->capsule;
				break;
			case b3_hullShape:
				query.hullB = shapeB->hull;
				break;
			default:
				B3_ASSERT( false );
				break;
		}

		source = world->voxelManifoldFcn( &query, &count, world->voxelManifoldContext );
	}

	if ( count <= 0 || source == NULL )
	{
		if ( contact->manifoldCount > 0 )
		{
			b3FreeManifolds( world, contact->manifolds, contact->manifoldCount );
			contact->manifolds = NULL;
			contact->manifoldCount = 0;
		}
		return false;
	}

	// Keep a copy of the old manifolds for warm starting
	int oldCount = contact->manifoldCount;
	b3Manifold* oldManifolds = NULL;
	int tableMask = 0;
	int* table = NULL;
	if ( oldCount > 0 )
	{
		oldManifolds = b3Bump( &arena, oldCount * (int)sizeof( b3Manifold ) );
		memcpy( oldManifolds, contact->manifolds, oldCount * sizeof( b3Manifold ) );

		int tableSize = 8;
		while ( tableSize < 2 * oldCount )
		{
			tableSize <<= 1;
		}
		tableMask = tableSize - 1;
		table = b3Bump( &arena, tableSize * (int)sizeof( int ) );
		for ( int i = 0; i < tableSize; ++i )
		{
			table[i] = B3_NULL_INDEX;
		}

		for ( int i = 0; i < oldCount; ++i )
		{
			uint32_t slot = b3VoxelKeyHash( oldManifolds[i].voxelKey ) & (uint32_t)tableMask;
			while ( table[slot] != B3_NULL_INDEX )
			{
				slot = ( slot + 1 ) & (uint32_t)tableMask;
			}
			table[slot] = i;
		}
	}

	if ( oldCount != count )
	{
		b3FreeManifolds( world, contact->manifolds, contact->manifoldCount );
		contact->manifolds = b3AllocateManifolds( world, count );
		contact->manifoldCount = (uint16_t)count;
	}
	else
	{
		memset( contact->manifolds, 0, count * sizeof( b3Manifold ) );
	}

	b3Matrix3 matrixA = b3MakeMatrixFromQuat( xfA.q );
	b3Vec3 offsetB = b3SubPos( xfA.p, xfB.p );
	float maxRestitution = 0.0f;
	float frictionSum = 0.0f;

	for ( int i = 0; i < count; ++i )
	{
		const b3VoxelManifold* vm = source + i;
		b3Manifold* manifold = contact->manifolds + i;

		int pointCount = b3ClampInt( vm->pointCount, 0, B3_MAX_MANIFOLD_POINTS );
		manifold->pointCount = pointCount;
		manifold->normal = b3MulMV( matrixA, vm->normal );
		manifold->customMaterial = true;
		manifold->friction = vm->friction;
		manifold->restitution = vm->restitution;
		manifold->tangentVelocity = vm->tangentVelocity;
		manifold->voxelKey = vm->key;
		manifold->userValue = vm->userValue;

		maxRestitution = b3MaxFloat( maxRestitution, vm->restitution );
		frictionSum += vm->friction;

		// Find the matching old manifold
		b3Manifold* old = NULL;
		if ( oldCount > 0 )
		{
			uint32_t slot = b3VoxelKeyHash( vm->key ) & (uint32_t)tableMask;
			while ( table[slot] != B3_NULL_INDEX )
			{
				b3Manifold* candidate = oldManifolds + table[slot];
				if ( candidate->voxelKey == vm->key )
				{
					old = candidate;
					break;
				}
				slot = ( slot + 1 ) & (uint32_t)tableMask;
			}
		}

		if ( old != NULL )
		{
			manifold->frictionImpulse = old->frictionImpulse;
			manifold->rollingImpulse = old->rollingImpulse;
			manifold->twistImpulse = old->twistImpulse;
		}

		for ( int j = 0; j < pointCount; ++j )
		{
			b3ManifoldPoint* mp = manifold->points + j;
			mp->anchorA = b3MulMV( matrixA, vm->points[j] );
			mp->anchorB = b3Add( mp->anchorA, offsetB );
			mp->separation = vm->separations[j];
			mp->featureId = vm->featureIds[j];
			mp->triangleIndex = B3_NULL_INDEX;
			mp->normalVelocity = 0.0f;
			mp->normalImpulse = 0.0f;
			mp->persisted = false;

			if ( old != NULL )
			{
				for ( int k = 0; k < old->pointCount; ++k )
				{
					b3ManifoldPoint* op = old->points + k;
					if ( op->featureId == mp->featureId && op->triangleIndex != B3_NULL_INDEX - 1 )
					{
						mp->normalImpulse = op->normalImpulse;
						mp->persisted = true;
						// claimed
						op->triangleIndex = B3_NULL_INDEX - 1;
						break;
					}
				}
			}
		}
	}

	// Contact level values: restitution is the maximum (used for early outs),
	// friction the average (only informational for voxel contacts).
	contact->friction = frictionSum / (float)count;
	contact->restitution = maxRestitution;
	contact->rollingResistance = 0.0f;
	contact->tangentVelocity = b3Vec3_zero;
	B3_UNUSED( workerIndex );
	return true;
}
