/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) Blender Foundation
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): Lukas Toenne
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/physics/intern/BPH_mass_spring.cpp
 *  \ingroup bph
 */

extern "C" {
#include "MEM_guardedalloc.h"

#include "DNA_cache_library_types.h"
#include "DNA_cloth_types.h"
#include "DNA_scene_types.h"
#include "DNA_object_force.h"
#include "DNA_object_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"

#include "BLI_math.h"
#include "BLI_linklist.h"
#include "BLI_utildefines.h"

#include "BKE_cloth.h"
#include "BKE_collision.h"
#include "BKE_colortools.h"
#include "BKE_effect.h"
#include "BKE_strands.h"
}

#include "BPH_mass_spring.h"
#include "implicit.h"

static float I3[3][3] = {{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}};

/* Number of off-diagonal non-zero matrix blocks.
 * Basically there is one of these for each vertex-vertex interaction.
 */
static int cloth_count_nondiag_blocks(Cloth *cloth)
{
	LinkNode *link;
	int nondiag = 0;
	
	for (link = cloth->springs; link; link = link->next) {
		ClothSpring *spring = (ClothSpring *)link->link;
		
		switch (spring->type) {
			case CLOTH_SPRING_TYPE_BENDING_ANG:
				/* angular bending combines 3 vertices */
				nondiag += 3;
				break;
				
			default:
				/* all other springs depend on 2 vertices only */
				nondiag += 1;
				break;
		}
	}
	
	return nondiag;
}

static struct Implicit_Data *cloth_solver_init_data(Cloth *cloth)
{
	int totvert = cloth->numverts;
	
	if (cloth->implicit && totvert != BPH_mass_spring_solver_numvert(cloth->implicit)) {
		BPH_mass_spring_solver_free(cloth->implicit);
		cloth->implicit = NULL;
	}
	
	if (!cloth->implicit) {
		int nondiag = cloth_count_nondiag_blocks(cloth);
		cloth->implicit = BPH_mass_spring_solver_create(totvert, nondiag);
	}
	
	return cloth->implicit;
}

int BPH_cloth_solver_init(Object *UNUSED(ob), ClothModifierData *clmd)
{
	Cloth *cloth = clmd->clothObject;
	ClothVertex *vert;
	const float ZERO[3] = {0.0f, 0.0f, 0.0f};
	Implicit_Data *id = cloth_solver_init_data(cloth);
	unsigned int i;
	
	vert = cloth->verts;
	for (i = 0; i < cloth->numverts; ++i, ++vert) {
		BPH_mass_spring_set_vertex_mass(id, i, vert->mass);
		BPH_mass_spring_set_motion_state(id, i, vert->x, ZERO);
	}
	
	return 1;
}

void BPH_cloth_solver_free(ClothModifierData *clmd)
{
	Cloth *cloth = clmd->clothObject;
	
	if (cloth->implicit) {
		BPH_mass_spring_solver_free(cloth->implicit);
		cloth->implicit = NULL;
	}
}

void BPH_cloth_solver_set_positions(ClothModifierData *clmd)
{
	Cloth *cloth = clmd->clothObject;
	ClothVertex *vert;
	unsigned int numverts = cloth->numverts, i;
	ClothHairData *cloth_hairdata = clmd->hairdata;
	Implicit_Data *id = cloth_solver_init_data(cloth);
	
	vert = cloth->verts;
	for (i = 0; i < numverts; ++i, ++vert) {
		if (cloth_hairdata) {
			ClothHairData *root = &cloth_hairdata[i];
			BPH_mass_spring_set_rest_transform(id, i, root->rot);
		}
		else
			BPH_mass_spring_set_rest_transform(id, i, I3);
		
		BPH_mass_spring_set_motion_state(id, i, vert->x, vert->v);
	}
}

static bool collision_response(ClothModifierData *clmd, CollisionModifierData *collmd, CollPair *collpair, float dt, float restitution, float r_impulse[3])
{
	Cloth *cloth = clmd->clothObject;
	int index = collpair->ap1;
	bool result = false;
	
	float v1[3], v2_old[3], v2_new[3], v_rel_old[3], v_rel_new[3];
	float epsilon2 = BLI_bvhtree_getepsilon(collmd->bvhtree);

	float margin_distance = collpair->distance - epsilon2;
	float mag_v_rel;
	
	zero_v3(r_impulse);
	
	if (margin_distance > 0.0f)
		return false; /* XXX tested before already? */
	
	/* only handle static collisions here */
	if ( collpair->flag & COLLISION_IN_FUTURE )
		return false;
	
	/* velocity */
	copy_v3_v3(v1, cloth->verts[index].v);
	collision_get_collider_velocity(v2_old, v2_new, collmd, collpair);
	/* relative velocity = velocity of the cloth point relative to the collider */
	sub_v3_v3v3(v_rel_old, v1, v2_old);
	sub_v3_v3v3(v_rel_new, v1, v2_new);
	/* normal component of the relative velocity */
	mag_v_rel = dot_v3v3(v_rel_old, collpair->normal);
	
	/* only valid when moving toward the collider */
	if (mag_v_rel < -ALMOST_ZERO) {
		float v_nor_old, v_nor_new;
		float v_tan_old[3], v_tan_new[3];
		float bounce, repulse;
		
		/* Collision response based on
		 * "Simulating Complex Hair with Robust Collision Handling" (Choe, Choi, Ko, ACM SIGGRAPH 2005)
		 * http://graphics.snu.ac.kr/publications/2005-choe-HairSim/Choe_2005_SCA.pdf
		 */
		
		v_nor_old = mag_v_rel;
		v_nor_new = dot_v3v3(v_rel_new, collpair->normal);
		
		madd_v3_v3v3fl(v_tan_old, v_rel_old, collpair->normal, -v_nor_old);
		madd_v3_v3v3fl(v_tan_new, v_rel_new, collpair->normal, -v_nor_new);
		
		bounce = -v_nor_old * restitution;
		
		repulse = -margin_distance / dt; /* base repulsion velocity in normal direction */
		/* XXX this clamping factor is quite arbitrary ...
		 * not sure if there is a more scientific approach, but seems to give good results
		 */
		CLAMP(repulse, 0.0f, 4.0f * bounce);
		
		if (margin_distance < -epsilon2) {
			mul_v3_v3fl(r_impulse, collpair->normal, max_ff(repulse, bounce) - v_nor_new);
		}
		else {
			bounce = 0.0f;
			mul_v3_v3fl(r_impulse, collpair->normal, repulse - v_nor_new);
		}
		
		result = true;
	}
	
	return result;
}

/* Init constraint matrix
 * This is part of the modified CG method suggested by Baraff/Witkin in
 * "Large Steps in Cloth Simulation" (Siggraph 1998)
 */
static void cloth_setup_constraints(ClothModifierData *clmd, ColliderContacts *contacts, int totcolliders, float dt)
{
	Cloth *cloth = clmd->clothObject;
	Implicit_Data *data = cloth->implicit;
	ClothVertex *vert;
	int numverts = cloth->numverts;
	int i, j, v;
	
	const float ZERO[3] = {0.0f, 0.0f, 0.0f};
	
	BPH_mass_spring_clear_constraints(data);
	
	vert = cloth->verts;
	for (v = 0; v < numverts; ++v, ++vert) {
		if (vert->flags & CLOTH_VERT_FLAG_PINNED) {
			/* pinned vertex constraints */
			BPH_mass_spring_add_constraint_ndof0(data, v, ZERO); /* velocity is defined externally */
		}
		
		vert->impulse_count = 0;
	}

	for (i = 0; i < totcolliders; ++i) {
		ColliderContacts *ct = &contacts[i];
		for (j = 0; j < ct->totcollisions; ++j) {
			CollPair *collpair = &ct->collisions[j];
			int v = collpair->face1;
			float impulse[3];
			
//			float restitution = (1.0f - clmd->coll_parms->damping) * (1.0f - ct->ob->pd->pdef_sbdamp);
			float restitution = 0.0f;
			
			vert = &cloth->verts[v];
			/* pinned verts handled separately */
			if (vert->flags & CLOTH_VERT_FLAG_PINNED)
				continue;
			
			/* XXX cheap way of avoiding instability from multiple collisions in the same step
			 * this should eventually be supported ...
			 */
			if (vert->impulse_count > 0)
				continue;
			
			/* calculate collision response */
			if (!collision_response(clmd, ct->collmd, collpair, dt, restitution, impulse))
				continue;
			
			BPH_mass_spring_add_constraint_ndof2(data, i, collpair->normal, impulse);
			++vert->impulse_count;
		}
	}
}

/* computes where the cloth would be if it were subject to perfectly stiff edges
 * (edge distance constraints) in a lagrangian solver.  then add forces to help
 * guide the implicit solver to that state.  this function is called after
 * collisions*/
static int UNUSED_FUNCTION(cloth_calc_helper_forces)(Object *UNUSED(ob), ClothModifierData *clmd, float (*initial_cos)[3], float UNUSED(step), float dt)
{
	Cloth *cloth= clmd->clothObject;
	float (*cos)[3] = (float (*)[3])MEM_callocN(sizeof(float)*3*cloth->numverts, "cos cloth_calc_helper_forces");
	float *masses = (float *)MEM_callocN(sizeof(float)*cloth->numverts, "cos cloth_calc_helper_forces");
	LinkNode *node;
	ClothSpring *spring;
	ClothVertex *vert;
	int i, steps;
	
	vert = cloth->verts;
	for (i=0; i<cloth->numverts; i++, vert++) {
		copy_v3_v3(cos[i], vert->tx);
		
		if (vert->goal == 1.0f || len_squared_v3v3(initial_cos[i], vert->tx) != 0.0f) {
			masses[i] = 1e+10;
		}
		else {
			masses[i] = vert->mass;
		}
	}
	
	steps = 55;
	for (i=0; i<steps; i++) {
		for (node=cloth->springs; node; node=node->next) {
			/* ClothVertex *cv1, *cv2; */ /* UNUSED */
			int v1, v2;
			float len, c, l, vec[3];
			
			spring = (ClothSpring *)node->link;
			if (spring->type != CLOTH_SPRING_TYPE_STRUCTURAL && spring->type != CLOTH_SPRING_TYPE_SHEAR) 
				continue;
			
			v1 = spring->ij; v2 = spring->kl;
			/* cv1 = cloth->verts + v1; */ /* UNUSED */
			/* cv2 = cloth->verts + v2; */ /* UNUSED */
			len = len_v3v3(cos[v1], cos[v2]);
			
			sub_v3_v3v3(vec, cos[v1], cos[v2]);
			normalize_v3(vec);
			
			c = (len - spring->restlen);
			if (c == 0.0f)
				continue;
			
			l = c / ((1.0f / masses[v1]) + (1.0f / masses[v2]));
			
			mul_v3_fl(vec, -(1.0f / masses[v1]) * l);
			add_v3_v3(cos[v1], vec);
	
			sub_v3_v3v3(vec, cos[v2], cos[v1]);
			normalize_v3(vec);
			
			mul_v3_fl(vec, -(1.0f / masses[v2]) * l);
			add_v3_v3(cos[v2], vec);
		}
	}
	
	vert = cloth->verts;
	for (i=0; i<cloth->numverts; i++, vert++) {
		float vec[3];
		
		/*compute forces*/
		sub_v3_v3v3(vec, cos[i], vert->tx);
		mul_v3_fl(vec, vert->mass*dt*20.0f);
		add_v3_v3(vert->tv, vec);
		//copy_v3_v3(cv->tx, cos[i]);
	}
	
	MEM_freeN(cos);
	MEM_freeN(masses);
	
	return 1;
}

BLI_INLINE void cloth_calc_spring_force(ClothModifierData *clmd, ClothSpring *s, float time)
{
	Cloth *cloth = clmd->clothObject;
	ClothSimSettings *parms = clmd->sim_parms;
	Implicit_Data *data = cloth->implicit;
	ClothVertex *verts = cloth->verts;
	
	bool no_compress = parms->flags & CLOTH_SIMSETTINGS_FLAG_NO_SPRING_COMPRESS;
	
	zero_v3(s->f);
	zero_m3(s->dfdx);
	zero_m3(s->dfdv);
	
	s->flags &= ~CLOTH_SPRING_FLAG_NEEDED;
	
	// calculate force of structural + shear springs
	if ((s->type & CLOTH_SPRING_TYPE_STRUCTURAL) || (s->type & CLOTH_SPRING_TYPE_SHEAR) || (s->type & CLOTH_SPRING_TYPE_SEWING) ) {
#ifdef CLOTH_FORCE_SPRING_STRUCTURAL
		float k, scaling;
		
		s->flags |= CLOTH_SPRING_FLAG_NEEDED;
		
		scaling = parms->structural + s->stiffness * fabsf(parms->max_struct - parms->structural);
		k = scaling / (parms->avg_spring_len + FLT_EPSILON);
		
		if (s->type & CLOTH_SPRING_TYPE_SEWING) {
			// TODO: verify, half verified (couldn't see error)
			// sewing springs usually have a large distance at first so clamp the force so we don't get tunnelling through colission objects
			BPH_mass_spring_force_spring_linear(data, s->ij, s->kl, s->restlen, k, parms->Cdis, no_compress, parms->max_sewing, s->f, s->dfdx, s->dfdv);
		}
		else {
			BPH_mass_spring_force_spring_linear(data, s->ij, s->kl, s->restlen, k, parms->Cdis, no_compress, 0.0f, s->f, s->dfdx, s->dfdv);
		}
#endif
	}
	else if (s->type & CLOTH_SPRING_TYPE_GOAL) {
#ifdef CLOTH_FORCE_SPRING_GOAL
		float goal_x[3], goal_v[3];
		float k, scaling;
		
		s->flags |= CLOTH_SPRING_FLAG_NEEDED;
		
		// current_position = xold + t * (newposition - xold)
		interp_v3_v3v3(goal_x, verts[s->ij].xold, verts[s->ij].xconst, time);
		sub_v3_v3v3(goal_v, verts[s->ij].xconst, verts[s->ij].xold); // distance covered over dt==1
		
		scaling = parms->goalspring + s->stiffness * fabsf(parms->max_struct - parms->goalspring);
		k = verts[s->ij].goal * scaling / (parms->avg_spring_len + FLT_EPSILON);
		
		BPH_mass_spring_force_spring_goal(data, s->ij, goal_x, goal_v, k, parms->goalfrict * 0.01f, s->f, s->dfdx, s->dfdv);
#endif
	}
	else if (s->type & CLOTH_SPRING_TYPE_BENDING) {  /* calculate force of bending springs */
#ifdef CLOTH_FORCE_SPRING_BEND
		float kb, cb, scaling;
		
		s->flags |= CLOTH_SPRING_FLAG_NEEDED;
		
		scaling = parms->bending + s->stiffness * fabsf(parms->max_bend - parms->bending);
		kb = scaling / (20.0f * (parms->avg_spring_len + FLT_EPSILON));
		
		scaling = parms->bending_damping;
		cb = scaling / (20.0f * (parms->avg_spring_len + FLT_EPSILON));
		
		BPH_mass_spring_force_spring_bending(data, s->ij, s->kl, s->restlen, kb, cb, s->f, s->dfdx, s->dfdv);
#endif
	}
	else if (s->type & CLOTH_SPRING_TYPE_BENDING_ANG) {
#ifdef CLOTH_FORCE_SPRING_BEND
		float kb, cb, scaling;
		
		s->flags |= CLOTH_SPRING_FLAG_NEEDED;
		
		/* XXX WARNING: angular bending springs for hair apply stiffness factor as an overall factor, unlike cloth springs!
		 * this is crap, but needed due to cloth/hair mixing ...
		 * max_bend factor is not even used for hair, so ...
		 */
		scaling = s->stiffness * parms->bending;
		kb = scaling / (20.0f * (parms->avg_spring_len + FLT_EPSILON));
		
		scaling = parms->bending_damping;
		cb = scaling / (20.0f * (parms->avg_spring_len + FLT_EPSILON));
		
		/* XXX assuming same restlen for ij and jk segments here, this can be done correctly for hair later */
		BPH_mass_spring_force_spring_bending_angular(data, s->ij, s->kl, s->mn, s->target, kb, cb);
		
#if 0
		{
			float x_kl[3], x_mn[3], v[3], d[3];
			
			BPH_mass_spring_get_motion_state(data, s->kl, x_kl, v);
			BPH_mass_spring_get_motion_state(data, s->mn, x_mn, v);
			
			BKE_sim_debug_data_add_dot(clmd->debug_data, x_kl, 0.9, 0.9, 0.9, "target", 7980, s->kl);
			BKE_sim_debug_data_add_line(clmd->debug_data, x_kl, x_mn, 0.8, 0.8, 0.8, "target", 7981, s->kl);
			
			copy_v3_v3(d, s->target);
			BKE_sim_debug_data_add_vector(clmd->debug_data, x_kl, d, 0.8, 0.8, 0.2, "target", 7982, s->kl);
			
//			copy_v3_v3(d, s->target_ij);
//			BKE_sim_debug_data_add_vector(clmd->debug_data, x, d, 1, 0.4, 0.4, "target", 7983, s->kl);
		}
#endif
#endif
	}
}

static void hair_get_boundbox(ClothModifierData *clmd, float gmin[3], float gmax[3])
{
	Cloth *cloth = clmd->clothObject;
	Implicit_Data *data = cloth->implicit;
	unsigned int numverts = cloth->numverts;
	ClothVertex *vert;
	int i;
	
	INIT_MINMAX(gmin, gmax);
	vert = cloth->verts;
	for (i = 0; i < numverts; i++, vert++) {
		float x[3];
		BPH_mass_spring_get_motion_state(data, i, x, NULL);
		DO_MINMAX(x, gmin, gmax);
	}
}

static void cloth_calc_force(ClothModifierData *clmd, float UNUSED(frame), ListBase *effectors, float time)
{
	/* Collect forces and derivatives:  F, dFdX, dFdV */
	Cloth *cloth = clmd->clothObject;
	Implicit_Data *data = cloth->implicit;
	unsigned int i	= 0;
	float 		drag 	= clmd->sim_parms->Cvi * 0.01f; /* viscosity of air scaled in percent */
	float 		gravity[3] = {0.0f, 0.0f, 0.0f};
	MFace 		*mfaces 	= cloth->mfaces;
	unsigned int numverts = cloth->numverts;
	ClothVertex *vert;
	
#ifdef CLOTH_FORCE_GRAVITY
	/* global acceleration (gravitation) */
	if (clmd->scene->physics_settings.flag & PHYS_GLOBAL_GRAVITY) {
		/* scale gravity force */
		mul_v3_v3fl(gravity, clmd->scene->physics_settings.gravity, 0.001f * clmd->sim_parms->effector_weights->global_gravity);
	}
	vert = cloth->verts;
	for (i = 0; i < cloth->numverts; i++, vert++) {
		BPH_mass_spring_force_gravity(data, i, vert->mass, gravity);
	}
#endif

	/* cloth_calc_volume_force(clmd); */

#ifdef CLOTH_FORCE_DRAG
	BPH_mass_spring_force_drag(data, drag);
#endif
	
	/* handle external forces like wind */
	if (effectors) {
		/* cache per-vertex forces to avoid redundant calculation */
		float (*winvec)[3] = (float (*)[3])MEM_callocN(sizeof(float) * 3 * numverts, "effector forces");
		
		vert = cloth->verts;
		for (i = 0; i < cloth->numverts; i++, vert++) {
			float x[3], v[3];
			EffectedPoint epoint;
			
			BPH_mass_spring_get_motion_state(data, i, x, v);
			pd_point_from_loc(clmd->scene, x, v, i, &epoint);
			pdDoEffectors(effectors, NULL, clmd->sim_parms->effector_weights, &epoint, winvec[i], NULL);
		}
		
		for (i = 0; i < cloth->numfaces; i++) {
			MFace *mf = &mfaces[i];
			BPH_mass_spring_force_face_wind(data, mf->v1, mf->v2, mf->v3, mf->v4, winvec);
		}

		/* Hair has only edges */
		if (cloth->numfaces == 0) {
			const float density = 0.01f; /* XXX arbitrary value, corresponds to effect of air density */
#if 0
			ClothHairData *hairdata = clmd->hairdata;
			ClothHairData *hair_ij, *hair_kl;
			
			for (LinkNode *link = cloth->springs; link; link = link->next) {
				ClothSpring *spring = (ClothSpring *)link->link;
				if (spring->type == CLOTH_SPRING_TYPE_STRUCTURAL) {
					ClothVertex *verts = cloth->verts;
					int si_ij = verts[spring->ij].solver_index;
					int si_kl = verts[spring->kl].solver_index;
					
					if (si_ij < 0 || si_kl < 0)
						continue;
					
					if (hairdata) {
						hair_ij = &hairdata[spring->ij];
						hair_kl = &hairdata[spring->kl];
						BPH_mass_spring_force_edge_wind(data, si_ij, si_kl, hair_ij->radius, hair_kl->radius, winvec);
					}
					else
						BPH_mass_spring_force_edge_wind(data, si_ij, si_kl, 1.0f, 1.0f, winvec);
				}
			}
#else
			ClothHairData *hairdata = clmd->hairdata;
			
			vert = cloth->verts;
			for (i = 0; i < cloth->numverts; i++, vert++) {
				if (hairdata) {
					ClothHairData *hair = &hairdata[i];
					BPH_mass_spring_force_vertex_wind(data, i, hair->radius * density, winvec);
				}
				else
					BPH_mass_spring_force_vertex_wind(data, i, density, winvec);
			}
#endif
		}

		MEM_freeN(winvec);
	}
	
	// calculate spring forces
	for (LinkNode *link = cloth->springs; link; link = link->next) {
		ClothSpring *spring = (ClothSpring *)link->link;
		// only handle active springs
		if (!(spring->flags & CLOTH_SPRING_FLAG_DEACTIVATE))
			cloth_calc_spring_force(clmd, spring, time);
	}
}

/* returns vertexes' motion state */
BLI_INLINE void cloth_get_grid_location(Implicit_Data *data, float cell_scale, const float cell_offset[3],
                                        int index, float x[3], float v[3])
{
	BPH_mass_spring_get_position(data, index, x);
	BPH_mass_spring_get_new_velocity(data, index, v);
	
	mul_v3_fl(x, cell_scale);
	add_v3_v3(x, cell_offset);
}

/* returns next spring forming a continous hair sequence */
BLI_INLINE LinkNode *hair_spring_next(LinkNode *spring_link)
{
	ClothSpring *spring = (ClothSpring *)spring_link->link;
	LinkNode *next = spring_link->next;
	if (next) {
		ClothSpring *next_spring = (ClothSpring *)next->link;
		if (next_spring->type == CLOTH_SPRING_TYPE_STRUCTURAL && next_spring->kl == spring->ij)
			return next;
	}
	return NULL;
}

/* XXX this is nasty: cloth meshes do not explicitly store
 * the order of hair segments!
 * We have to rely on the spring build function for now,
 * which adds structural springs in reverse order:
 *   (3,4), (2,3), (1,2)
 * This is currently the only way to figure out hair geometry inside this code ...
 */
static LinkNode *cloth_continuum_add_hair_segments(HairGrid *grid, const float cell_scale, const float cell_offset[3], Cloth *cloth, LinkNode *spring_link)
{
	Implicit_Data *data = cloth->implicit;
	LinkNode *next_spring_link = NULL; /* return value */
	ClothSpring *spring1, *spring2, *spring3;
	float x1[3], v1[3], x2[3], v2[3], x3[3], v3[3], x4[3], v4[3];
	float dir1[3], dir2[3], dir3[3];
	
	spring1 = NULL;
	spring2 = NULL;
	spring3 = (ClothSpring *)spring_link->link;
	
	zero_v3(x1); zero_v3(v1);
	zero_v3(dir1);
	zero_v3(x2); zero_v3(v2);
	zero_v3(dir2);
	
	cloth_get_grid_location(data, cell_scale, cell_offset, spring3->kl, x3, v3);
	cloth_get_grid_location(data, cell_scale, cell_offset, spring3->ij, x4, v4);
	sub_v3_v3v3(dir3, x4, x3);
	normalize_v3(dir3);
	
	while (spring_link) {
		/* move on */
		spring1 = spring2;
		spring2 = spring3;
		
		copy_v3_v3(x1, x2); copy_v3_v3(v1, v2);
		copy_v3_v3(x2, x3); copy_v3_v3(v2, v3);
		copy_v3_v3(x3, x4); copy_v3_v3(v3, v4);
		
		copy_v3_v3(dir1, dir2);
		copy_v3_v3(dir2, dir3);
		
		/* read next segment */
		next_spring_link = spring_link->next;
		spring_link = hair_spring_next(spring_link);
		
		if (spring_link) {
			spring3 = (ClothSpring *)spring_link->link;
			cloth_get_grid_location(data, cell_scale, cell_offset, spring3->ij, x4, v4);
			sub_v3_v3v3(dir3, x4, x3);
			normalize_v3(dir3);
		}
		else {
			spring3 = NULL;
			zero_v3(x4); zero_v3(v4);
			zero_v3(dir3);
		}
		
		BPH_hair_volume_add_segment(grid, x1, v1, x2, v2, x3, v3, x4, v4,
		                            spring1 ? dir1 : NULL,
		                            dir2,
		                            spring3 ? dir3 : NULL);
	}
	
	return next_spring_link;
}

static void cloth_continuum_fill_grid(HairGrid *grid, Cloth *cloth)
{
#if 0
	Implicit_Data *data = cloth->implicit;
	int numverts = cloth->numverts;
	ClothVertex *vert;
	int i;
	
	for (i = 0, vert = cloth->verts; i < numverts; i++, vert++) {
		float x[3], v[3];
		
		cloth_get_vertex_motion_state(data, vert, x, v);
		BPH_hair_volume_add_vertex(grid, x, v);
	}
#else
	LinkNode *link;
	float cellsize, gmin[3], cell_scale, cell_offset[3];
	
	/* scale and offset for transforming vertex locations into grid space
	 * (cell size is 0..1, gmin becomes origin)
	 */
	BPH_hair_volume_grid_geometry(grid, &cellsize, NULL, gmin, NULL);
	cell_scale = cellsize > 0.0f ? 1.0f / cellsize : 0.0f;
	mul_v3_v3fl(cell_offset, gmin, cell_scale);
	negate_v3(cell_offset);
	
	link = cloth->springs;
	while (link) {
		ClothSpring *spring = (ClothSpring *)link->link;
		if (spring->type == CLOTH_SPRING_TYPE_STRUCTURAL)
			link = cloth_continuum_add_hair_segments(grid, cell_scale, cell_offset, cloth, link);
		else
			link = link->next;
	}
#endif
	BPH_hair_volume_normalize_vertex_grid(grid);
}

static void cloth_continuum_step(ClothModifierData *clmd, float dt)
{
	ClothSimSettings *parms = clmd->sim_parms;
	Cloth *cloth = clmd->clothObject;
	Implicit_Data *data = cloth->implicit;
	int numverts = cloth->numverts;
	ClothVertex *vert;
	
	const float fluid_factor = 0.95f; /* blend between PIC and FLIP methods */
	float smoothfac = parms->velocity_smooth;
	/* XXX FIXME arbitrary factor!!! this should be based on some intuitive value instead,
	 * like number of hairs per cell and time decay instead of "strength"
	 */
	float density_target = parms->density_target;
	float density_strength = parms->density_strength;
	float gmin[3], gmax[3];
	int i;
	
	/* clear grid info */
	zero_v3_int(clmd->hair_grid_res);
	zero_v3(clmd->hair_grid_min);
	zero_v3(clmd->hair_grid_max);
	clmd->hair_grid_cellsize = 0.0f;
	
	hair_get_boundbox(clmd, gmin, gmax);
	
	/* gather velocities & density */
	if (smoothfac > 0.0f || density_strength > 0.0f) {
		HairGrid *grid = BPH_hair_volume_create_vertex_grid(clmd->sim_parms->voxel_cell_size, gmin, gmax);
		
		cloth_continuum_fill_grid(grid, cloth);
		
		/* main hair continuum solver */
		BPH_hair_volume_solve_divergence(grid, dt, density_target, density_strength);
		
		for (i = 0, vert = cloth->verts; i < numverts; i++, vert++) {
			float x[3], v[3], nv[3];
			
			/* calculate volumetric velocity influence */
			BPH_mass_spring_get_position(data, i, x);
			BPH_mass_spring_get_new_velocity(data, i, v);
			
			BPH_hair_volume_grid_velocity(grid, x, v, fluid_factor, nv);
			
			interp_v3_v3v3(nv, v, nv, smoothfac);
			
			/* apply on hair data */
			BPH_mass_spring_set_new_velocity(data, i, nv);
		}
		
		/* store basic grid info in the modifier data */
		BPH_hair_volume_grid_geometry(grid, &clmd->hair_grid_cellsize, clmd->hair_grid_res, clmd->hair_grid_min, clmd->hair_grid_max);
		
#if 0 /* DEBUG hair velocity vector field */
		{
			const int size = 64;
			int i, j;
			float offset[3], a[3], b[3];
			const int axis = 0;
			const float shift = 0.0f;
			
			copy_v3_v3(offset, clmd->hair_grid_min);
			zero_v3(a);
			zero_v3(b);
			
			offset[axis] = shift * clmd->hair_grid_cellsize;
			a[(axis+1) % 3] = clmd->hair_grid_max[(axis+1) % 3] - clmd->hair_grid_min[(axis+1) % 3];
			b[(axis+2) % 3] = clmd->hair_grid_max[(axis+2) % 3] - clmd->hair_grid_min[(axis+2) % 3];
			
			BKE_sim_debug_data_clear_category(clmd->debug_data, "grid velocity");
			for (j = 0; j < size; ++j) {
				for (i = 0; i < size; ++i) {
					float x[3], v[3], gvel[3], gvel_smooth[3], gdensity;
					
					madd_v3_v3v3fl(x, offset, a, (float)i / (float)(size-1));
					madd_v3_v3fl(x, b, (float)j / (float)(size-1));
					zero_v3(v);
					
					BPH_hair_volume_grid_interpolate(grid, x, &gdensity, gvel, gvel_smooth, NULL, NULL);
					
//					BKE_sim_debug_data_add_circle(clmd->debug_data, x, gdensity, 0.7, 0.3, 1, "grid density", i, j, 3111);
					if (!is_zero_v3(gvel) || !is_zero_v3(gvel_smooth)) {
						float dvel[3];
						sub_v3_v3v3(dvel, gvel_smooth, gvel);
//						BKE_sim_debug_data_add_vector(clmd->debug_data, x, gvel, 0.4, 0, 1, "grid velocity", i, j, 3112);
//						BKE_sim_debug_data_add_vector(clmd->debug_data, x, gvel_smooth, 0.6, 1, 1, "grid velocity", i, j, 3113);
						BKE_sim_debug_data_add_vector(clmd->debug_data, x, dvel, 0.4, 1, 0.7, "grid velocity", i, j, 3114);
#if 0
						if (gdensity > 0.0f) {
							float col0[3] = {0.0, 0.0, 0.0};
							float col1[3] = {0.0, 1.0, 0.0};
							float col[3];
							
							interp_v3_v3v3(col, col0, col1, CLAMPIS(gdensity * clmd->sim_parms->density_strength, 0.0, 1.0));
//							BKE_sim_debug_data_add_circle(clmd->debug_data, x, gdensity * clmd->sim_parms->density_strength, 0, 1, 0.4, "grid velocity", i, j, 3115);
//							BKE_sim_debug_data_add_dot(clmd->debug_data, x, col[0], col[1], col[2], "grid velocity", i, j, 3115);
							BKE_sim_debug_data_add_circle(clmd->debug_data, x, 0.01f, col[0], col[1], col[2], "grid velocity", i, j, 3115);
						}
#endif
					}
				}
			}
		}
#endif
		
		BPH_hair_volume_free_vertex_grid(grid);
	}
}

/* old collision stuff for cloth, use for continuity
 * until a good replacement is ready
 */
static void cloth_collision_solve_extra(Object *ob, ClothModifierData *clmd, ListBase *effectors, float frame, float step, float dt)
{
	Cloth *cloth = clmd->clothObject;
	Implicit_Data *id = cloth->implicit;
	ClothVertex *verts = cloth->verts;
	int numverts = cloth->numverts;
	const float spf = (float)clmd->sim_parms->stepsPerFrame / clmd->sim_parms->timescale;;
	
	bool do_extra_solve;
	int i;
	
	if (!(clmd->coll_parms->flags & CLOTH_COLLSETTINGS_FLAG_ENABLED))
		return;
	if (!clmd->clothObject->bvhtree)
		return;
	
	// update verts to current positions
	for (i = 0; i < numverts; i++) {
		BPH_mass_spring_get_new_position(id, i, verts[i].tx);
		
		sub_v3_v3v3(verts[i].tv, verts[i].tx, verts[i].txold);
		copy_v3_v3(verts[i].v, verts[i].tv);
	}
	
#if 0 /* unused */
	for (i=0, cv=cloth->verts; i<cloth->numverts; i++, cv++) {
		copy_v3_v3(initial_cos[i], cv->tx);
	}
#endif
	
	// call collision function
	// TODO: check if "step" or "step+dt" is correct - dg
	do_extra_solve = cloth_bvh_objcollision(ob, clmd, step / clmd->sim_parms->timescale, dt / clmd->sim_parms->timescale);
	
	// copy corrected positions back to simulation
	for (i = 0; i < numverts; i++) {
		float curx[3];
		BPH_mass_spring_get_position(id, i, curx);
		// correct velocity again, just to be sure we had to change it due to adaptive collisions
		sub_v3_v3v3(verts[i].tv, verts[i].tx, curx);
	}
	
	if (do_extra_solve) {
//		cloth_calc_helper_forces(ob, clmd, initial_cos, step/clmd->sim_parms->timescale, dt/clmd->sim_parms->timescale);
		
		for (i = 0; i < numverts; i++) {
		
			float newv[3];
			
			if ((clmd->sim_parms->flags & CLOTH_SIMSETTINGS_FLAG_GOAL) && (verts [i].flags & CLOTH_VERT_FLAG_PINNED))
				continue;
			
			BPH_mass_spring_set_new_position(id, i, verts[i].tx);
			mul_v3_v3fl(newv, verts[i].tv, spf);
			BPH_mass_spring_set_new_velocity(id, i, newv);
		}
	}
	
	// X = Xnew;
	BPH_mass_spring_apply_result(id);
	
	if (do_extra_solve) {
		ImplicitSolverResult result;
		
		/* initialize forces to zero */
		BPH_mass_spring_clear_forces(id);
		
		// calculate forces
		cloth_calc_force(clmd, frame, effectors, step);
		
		// calculate new velocity and position
		BPH_mass_spring_solve_velocities(id, dt, &result);
//		cloth_record_result(clmd, &result, clmd->sim_parms->stepsPerFrame);
		
		/* note: positions are advanced only once in the main solver step! */
		
		BPH_mass_spring_apply_result(id);
	}
}

static void cloth_clear_result(ClothModifierData *clmd)
{
	ClothSolverResult *sres = clmd->solver_result;
	
	sres->status = 0;
	sres->max_error = sres->min_error = sres->avg_error = 0.0f;
	sres->max_iterations = sres->min_iterations = 0;
	sres->avg_iterations = 0.0f;
}

static void cloth_record_result(ClothModifierData *clmd, ImplicitSolverResult *result, int steps)
{
	ClothSolverResult *sres = clmd->solver_result;
	
	if (sres->status) { /* already initialized ? */
		/* error only makes sense for successful iterations */
		if (result->status == BPH_SOLVER_SUCCESS) {
			sres->min_error = min_ff(sres->min_error, result->error);
			sres->max_error = max_ff(sres->max_error, result->error);
			sres->avg_error += result->error / (float)steps;
		}
		
		sres->min_iterations = min_ii(sres->min_iterations, result->iterations);
		sres->max_iterations = max_ii(sres->max_iterations, result->iterations);
		sres->avg_iterations += (float)result->iterations / (float)steps;
	}
	else {
		/* error only makes sense for successful iterations */
		if (result->status == BPH_SOLVER_SUCCESS) {
			sres->min_error = sres->max_error = result->error;
			sres->avg_error += result->error / (float)steps;
		}
		
		sres->min_iterations = sres->max_iterations  = result->iterations;
		sres->avg_iterations += (float)result->iterations / (float)steps;
	}
	
	sres->status |= result->status;
}

int BPH_cloth_solve(Object *ob, float frame, ClothModifierData *clmd, ListBase *effectors)
{
	/* Hair currently is a cloth sim in disguise ...
	 * Collision detection and volumetrics work differently then.
	 * Bad design, TODO
	 */
	const bool is_hair = (clmd->hairdata != NULL);
	
	unsigned int i=0;
	float step=0.0f, tf=clmd->sim_parms->timescale;
	Cloth *cloth = clmd->clothObject;
	ClothVertex *verts = cloth->verts, *vert;
	unsigned int numverts = cloth->numverts;
	float dt = clmd->sim_parms->timescale / clmd->sim_parms->stepsPerFrame;
	Implicit_Data *id = cloth->implicit;
	ColliderContacts *contacts = NULL;
	int totcolliders = 0;
	
	BKE_sim_debug_data_clear_category("collision");
	
	if (!clmd->solver_result)
		clmd->solver_result = (ClothSolverResult *)MEM_callocN(sizeof(ClothSolverResult), "cloth solver result");
	cloth_clear_result(clmd);
	
	if (clmd->sim_parms->flags & CLOTH_SIMSETTINGS_FLAG_GOAL) { /* do goal stuff */
		vert = verts;
		for (i = 0; i < numverts; i++, vert++) {
			// update velocities with constrained velocities from pinned verts
			if (vert->flags & CLOTH_VERT_FLAG_PINNED) {
				float v[3];
				
				sub_v3_v3v3(v, verts[i].xconst, verts[i].xold);
				// mul_v3_fl(v, clmd->sim_parms->stepsPerFrame);
				BPH_mass_spring_set_velocity(id, i, v);
			}
		}
	}
	
	while (step < tf) {
		ImplicitSolverResult result;
		
		/* copy velocities for collision */
		vert = verts;
		for (i = 0; i < numverts; i++, vert++) {
			BPH_mass_spring_get_motion_state(id, i, NULL, verts[i].tv);
			copy_v3_v3(verts[i].v, verts[i].tv);
		}
		
		if (is_hair) {
			/* determine contact points */
			if (clmd->coll_parms->flags & CLOTH_COLLSETTINGS_FLAG_ENABLED) {
				cloth_find_point_contacts(ob, clmd, 0.0f, tf, &contacts, &totcolliders);
			}
			
			/* setup vertex constraints for pinned vertices and contacts */
			cloth_setup_constraints(clmd, contacts, totcolliders, dt);
		}
		else {
			/* setup vertex constraints for pinned vertices */
			cloth_setup_constraints(clmd, NULL, 0, dt);
		}
		
		/* initialize forces to zero */
		BPH_mass_spring_clear_forces(id);
		
		// damping velocity for artistic reasons
		// this is a bad way to do it, should be removed imo - lukas_t
		if (clmd->sim_parms->vel_damping != 1.0f) {
			vert = verts;
			for (i = 0; i < numverts; i++, vert++) {
				float v[3];
				BPH_mass_spring_get_motion_state(id, i, NULL, v);
				mul_v3_fl(v, clmd->sim_parms->vel_damping);
				BPH_mass_spring_set_velocity(id, i, v);
			}
		}
		
		// calculate forces
		cloth_calc_force(clmd, frame, effectors, step);
		
		// calculate new velocity and position
		BPH_mass_spring_solve_velocities(id, dt, &result);
		cloth_record_result(clmd, &result, clmd->sim_parms->stepsPerFrame);
		
		if (is_hair) {
			cloth_continuum_step(clmd, dt);
		}
		
		BPH_mass_spring_solve_positions(id, dt);
		
		if (!is_hair) {
			cloth_collision_solve_extra(ob, clmd, effectors, frame, step, dt);
		}
		
		BPH_mass_spring_apply_result(id);
		
		/* move pinned verts to correct position */
		vert = verts;
		for (i = 0; i < numverts; i++, vert++) {
			if (clmd->sim_parms->flags & CLOTH_SIMSETTINGS_FLAG_GOAL) {
				if (vert->flags & CLOTH_VERT_FLAG_PINNED) {
					float x[3];
					interp_v3_v3v3(x, verts[i].xold, verts[i].xconst, step + dt);
					BPH_mass_spring_set_position(id, i, x);
				}
			}
			
			BPH_mass_spring_get_motion_state(id, i, verts[i].txold, NULL);
		}
		
		/* free contact points */
		if (contacts) {
			cloth_free_contacts(contacts, totcolliders);
		}
		
		step += dt;
	}
	
	/* copy results back to cloth data */
	vert = verts;
	for (i = 0; i < numverts; i++, vert++) {
		BPH_mass_spring_get_motion_state(id, i, vert->x, vert->v);
		copy_v3_v3(vert->txold, vert->x);
	}
	
	return 1;
}

bool BPH_cloth_solver_get_texture_data(Object *UNUSED(ob), ClothModifierData *clmd, VoxelData *vd)
{
	Cloth *cloth = clmd->clothObject;
	HairGrid *grid;
	float gmin[3], gmax[3];
	
	if (!clmd->clothObject || !clmd->clothObject->implicit)
		return false;
	
	hair_get_boundbox(clmd, gmin, gmax);
	
	grid = BPH_hair_volume_create_vertex_grid(clmd->sim_parms->voxel_cell_size, gmin, gmax);
	cloth_continuum_fill_grid(grid, cloth);
	
	BPH_hair_volume_get_texture_data(grid, vd);
	
	BPH_hair_volume_free_vertex_grid(grid);
	
	return true;
}

/* ========================================================================= */

struct Implicit_Data *BPH_strands_solver_create(struct Strands *strands, struct HairSimParams *UNUSED(params))
{
	static float I3[3][3] = { {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f} };
	
	struct Implicit_Data *id;
	int numverts = strands->totverts;
	int numcurves = strands->totcurves;
	int numedges = max_ii(numverts - numcurves, 0);
	int numbends = max_ii(numverts - 2*numcurves, 0);
	
	/* goal springs:    1 per vertex, except roots
	 * stretch springs: 1 per edge
	 * bending sprints: 3 per bend // XXX outdated, 1 is enough
	 */
	int numsprings = (numverts-numcurves) + numedges + 3*numbends;
	int i;
	
	id = BPH_mass_spring_solver_create(numverts, numsprings);
	
	for (i = 0; i < numverts; i++) {
		// TODO define mass
		float mass = 1.0f;
		BPH_mass_spring_set_vertex_mass(id, i, mass);
	}
	
	for (i = 0; i < numverts; i++) {
		BPH_mass_spring_set_rest_transform(id, i, I3);
	}
	
	return id;
}

/* Init constraint matrix
 * This is part of the modified CG method suggested by Baraff/Witkin in
 * "Large Steps in Cloth Simulation" (Siggraph 1998)
 */
static void strands_setup_constraints(Strands *strands, Implicit_Data *data, ColliderContacts *UNUSED(contacts), int UNUSED(totcolliders), float UNUSED(dt))
{
	static const float ZERO[3] = { 0.0f, 0.0f, 0.0f };
	
	BPH_mass_spring_clear_constraints(data);
	
	StrandIterator it_strand;
	for (BKE_strand_iter_init(&it_strand, strands); BKE_strand_iter_valid(&it_strand); BKE_strand_iter_next(&it_strand)) {
		int index = BKE_strand_iter_vertex_offset(strands, &it_strand);
		
		/* pin strand roots */
		BPH_mass_spring_add_constraint_ndof0(data, index, ZERO); /* velocity is defined externally */
		
//		StrandVertexIterator it_vert;
//		for (BKE_strand_vertex_iter_init(&it_vert, &it_strand); BKE_strand_vertex_iter_valid(&it_vert); BKE_strand_vertex_iter_next(&it_vert)) {
//		}
	}

#if 0
	for (i = 0; i < totcolliders; ++i) {
		ColliderContacts *ct = &contacts[i];
		for (j = 0; j < ct->totcollisions; ++j) {
			CollPair *collpair = &ct->collisions[j];
//			float restitution = (1.0f - clmd->coll_parms->damping) * (1.0f - ct->ob->pd->pdef_sbdamp);
			float restitution = 0.0f;
			int v = collpair->face1;
			float impulse[3];
			
			/* pinned verts handled separately */
			if (verts[v].flags & CLOTH_VERT_FLAG_PINNED)
				continue;
			
			/* XXX cheap way of avoiding instability from multiple collisions in the same step
			 * this should eventually be supported ...
			 */
			if (verts[v].impulse_count > 0)
				continue;
			
			/* calculate collision response */
			if (!collision_response(clmd, ct->collmd, collpair, dt, restitution, impulse))
				continue;
			
			BPH_mass_spring_add_constraint_ndof2(data, v, collpair->normal, impulse);
			++verts[v].impulse_count;
		}
	}
#endif
}

/* stretch forces are created between 2 vertices of each segment */
static void strands_calc_curve_stretch_forces(Strands *strands, float UNUSED(space[4][4]), HairSimParams *params, Implicit_Data *data, StrandIterator *it_strand)
{
	StrandEdgeIterator it_edge;
	
	for (BKE_strand_edge_iter_init(&it_edge, it_strand); BKE_strand_edge_iter_valid(&it_edge); BKE_strand_edge_iter_next(&it_edge)) {
		int vi = BKE_strand_edge_iter_vertex0_offset(strands, &it_edge);
		int vj = BKE_strand_edge_iter_vertex1_offset(strands, &it_edge);
		float restlen = len_v3v3(it_edge.vertex0->co, it_edge.vertex1->co);
		
		float stiffness = params->stretch_stiffness;
		float damping = stiffness * params->stretch_damping;
		BPH_mass_spring_force_spring_linear(data, vi, vj, restlen, stiffness, damping, true, 0.0f, NULL, NULL, NULL);
	}
}

/* bending forces aim to restore the rest shape of each strand locally */
static void strands_calc_curve_bending_forces(Strands *strands, float space[4][4], HairSimParams *params, Implicit_Data *data, StrandIterator *it_strand)
{
	StrandBendIterator it_bend;
	
	const float stiffness = params->bend_stiffness;
	const float damping = stiffness * params->bend_damping;
	
	BKE_strand_bend_iter_init(&it_bend, it_strand);
	if (!BKE_strand_bend_iter_valid(&it_bend))
		return;
	
	/* The 'mat' matrix (here: A) contains the relative transform between the local rest and motion state coordinate systems.
	 * In the beginning both systems are the root matrix R, so the relative transform is the unit matrix.
	 * 
	 * A = M_state * M_rest^T
	 *   = R * R^T
	 *   = I
	 * 
	 * With each bend the matrices are rotated along the curvature, described by matrix B^T. Since we are only
	 * interested in the combined transform however, the resulting operation becomes
	 * 
	 * A' = M_state' * M_rest'
	 *    = (B_state^T * M_state) * (B_rest^T * M_rest)^T
	 *    = B_state^T * M_state * M_rest^T * B_rest
	 *    = B_state^T * A * B_rest
	 * 
	 * The target vector is originally the direction of the first segment. For each bend, the target vector
	 * is the _previous_ segment's direction, i.e. the target vector is rotated by B with a 1-step delay.
	 * 
	 * The target vector in the current motion state system for each segment could thus be calculated by multiplying
	 * 
	 * t_state = M * t_rest
	 * 
	 * but using the edge vector directly is more practical.
	 * 
	 */
	float mat[3][3];
//	float Mrest[3][3], Mstate[3][3];
	
	{ /* initialize using the first edge deviation from the rest direction */
		float edge_rest[3], edge_state[3], rot[3][3];
		sub_v3_v3v3(edge_rest, it_strand->verts[1].co, it_strand->verts[0].co);
		sub_v3_v3v3(edge_state, it_strand->state[1].co, it_strand->state[0].co);
		normalize_v3(edge_rest);
		normalize_v3(edge_state);
		rotation_between_vecs_to_mat3(rot, edge_rest, edge_state);
		
		copy_m3_m3(mat, rot);
//		copy_m3_m3(Mrest, it_strand->curve->root_matrix);
//		mul_m3_m3m3(Mstate, rot, Mrest);
	}
	
	{ /* apply force */
		/* Note: applying forces to the first segment is necessary to equalize forces on the root,
		 * otherwise energy gets introduced at the root and can destabilize the simulation.
		 */
		float target[3];
		sub_v3_v3v3(target, it_strand->verts[1].co, it_strand->verts[0].co);
		mul_mat3_m4_v3(space, target); /* to solver space (world space) */
		
		float target_state[3];
		mul_v3_m3v3(target_state, mat, target);
		
		int vroot = BKE_strand_bend_iter_vertex0_offset(strands, &it_bend); /* root velocity used as goal velocity */
		int vj = BKE_strand_bend_iter_vertex1_offset(strands, &it_bend);
		float goal[3], rootvel[3];
		mul_v3_m4v3(goal, space, it_strand->verts[1].co);
		BPH_mass_spring_get_velocity(data, vroot, rootvel);
		BPH_mass_spring_force_spring_goal(data, vj, goal, rootvel, stiffness, damping, NULL, NULL, NULL);
	}
	
	do {
		{ /* advance the coordinate frame */
			float rotrest[3][3], rotrest_inv[3][3], rotstate[3][3], rotstate_inv[3][3];
			BKE_strand_bend_iter_transform_rest(&it_bend, rotrest);
			BKE_strand_bend_iter_transform_state(&it_bend, rotstate);
			transpose_m3_m3(rotrest_inv, rotrest);
			transpose_m3_m3(rotstate_inv, rotstate);
			
//			mul_m3_m3m3(Mrest, rotrest_inv, Mrest);
//			mul_m3_m3m3(Mstate, rotstate_inv, Mstate);
			
			mul_m3_m3m3(mat, mat, rotrest);
			mul_m3_m3m3(mat, rotstate_inv, mat);
		}
		
		{ /* apply force */
			float target[3];
			sub_v3_v3v3(target, it_bend.vertex1->co, it_bend.vertex0->co);
			mul_mat3_m4_v3(space, target); /* to solver space (world space) */
			
			float target_state[3];
			mul_v3_m3v3(target_state, mat, target);
			
			int vi = BKE_strand_bend_iter_vertex0_offset(strands, &it_bend);
			int vj = BKE_strand_bend_iter_vertex1_offset(strands, &it_bend);
			int vk = BKE_strand_bend_iter_vertex2_offset(strands, &it_bend);
			BPH_mass_spring_force_spring_bending_angular(data, vi, vj, vk, target_state, stiffness, damping);
			
#if 0 /* debug */
			{
				float mscale = 0.1f;
				
				float x[3];
				BPH_mass_spring_get_position(data, vj, x);
				BKE_sim_debug_data_add_vector(x, target, 0,0,1, "hairsim", 2598, vi, vj, vk);
				BKE_sim_debug_data_add_vector(x, target_state, 0.4,0.4,1, "hairsim", 2599, vi, vj, vk);
				
				float mr[3][3];
				copy_m3_m3(mr, Mrest);
				mul_m3_fl(mr, mscale);
				BKE_sim_debug_data_add_vector(x, mr[0], 0.7,0.0,0.0, "hairsim", 1957, vi, vj, vk);
				BKE_sim_debug_data_add_vector(x, mr[1], 0.0,0.7,0.0, "hairsim", 1958, vi, vj, vk);
				BKE_sim_debug_data_add_vector(x, mr[2], 0.0,0.0,0.7, "hairsim", 1959, vi, vj, vk);
				
				float ms[3][3];
				copy_m3_m3(ms, Mstate);
				mul_m3_fl(ms, mscale);
				BKE_sim_debug_data_add_vector(x, ms[0], 1.0,0.4,0.4, "hairsim", 1857, vi, vj, vk);
				BKE_sim_debug_data_add_vector(x, ms[1], 0.4,1.0,0.4, "hairsim", 1858, vi, vj, vk);
				BKE_sim_debug_data_add_vector(x, ms[2], 0.4,0.4,1.0, "hairsim", 1859, vi, vj, vk);
			}
#endif
		}
		
		BKE_strand_bend_iter_next(&it_bend);
	} while (BKE_strand_bend_iter_valid(&it_bend));
}

static float strands_goal_stiffness(Strands *UNUSED(strands), HairSimParams *params, StrandsVertex *vert, float t)
{
	/* XXX There is no possibility of tweaking them in linked data currently,
	 * so the original workflow of painting weights in particle edit mode is virtually useless.
	 */
	float weight;
	
	if (params->flag & eHairSimParams_Flag_UseGoalStiffnessCurve)
		weight = curvemapping_evaluateF(params->goal_stiffness_mapping, 0, t);
	else
		weight = vert->weight;
	CLAMP(weight, 0.0f, 1.0f);
	
	return params->goal_stiffness * weight;
}

/* goal forces pull vertices toward their rest position */
static void strands_calc_vertex_goal_forces(Strands *strands, float space[4][4], HairSimParams *params, Implicit_Data *data, StrandIterator *it_strand)
{
	StrandEdgeIterator it_edge;
	
	float rootvel[3];
	BPH_mass_spring_get_velocity(data, BKE_strand_iter_vertex_offset(strands, it_strand), rootvel);
	
	float length = 0.0f;
	for (BKE_strand_edge_iter_init(&it_edge, it_strand); BKE_strand_edge_iter_valid(&it_edge); BKE_strand_edge_iter_next(&it_edge))
		length += len_v3v3(it_edge.vertex1->co, it_edge.vertex0->co);
	float length_inv = length > 0.0f ? 1.0f / length : 0.0f;
	
	float t = 0.0f;
	for (BKE_strand_edge_iter_init(&it_edge, it_strand); BKE_strand_edge_iter_valid(&it_edge); BKE_strand_edge_iter_next(&it_edge)) {
		int vj = BKE_strand_edge_iter_vertex1_offset(strands, &it_edge);
		t += len_v3v3(it_edge.vertex1->co, it_edge.vertex0->co);
		
		float stiffness = strands_goal_stiffness(strands, params, it_edge.vertex1, t * length_inv);
		float damping = stiffness * params->goal_damping;
		
		float goal[3];
		mul_v3_m4v3(goal, space, it_edge.vertex1->co);
		
		BPH_mass_spring_force_spring_goal(data, vj, goal, rootvel, stiffness, damping, NULL, NULL, NULL);
	}
}

/* calculates internal forces for a single strand curve */
static void strands_calc_curve_forces(Strands *strands, float space[4][4], HairSimParams *params, Implicit_Data *data, StrandIterator *it_strand)
{
	strands_calc_curve_stretch_forces(strands, space, params, data, it_strand);
	strands_calc_curve_bending_forces(strands, space, params, data, it_strand);
	strands_calc_vertex_goal_forces(strands, space, params, data, it_strand);
}

/* Collect forces and derivatives:  F, dFdX, dFdV */
static void strands_calc_force(Strands *strands, float space[4][4], HairSimParams *params, Implicit_Data *data, float UNUSED(frame), Scene *scene, ListBase * /*effectors*/)
{
	unsigned int numverts = strands->totverts;
	
	int i = 0;
//	float drag = params->Cvi * 0.01f; /* viscosity of air scaled in percent */
	float gravity[3] = {0.0f, 0.0f, 0.0f};
	
	/* global acceleration (gravitation) */
	if (scene->physics_settings.flag & PHYS_GLOBAL_GRAVITY) {
		/* scale gravity force */
		mul_v3_v3fl(gravity, scene->physics_settings.gravity, params->effector_weights->global_gravity);
	}
	for (i = 0; i < numverts; i++) {
		float mass = 1.0f; // TODO
		BPH_mass_spring_force_gravity(data, i, mass, gravity);
	}

#if 0
	BPH_mass_spring_force_drag(data, drag);
#endif
	
	/* handle external forces like wind */
	if (effectors) {
		/* cache per-vertex forces to avoid redundant calculation */
		float (*ext_forces)[3] = (float (*)[3])MEM_callocN(sizeof(float) * 3 * numverts, "effector forces");
		for (i = 0; i < numverts; ++i) {
			float x[3], v[3];
			EffectedPoint epoint;
			
			BPH_mass_spring_get_motion_state(data, i, x, v);
			pd_point_from_loc(scene, x, v, i, &epoint);
			pdDoEffectors(effectors, NULL, params->effector_weights, &epoint, ext_forces[i], NULL);
		}
		
		for (i = 0; i < numverts; ++i) {
			BPH_mass_spring_force_vertex_wind(data, i, 1.0f, ext_forces);
		}

		MEM_freeN(ext_forces);
	}
	
	/* spring forces */
	StrandIterator it_strand;
	for (BKE_strand_iter_init(&it_strand, strands); BKE_strand_iter_valid(&it_strand); BKE_strand_iter_next(&it_strand)) {
		strands_calc_curve_forces(strands, space, params, data, &it_strand);
	}
}

/* calculates the velocity of strand roots using the new rest location (verts->co) and the current motion state */
static void strands_calc_root_velocity(Strands *strands, float mat[4][4], Implicit_Data *data, float timestep)
{
	StrandIterator it_strand;
	for (BKE_strand_iter_init(&it_strand, strands); BKE_strand_iter_valid(&it_strand); BKE_strand_iter_next(&it_strand)) {
		if (it_strand.curve->numverts > 0) {
			int index = BKE_strand_iter_vertex_offset(strands, &it_strand);
			
			float vel[3];
			sub_v3_v3v3(vel, it_strand.verts[0].co, it_strand.state[0].co);
			mul_v3_fl(vel, 1.0f/timestep);
			mul_mat3_m4_v3(mat, vel);
			
			BPH_mass_spring_set_velocity(data, index, vel);
		}
	}
}

/* calculates the location of strand roots using the new rest location (verts->co) and the current motion state */
static void strands_calc_root_location(Strands *strands, float mat[4][4], Implicit_Data *data, float step)
{
	StrandIterator it_strand;
	for (BKE_strand_iter_init(&it_strand, strands); BKE_strand_iter_valid(&it_strand); BKE_strand_iter_next(&it_strand)) {
		if (it_strand.curve->numverts > 0) {
			int index = BKE_strand_iter_vertex_offset(strands, &it_strand);
			
			float co[3];
			interp_v3_v3v3(co, it_strand.state[0].co, it_strand.verts[0].co, step);
			mul_m4_v3(mat, co);
			
			BPH_mass_spring_set_position(data, index, co);
		}
	}
}

/* XXX Do we need to take fictitious forces from the moving and/or accelerated frame of reference into account?
 * This would mean we pass not only the basic world transform mat, but also linear/angular velocity and acceleration.
 */
bool BPH_strands_solve(Strands *strands, float mat[4][4], Implicit_Data *id, HairSimParams *params, float frame, float frame_prev, Scene *scene, ListBase *effectors)
{
	if (params->timescale == 0.0f || params->substeps < 1)
		return false;
	
	float timestep = (FRA2TIME(frame) - FRA2TIME(frame_prev)) * params->timescale;
	float dstep = 1.0f / params->substeps;
	float dtime = timestep * dstep;
	int numverts = strands->totverts;
	
	int i;
	ColliderContacts *contacts = NULL;
	int totcolliders = 0;
	
	float imat[4][4];
	invert_m4_m4(imat, mat);
	
//	if (!clmd->solver_result)
//		clmd->solver_result = (ClothSolverResult *)MEM_callocN(sizeof(ClothSolverResult), "cloth solver result");
//	cloth_clear_result(clmd);
	
	/* initialize solver data */
	for (i = 0; i < numverts; i++) {
		float wco[3], wvel[3];
		copy_v3_v3(wco, strands->state[i].co);
		copy_v3_v3(wvel, strands->state[i].vel);
		mul_m4_v3(mat, wco);
		mul_mat3_m4_v3(mat, wvel);
		BPH_mass_spring_set_motion_state(id, i, wco, wvel);
	}
	strands_calc_root_velocity(strands, mat, id, timestep);
	
	for (float step = 0.0f; step < 1.0f; step += dstep) {
		ImplicitSolverResult result;
		
#if 0
		/* determine contact points */
		if (clmd->coll_parms->flags & CLOTH_COLLSETTINGS_FLAG_ENABLED) {
			cloth_find_point_contacts(ob, clmd, 0.0f, tf, &contacts, &totcolliders);
		}
#endif
		
		/* setup vertex constraints for pinned vertices and contacts */
		strands_setup_constraints(strands, id, contacts, totcolliders, dtime);
		
		/* initialize forces to zero */
		BPH_mass_spring_clear_forces(id);
		
		// calculate forces
		strands_calc_force(strands, mat, params, id, frame, scene, effectors);
		
		// calculate new velocity and position
		BPH_mass_spring_solve_velocities(id, dtime, &result);
//		cloth_record_result(clmd, &result, clmd->sim_parms->stepsPerFrame);
		
#if 0
		if (is_hair) {
			cloth_continuum_step(clmd, dtime);
		}
#endif
		
		BPH_mass_spring_solve_positions(id, dtime);
		
		BPH_mass_spring_apply_result(id);
		
		/* move pinned verts to correct position */
		strands_calc_root_location(strands, mat, id, step + dstep);
		
#if 0
		/* free contact points */
		if (contacts) {
			cloth_free_contacts(contacts, totcolliders);
		}
#endif
		
		step += dstep;
	}
	
	/* copy results back to strand data */
	for (i = 0; i < numverts; i++) {
		float co[3], vel[3];
		BPH_mass_spring_get_motion_state(id, i, co, vel);
		mul_m4_v3(imat, co);
		mul_mat3_m4_v3(imat, vel);
		copy_v3_v3(strands->state[i].co, co);
		copy_v3_v3(strands->state[i].vel, vel);
	}
	
	return true;
}
