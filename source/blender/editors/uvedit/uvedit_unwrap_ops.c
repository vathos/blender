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
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/editors/uvedit/uvedit_unwrap_ops.c
 *  \ingroup eduv
 */


#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "MEM_guardedalloc.h"

#include "DNA_camera_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_modifier_types.h"

#include "BLI_utildefines.h"
#include "BLI_alloca.h"
#include "BLI_math.h"
#include "BLI_uvproject.h"
#include "BLI_string.h"

#include "BLT_translation.h"

#include "BKE_cdderivedmesh.h"
#include "BKE_subsurf.h"
#include "BKE_context.h"
#include "BKE_customdata.h"
#include "BKE_depsgraph.h"
#include "BKE_image.h"
#include "BKE_main.h"
#include "BKE_report.h"
#include "BKE_scene.h"
#include "BKE_editmesh.h"
#include "BKE_deform.h"

#include "PIL_time.h"

#include "UI_interface.h"

#include "ED_image.h"
#include "ED_mesh.h"
#include "ED_screen.h"
#include "ED_uvedit.h"
#include "ED_view3d.h"

#include "RNA_access.h"
#include "RNA_define.h"

#include "WM_api.h"
#include "WM_types.h"

#include "uvedit_intern.h"
#include "uvedit_parametrizer.h"

static bool ED_uvedit_ensure_uvs(bContext *C, Scene *scene, Object *obedit)
{
	Main *bmain = CTX_data_main(C);
	BMEditMesh *em = BKE_editmesh_from_object(obedit);
	BMFace *efa;
	BMIter iter;
	Image *ima;
	bScreen *sc;
	ScrArea *sa;
	SpaceLink *slink;
	SpaceImage *sima;
	int cd_loop_uv_offset;

	if (ED_uvedit_test(obedit))
		return 1;

	if (em && em->bm->totface && !CustomData_has_layer(&em->bm->pdata, CD_MTEXPOLY))
		ED_mesh_uv_texture_add(obedit->data, NULL, true);

	if (!ED_uvedit_test(obedit))
		return 0;

	cd_loop_uv_offset = CustomData_get_offset(&em->bm->ldata, CD_MLOOPUV);

	ima = CTX_data_edit_image(C);

	if (!ima) {
		/* no image in context in the 3d view, we find first image window .. */
		sc = CTX_wm_screen(C);

		for (sa = sc->areabase.first; sa; sa = sa->next) {
			slink = sa->spacedata.first;
			if (slink->spacetype == SPACE_IMAGE) {
				sima = (SpaceImage *)slink;

				ima = sima->image;
				if (ima) {
					if (ima->type == IMA_TYPE_R_RESULT || ima->type == IMA_TYPE_COMPOSITE)
						ima = NULL;
					else
						break;
				}
			}
		}
	}
	
	if (ima)
		ED_uvedit_assign_image(bmain, scene, obedit, ima, NULL);
	
	/* select new UV's (ignore UV_SYNC_SELECTION in this case) */
	BM_ITER_MESH (efa, &iter, em->bm, BM_FACES_OF_MESH) {
		BMIter liter;
		BMLoop *l;

		BM_ITER_ELEM (l, &liter, efa, BM_LOOPS_OF_FACE) {
			MLoopUV *luv = BM_ELEM_CD_GET_VOID_P(l, cd_loop_uv_offset);
			luv->flag |= MLOOPUV_VERTSEL;
		}
	}

	return 1;
}

/******************* Remembered Properties ******************/

typedef struct UnwrapProperties {
	bool use_slim;
	bool use_abf;
	bool correct_aspect;
	bool fill_holes;
	bool use_subsurf;
	char vertex_group[MAX_ID_NAME];
	float vertex_group_factor;
	float relative_scale;
	int reflection_mode;
	int iterations;
} UnwrapProperties;

static UnwrapProperties unwrap_properties_get(wmOperator *op, Object *ob)
{
	/* We use the properties from the last unwrap operator for subsequent
	 * live unwrap and minize stretch operators. */
	PointerRNA ptr;
	WM_operator_last_properties_alloc(op, "UV_OT_unwrap", &ptr);

	UnwrapProperties unwrap;
	unwrap.use_abf = RNA_enum_get(&ptr, "method") == 0;
	unwrap.use_slim = RNA_enum_get(&ptr, "method") == 2;
	unwrap.correct_aspect = RNA_boolean_get(&ptr, "correct_aspect");
	unwrap.fill_holes = RNA_boolean_get(&ptr, "fill_holes");
	unwrap.use_subsurf = RNA_boolean_get(&ptr, "use_subsurf_data");
	RNA_string_get(&ptr, "vertex_group", unwrap.vertex_group);
	unwrap.vertex_group_factor = RNA_float_get(&ptr, "vertex_group_factor");
	unwrap.relative_scale = RNA_float_get(&ptr, "relative_scale");
	unwrap.reflection_mode = RNA_enum_get(&ptr, "reflection_mode");
	unwrap.iterations = RNA_int_get(&ptr, "iterations");

	/* SLIM requires hole filling */
	if (unwrap.use_slim) {
		unwrap.fill_holes = true;
	}

	/* Subsurf will take the modifier settings only if modifier is first or right after mirror */
	if (unwrap.use_subsurf) {
		ModifierData *md = ob->modifiers.first;

		if (!(md && md->type == eModifierType_Subsurf)) {
			unwrap.use_subsurf = false;
			if (op)
				BKE_report(op->reports, RPT_INFO, "Subdivision Surface modifier needs to be first to work with unwrap");
		}
	}

	WM_operator_properties_free(&ptr);

	return unwrap;
}

/****************** Parametrizer Conversion ***************/

static bool uvedit_have_selection(Scene *scene, BMEditMesh *em, bool implicit)
{
	BMFace *efa;
	BMLoop *l;
	BMIter iter, liter;
	const int cd_loop_uv_offset  = CustomData_get_offset(&em->bm->ldata, CD_MLOOPUV);
	
	if (cd_loop_uv_offset == -1) {
		return (em->bm->totfacesel != 0);
	}

	/* verify if we have any selected uv's before unwrapping,
	 * so we can cancel the operator early */
	BM_ITER_MESH (efa, &iter, em->bm, BM_FACES_OF_MESH) {
		if (scene->toolsettings->uv_flag & UV_SYNC_SELECTION) {
			if (BM_elem_flag_test(efa, BM_ELEM_HIDDEN))
				continue;
		}
		else if (!BM_elem_flag_test(efa, BM_ELEM_SELECT))
			continue;
	
		BM_ITER_ELEM (l, &liter, efa, BM_LOOPS_OF_FACE) {
			if (uvedit_uv_select_test(scene, l, cd_loop_uv_offset))
				break;
		}
		
		if (implicit && !l)
			continue;
		
		return true;
	}

	return false;
}

void ED_uvedit_get_aspect(Scene *scene, Object *ob, BMesh *bm, float *aspx, float *aspy)
{
	bool sloppy = true;
	bool selected = false;
	BMFace *efa;
	Image *ima;

	efa = BM_mesh_active_face_get(bm, sloppy, selected);

	if (efa) {
		if (BKE_scene_use_new_shading_nodes(scene)) {
			ED_object_get_active_image(ob, efa->mat_nr + 1, &ima, NULL, NULL, NULL);
		}
		else {
			MTexPoly *tf = CustomData_bmesh_get(&bm->pdata, efa->head.data, CD_MTEXPOLY);
			ima = tf->tpage;
		}

		ED_image_get_uv_aspect(ima, NULL, aspx, aspy);
	}
	else {
		*aspx = 1.0f;
		*aspy = 1.0f;
	}
}

static void construct_param_handle_face_add(ParamHandle *handle, Scene *scene,
                                            BMFace *efa, int face_index,
											const int cd_loop_uv_offset,
											const int cd_weight_offset,
											const int cd_weight_index)
{
	ParamKey key;
	ParamKey *vkeys = BLI_array_alloca(vkeys, efa->len);
	ParamBool *pin = BLI_array_alloca(pin, efa->len);
	ParamBool *select = BLI_array_alloca(select, efa->len);
	float **co = BLI_array_alloca(co, efa->len);
	float **uv = BLI_array_alloca(uv, efa->len);
	float *weight = BLI_array_alloca(weight, efa->len);
	int i;

	BMIter liter;
	BMLoop *l;

	key = (ParamKey)face_index;

	/* let parametrizer split the ngon, it can make better decisions
	 * about which split is best for unwrapping than scanfill */
	BM_ITER_ELEM_INDEX (l, &liter, efa, BM_LOOPS_OF_FACE, i) {
		MLoopUV *luv = BM_ELEM_CD_GET_VOID_P(l, cd_loop_uv_offset);

		vkeys[i] = (ParamKey)BM_elem_index_get(l->v);
		co[i] = l->v->co;
		uv[i] = luv->uv;
		pin[i] = (luv->flag & MLOOPUV_PINNED) != 0;
		select[i] = uvedit_uv_select_test(scene, l, cd_loop_uv_offset);

		/* optional vertex group weighting */
		if (cd_weight_offset >= 0 && cd_weight_index >= 0) {
			MDeformVert *dv = BM_ELEM_CD_GET_VOID_P(l->v, cd_weight_offset);
			weight[i] = defvert_find_weight(dv, cd_weight_index);
		}
		else {
			weight[i] = 1.0f;
		}
	}

	param_face_add(handle, key, i, vkeys, co, uv, weight, pin, select, efa->no);
}

static ParamHandle *construct_param_handle(Scene *scene, Object *ob, BMesh *bm,
                                           const bool implicit, const bool sel,
										   const UnwrapProperties *unwrap)
{
	ParamHandle *handle;
	BMFace *efa;
	BMLoop *l;
	BMEdge *eed;
	BMIter iter, liter;
	int i;
	
	const int cd_loop_uv_offset = CustomData_get_offset(&bm->ldata, CD_MLOOPUV);
	const int cd_weight_offset = CustomData_get_offset(&bm->vdata, CD_MDEFORMVERT);
	const int cd_weight_index = defgroup_name_index(ob, unwrap->vertex_group);

	handle = param_construct_begin();

	if (unwrap->correct_aspect) {
		float aspx, aspy;

		ED_uvedit_get_aspect(scene, ob, bm, &aspx, &aspy);

		if (aspx != aspy)
			param_aspect_ratio(handle, aspx, aspy);
	}
	
	/* we need the vert indices */
	BM_mesh_elem_index_ensure(bm, BM_VERT);
	
	BM_ITER_MESH_INDEX (efa, &iter, bm, BM_FACES_OF_MESH, i) {

		if ((BM_elem_flag_test(efa, BM_ELEM_HIDDEN)) || (sel && BM_elem_flag_test(efa, BM_ELEM_SELECT) == 0)) {
			continue;
		}

		if (implicit) {
			bool is_loopsel = false;

			BM_ITER_ELEM (l, &liter, efa, BM_LOOPS_OF_FACE) {
				if (uvedit_uv_select_test(scene, l, cd_loop_uv_offset)) {
					is_loopsel = true;
					break;
				}
			}
			if (is_loopsel == false) {
				continue;
			}
		}

		construct_param_handle_face_add(handle, scene, efa, i,
			cd_loop_uv_offset, cd_weight_offset, cd_weight_index);
	}

	if (!implicit) {
		BM_ITER_MESH (eed, &iter, bm, BM_EDGES_OF_MESH) {
			if (BM_elem_flag_test(eed, BM_ELEM_SEAM)) {
				ParamKey vkeys[2];
				vkeys[0] = (ParamKey)BM_elem_index_get(eed->v1);
				vkeys[1] = (ParamKey)BM_elem_index_get(eed->v2);
				param_edge_set_seam(handle, vkeys);
			}
		}
	}

	param_construct_end(handle, unwrap->fill_holes, implicit);

	return handle;
}


static void texface_from_original_index(BMFace *efa, int index, float **uv, ParamBool *pin, ParamBool *select,
                                        Scene *scene, const int cd_loop_uv_offset)
{
	BMLoop *l;
	BMIter liter;
	MLoopUV *luv;

	*uv = NULL;
	*pin = 0;
	*select = 1;

	if (index == ORIGINDEX_NONE)
		return;

	BM_ITER_ELEM (l, &liter, efa, BM_LOOPS_OF_FACE) {
		if (BM_elem_index_get(l->v) == index) {
			luv = BM_ELEM_CD_GET_VOID_P(l, cd_loop_uv_offset);
			*uv = luv->uv;
			*pin = (luv->flag & MLOOPUV_PINNED) ? 1 : 0;
			*select = uvedit_uv_select_test(scene, l, cd_loop_uv_offset);
			break;
		}
	}
}

/* unwrap handle initialization for subsurf aware-unwrapper. The many modifications required to make the original function(see above)
 * work justified the existence of a new function. */
static ParamHandle *construct_param_handle_subsurfed(Scene *scene, Object *ob, BMEditMesh *em,
                                                     bool sel, const UnwrapProperties *unwrap)
{
	ParamHandle *handle;
	/* index pointers */
	MPoly *mpoly;
	MLoop *mloop;
	MEdge *edge;
	int i;

	/* pointers to modifier data for unwrap control */
	ModifierData *md;
	SubsurfModifierData *smd_real;
	/* modifier initialization data, will  control what type of subdivision will happen*/
	SubsurfModifierData smd = {{NULL}};
	/* Used to hold subsurfed Mesh */
	DerivedMesh *derivedMesh, *initialDerived;
	/* holds original indices for subsurfed mesh */
	const int *origVertIndices, *origEdgeIndices, *origPolyIndices;
	/* Holds vertices of subsurfed mesh */
	MVert *subsurfedVerts;
	MEdge *subsurfedEdges;
	MPoly *subsurfedPolys;
	MLoop *subsurfedLoops;
	MDeformVert *subsurfedDVerts = NULL;
	/* number of vertices and faces for subsurfed mesh*/
	int numOfEdges, numOfFaces;

	/* holds a map to editfaces for every subsurfed MFace. These will be used to get hidden/ selected flags etc. */
	BMFace **faceMap;
	/* similar to the above, we need a way to map edges to their original ones */
	BMEdge **edgeMap;

	const int cd_weight_index = defgroup_name_index(ob, unwrap->vertex_group);
	const int cd_loop_uv_offset = CustomData_get_offset(&em->bm->ldata, CD_MLOOPUV);

	handle = param_construct_begin();

	if (unwrap->correct_aspect) {
		float aspx, aspy;

		ED_uvedit_get_aspect(scene, ob, em->bm, &aspx, &aspy);

		if (aspx != aspy)
			param_aspect_ratio(handle, aspx, aspy);
	}

	/* number of subdivisions to perform */
	md = ob->modifiers.first;
	smd_real = (SubsurfModifierData *)md;

	smd.levels = smd_real->levels;
	smd.subdivType = smd_real->subdivType;
		
	initialDerived = CDDM_from_editbmesh(em, false, false);
	derivedMesh = subsurf_make_derived_from_derived(initialDerived, &smd,
	                                                NULL, SUBSURF_IN_EDIT_MODE);

	initialDerived->release(initialDerived);

	/* get the derived data */
	subsurfedVerts = derivedMesh->getVertArray(derivedMesh);
	subsurfedEdges = derivedMesh->getEdgeArray(derivedMesh);
	subsurfedPolys = derivedMesh->getPolyArray(derivedMesh);
	subsurfedLoops = derivedMesh->getLoopArray(derivedMesh);
	if (cd_weight_index >= 0)
		subsurfedDVerts = derivedMesh->getVertDataArray(derivedMesh, CD_MDEFORMVERT);

	origVertIndices = derivedMesh->getVertDataArray(derivedMesh, CD_ORIGINDEX);
	origEdgeIndices = derivedMesh->getEdgeDataArray(derivedMesh, CD_ORIGINDEX);
	origPolyIndices = derivedMesh->getPolyDataArray(derivedMesh, CD_ORIGINDEX);

	numOfEdges = derivedMesh->getNumEdges(derivedMesh);
	numOfFaces = derivedMesh->getNumPolys(derivedMesh);

	faceMap = MEM_mallocN(numOfFaces * sizeof(BMFace *), "unwrap_edit_face_map");

	BM_mesh_elem_index_ensure(em->bm, BM_VERT);
	BM_mesh_elem_table_ensure(em->bm, BM_EDGE | BM_FACE);

	/* map subsurfed faces to original editFaces */
	for (i = 0; i < numOfFaces; i++)
		faceMap[i] = BM_face_at_index(em->bm, origPolyIndices[i]);

	edgeMap = MEM_mallocN(numOfEdges * sizeof(BMEdge *), "unwrap_edit_edge_map");

	/* map subsurfed edges to original editEdges */
	for (i = 0; i < numOfEdges; i++) {
		/* not all edges correspond to an old edge */
		edgeMap[i] = (origEdgeIndices[i] != ORIGINDEX_NONE) ?
		             BM_edge_at_index(em->bm, origEdgeIndices[i]) : NULL;
	}

	/* Prepare and feed faces to the solver */
	for (i = 0, mpoly = subsurfedPolys; i < numOfFaces; i++, mpoly++) {
		ParamKey key, vkeys[4];
		ParamBool pin[4], select[4];
		float *co[4];
		float *uv[4];
		float weight[4];
		BMFace *origFace = faceMap[i];

		if (scene->toolsettings->uv_flag & UV_SYNC_SELECTION) {
			if (BM_elem_flag_test(origFace, BM_ELEM_HIDDEN))
				continue;
		}
		else {
			if (BM_elem_flag_test(origFace, BM_ELEM_HIDDEN) || (sel && !BM_elem_flag_test(origFace, BM_ELEM_SELECT)))
				continue;
		}

		mloop = &subsurfedLoops[mpoly->loopstart];

		/* We will not check for v4 here. Subsurfed mfaces always have 4 vertices. */
		BLI_assert(mpoly->totloop == 4);
		key = (ParamKey)i;
		vkeys[0] = (ParamKey)mloop[0].v;
		vkeys[1] = (ParamKey)mloop[1].v;
		vkeys[2] = (ParamKey)mloop[2].v;
		vkeys[3] = (ParamKey)mloop[3].v;

		co[0] = subsurfedVerts[mloop[0].v].co;
		co[1] = subsurfedVerts[mloop[1].v].co;
		co[2] = subsurfedVerts[mloop[2].v].co;
		co[3] = subsurfedVerts[mloop[3].v].co;

		/* Optional vertex group weights. */
		if (subsurfedDVerts) {
			weight[0] = defvert_find_weight(subsurfedDVerts + mloop[0].v, cd_weight_index);
			weight[1] = defvert_find_weight(subsurfedDVerts + mloop[1].v, cd_weight_index);
			weight[2] = defvert_find_weight(subsurfedDVerts + mloop[2].v, cd_weight_index);
			weight[3] = defvert_find_weight(subsurfedDVerts + mloop[3].v, cd_weight_index);
		}
		else {
			weight[0] = 1.0f;
			weight[1] = 1.0f;
			weight[2] = 1.0f;
			weight[3] = 1.0f;
		}
		
		/* This is where all the magic is done. If the vertex exists in the, we pass the original uv pointer to the solver, thus
		 * flushing the solution to the edit mesh. */
		texface_from_original_index(origFace, origVertIndices[mloop[0].v], &uv[0], &pin[0], &select[0], scene, cd_loop_uv_offset);
		texface_from_original_index(origFace, origVertIndices[mloop[1].v], &uv[1], &pin[1], &select[1], scene, cd_loop_uv_offset);
		texface_from_original_index(origFace, origVertIndices[mloop[2].v], &uv[2], &pin[2], &select[2], scene, cd_loop_uv_offset);
		texface_from_original_index(origFace, origVertIndices[mloop[3].v], &uv[3], &pin[3], &select[3], scene, cd_loop_uv_offset);

		param_face_add(handle, key, 4, vkeys, co, uv, weight, pin, select, NULL);
	}

	/* these are calculated from original mesh too */
	for (edge = subsurfedEdges, i = 0; i < numOfEdges; i++, edge++) {
		if ((edgeMap[i] != NULL) && BM_elem_flag_test(edgeMap[i], BM_ELEM_SEAM)) {
			ParamKey vkeys[2];
			vkeys[0] = (ParamKey)edge->v1;
			vkeys[1] = (ParamKey)edge->v2;
			param_edge_set_seam(handle, vkeys);
		}
	}

	param_construct_end(handle, unwrap->fill_holes, 0);

	/* cleanup */
	MEM_freeN(faceMap);
	MEM_freeN(edgeMap);
	derivedMesh->release(derivedMesh);

	return handle;
}

/********************* Minimize Stretch operator **************** */

/* Get SLIM parameters from scene */
static SLIMMatrixTransfer *slim_matrix_transfer(const UnwrapProperties *unwrap)
{
	SLIMMatrixTransfer *mt = MEM_callocN(sizeof(SLIMMatrixTransfer), "Matrix Transfer to SLIM");

	mt->with_weighted_parameterization = strlen(unwrap->vertex_group) > 0;
	mt->weight_influence = unwrap->vertex_group_factor;
	mt->relative_scale = unwrap->relative_scale;
	mt->n_iterations =  unwrap->iterations;

	return mt;
}

/* Holds all necessary state for one session of interactive parametrisation. */
typedef struct MinStretch {
	ParamHandle *handle;
	Object *obedit;

	wmTimer *timer;
	float blend;
	bool fix_boundary;
} MinStretch;

/* Initializes SLIM and transfars data matrices */
static bool minimize_stretch_init(bContext *C, wmOperator *op)
{
	Scene *scene = CTX_data_scene(C);
	Object *obedit = CTX_data_edit_object(C);
	BMEditMesh *em = BKE_editmesh_from_object(obedit);
	bool implicit = true;

	if (!uvedit_have_selection(scene, em, implicit)) {
		return false;
	}

	UnwrapProperties unwrap = unwrap_properties_get(NULL, obedit);
	unwrap.fill_holes = true;

	ParamHandle *handle = construct_param_handle(scene, obedit, em->bm, implicit, true, &unwrap);

	MinStretch *ms = MEM_callocN(sizeof(MinStretch), "Data for minimizing stretch with SLIM");

	ms->handle = handle;
	ms->obedit = obedit;
	ms->fix_boundary = RNA_boolean_get(op->ptr, "fix_boundary");

	SLIMMatrixTransfer *mt = slim_matrix_transfer(&unwrap);
	mt->is_minimize_stretch = true;
	mt->skip_initialization = true;
	mt->fixed_boundary = ms->fix_boundary;

	param_slim_begin(ms->handle, mt);

	op->customdata = ms;
	return true;
}

/* After initialisation, these iterations are executed, until applied or canceled by the user. */
static void minimize_stretch_iteration(bContext *C, wmOperator *op, bool interactive)
{
	MinStretch *ms = op->customdata;

	param_slim_stretch_iteration(ms->handle, ms->blend);
	param_flush(ms->handle);

	DAG_id_tag_update(ms->obedit->data, 0);
	WM_event_add_notifier(C, NC_GEOM | ND_DATA, ms->obedit->data);
}

/* Exit interactive parametrisation. Clean up memory. */
static void minimize_stretch_exit(bContext *C, wmOperator *op, bool cancel)
{
	MinStretch *ms = op->customdata;

	param_slim_end(ms->handle);

	if (cancel) {
		param_flush_restore(ms->handle);
	}

	param_delete(ms->handle);

	DAG_id_tag_update(ms->obedit->data, 0);
	WM_event_add_notifier(C, NC_GEOM | ND_DATA, ms->obedit->data);

	MEM_freeN(ms);
	op->customdata = NULL;
}

/* Used Only to adjust parameters. */
static int minimize_stretch_exec(bContext *C, wmOperator *op)
{

	return OPERATOR_FINISHED;
}

/* Entry point to interactive parametrisation. Already executes one iteration, allowing faster feedback. */
static int minimize_stretch_invoke(bContext *C, wmOperator *op, const wmEvent *UNUSED(event))
{
	MinStretch *ms;

	if (!minimize_stretch_init(C, op))
		return OPERATOR_CANCELLED;

	minimize_stretch_iteration(C, op, true);

	ms = op->customdata;
	WM_event_add_modal_handler(C, op);
	ms->timer = WM_event_add_timer(CTX_wm_manager(C), CTX_wm_window(C), TIMER, 0.01f);

	return OPERATOR_RUNNING_MODAL;
}

/* The control structure of the modal operator. a next iteration is either started due to a timer or user input. */
static int minimize_stretch_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
	MinStretch *ms = op->customdata;

	switch (event->type) {
		case ESCKEY:
		case RIGHTMOUSE:
			minimize_stretch_exit(C, op, true);
			return OPERATOR_CANCELLED;
		case RETKEY:
		case PADENTER:
		case LEFTMOUSE:
			minimize_stretch_exit(C, op, false);
			return OPERATOR_FINISHED;
		case PADPLUSKEY:
		case WHEELUPMOUSE:
			if (event->val == KM_PRESS) {
				if (ms->blend < 1.0f) {
					ms->blend += MIN2(0.1f, 1 - (ms->blend));
					minimize_stretch_iteration(C, op, true);
				}
			}
			break;
		case PADMINUS:
		case WHEELDOWNMOUSE:
			if (event->val == KM_PRESS) {
				if (ms->blend > 0.0f) {
					ms->blend -= MIN2(0.1f, ms->blend);
					minimize_stretch_iteration(C, op, true);
				}
			}
			break;
		case TIMER:
			if (ms->timer == event->customdata) {
				double start = PIL_check_seconds_timer();

				do {
					minimize_stretch_iteration(C, op, true);
				} while (PIL_check_seconds_timer() - start < 0.01);
			}
			break;
	}

	/*if (ms->iterations && ms->i >= ms->iterations) {
		minimize_stretch_exit(C, op, false);
		return OPERATOR_FINISHED;
	 }*/

	return OPERATOR_RUNNING_MODAL;
}

/* Cancels the interactive parametrisation and discards the obtained map. */
static void minimize_stretch_cancel(bContext *C, wmOperator *op)
{
	minimize_stretch_exit(C, op, true);
}

/* Registration of the operator and integration into UI */
void UV_OT_minimize_stretch(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Minimize Stretch";
	ot->idname = "UV_OT_minimize_stretch";
	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_GRAB_CURSOR | OPTYPE_BLOCKING;
	ot->description = "Reduce UV stretching by applying the SLIM algorithm";

	/* api callbacks */
	ot->exec = minimize_stretch_exec;
	ot->invoke = minimize_stretch_invoke;
	ot->modal = minimize_stretch_modal;
	ot->cancel = minimize_stretch_cancel;
	ot->poll = ED_operator_uvedit;

	RNA_def_boolean(ot->srna, "fix_boundary", 1, "Fix Boundary", "Wether the vertices on the border may move or not.");
}

/* ******************** Pack Islands operator **************** */

void ED_uvedit_pack_islands(Scene *scene, Object *ob, BMesh *bm, bool selected, bool correct_aspect, bool do_rotate)
{
	UnwrapProperties unwrap = unwrap_properties_get(NULL, ob);
	unwrap.fill_holes = false;
	unwrap.correct_aspect = correct_aspect;

	ParamHandle *handle = construct_param_handle(scene, ob, bm, true, selected, &unwrap);
	param_pack(handle, scene->toolsettings->uvcalc_margin, do_rotate);
	param_flush(handle);
	param_delete(handle);
}

static int pack_islands_exec(bContext *C, wmOperator *op)
{
	Scene *scene = CTX_data_scene(C);
	Object *obedit = CTX_data_edit_object(C);
	BMEditMesh *em = BKE_editmesh_from_object(obedit);
	bool do_rotate = RNA_boolean_get(op->ptr, "rotate");

	if (!uvedit_have_selection(scene, em, true)) {
		return OPERATOR_CANCELLED;
	}

	if (RNA_struct_property_is_set(op->ptr, "margin"))
		scene->toolsettings->uvcalc_margin = RNA_float_get(op->ptr, "margin");
	else
		RNA_float_set(op->ptr, "margin", scene->toolsettings->uvcalc_margin);

	ED_uvedit_pack_islands(scene, obedit, em->bm, true, true, do_rotate);
	
	DAG_id_tag_update(obedit->data, 0);
	WM_event_add_notifier(C, NC_GEOM | ND_DATA, obedit->data);

	return OPERATOR_FINISHED;
}

void UV_OT_pack_islands(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Pack Islands";
	ot->idname = "UV_OT_pack_islands";
	ot->description = "Transform all islands so that they fill up the UV space as much as possible";

	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
	
	/* api callbacks */
	ot->exec = pack_islands_exec;
	ot->poll = ED_operator_uvedit;

	/* properties */
	RNA_def_boolean(ot->srna, "rotate", true, "Rotate", "Rotate islands for best fit");
	RNA_def_float_factor(ot->srna, "margin", 0.001f, 0.0f, 1.0f, "Margin", "Space between islands", 0.0f, 1.0f);
}

/* ******************** Average Islands Scale operator **************** */

static int average_islands_scale_exec(bContext *C, wmOperator *UNUSED(op))
{
	Scene *scene = CTX_data_scene(C);
	Object *obedit = CTX_data_edit_object(C);
	BMEditMesh *em = BKE_editmesh_from_object(obedit);
	bool implicit = true;

	if (!uvedit_have_selection(scene, em, implicit)) {
		return OPERATOR_CANCELLED;
	}

	UnwrapProperties unwrap = unwrap_properties_get(NULL, obedit);
	unwrap.fill_holes = false;
	unwrap.correct_aspect = true;

	ParamHandle *handle = construct_param_handle(scene, obedit, em->bm, implicit, 1, &unwrap);
	param_average(handle);
	param_flush(handle);
	param_delete(handle);
	
	DAG_id_tag_update(obedit->data, 0);
	WM_event_add_notifier(C, NC_GEOM | ND_DATA, obedit->data);

	return OPERATOR_FINISHED;
}

void UV_OT_average_islands_scale(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Average Islands Scale";
	ot->idname = "UV_OT_average_islands_scale";
	ot->description = "Average the size of separate UV islands, based on their area in 3D space";

	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
	
	/* api callbacks */
	ot->exec = average_islands_scale_exec;
	ot->poll = ED_operator_uvedit;
}

/**************** Live Unwrap *****************/

static ParamHandle *liveHandle = NULL;

void ED_uvedit_live_unwrap_begin(Scene *scene, Object *obedit)
{
	BMEditMesh *em = BKE_editmesh_from_object(obedit);

	if (!ED_uvedit_test(obedit)) {
		return;
	}

	UnwrapProperties unwrap = unwrap_properties_get(NULL, obedit);

	if (unwrap.use_subsurf)
		liveHandle = construct_param_handle_subsurfed(scene, obedit, em, false, &unwrap);
	else
		liveHandle = construct_param_handle(scene, obedit, em->bm, false, false, &unwrap);

	if (unwrap.use_slim) {
		SLIMMatrixTransfer *mt = slim_matrix_transfer(&unwrap);
		mt->skip_initialization = true;

		param_slim_begin(liveHandle, mt);
	}
	else {
		param_lscm_begin(liveHandle, PARAM_TRUE, unwrap.use_abf);
	}
}

void ED_uvedit_live_unwrap_re_solve(void)
{
	if (liveHandle) {
		if (param_is_slim(liveHandle)) {
			slim_reload_all_uvs(liveHandle);
			param_slim_solve_iteration(liveHandle);
		}
		else {
			param_lscm_solve(liveHandle);
		}

		param_flush(liveHandle);
	}
}
	
void ED_uvedit_live_unwrap_end(short cancel)
{
	if (liveHandle) {
		if (param_is_slim(liveHandle)) {
			param_slim_end(liveHandle);
		}
		else {
			param_lscm_end(liveHandle);
		}

		if (cancel)
			param_flush_restore(liveHandle);

		param_delete(liveHandle);
		liveHandle = NULL;
	}
}

void ED_uvedit_live_unwrap(Scene *scene, Object *obedit)
{
	BMEditMesh *em = BKE_editmesh_from_object(obedit);

	if (scene->toolsettings->edge_mode_live_unwrap &&
	    CustomData_has_layer(&em->bm->ldata, CD_MLOOPUV))
	{
		ED_uvedit_unwrap(scene, obedit, false, NULL); /* unwrap all not just sel */
	}
}

/*************** UV Map Common Transforms *****************/

#define VIEW_ON_EQUATOR 0
#define VIEW_ON_POLES   1
#define ALIGN_TO_OBJECT 2

#define POLAR_ZX    0
#define POLAR_ZY    1

static void uv_map_transform_center(Scene *scene, View3D *v3d, float *result, 
                                    Object *ob, BMEditMesh *em)
{
	const int around = (v3d) ? v3d->around : V3D_AROUND_CENTER_BOUNDS;

	/* only operates on the edit object - this is all that's needed now */

	switch (around) {
		case V3D_AROUND_CENTER_BOUNDS: /* bounding box center */
		{
			BMFace *efa;
			BMLoop *l;
			BMIter iter, liter;
			float min[3], max[3];

			INIT_MINMAX(min, max);
			
			BM_ITER_MESH (efa, &iter, em->bm, BM_FACES_OF_MESH) {
				if (BM_elem_flag_test(efa, BM_ELEM_SELECT)) {
					BM_ITER_ELEM (l, &liter, efa, BM_LOOPS_OF_FACE) {
						minmax_v3v3_v3(min, max, l->v->co);
					}
				}
			}
			mid_v3_v3v3(result, min, max);
			break;
		}
		case V3D_AROUND_CURSOR:  /* cursor center */
		{
			const float *curs = ED_view3d_cursor3d_get(scene, v3d);
			/* shift to objects world */
			sub_v3_v3v3(result, curs, ob->obmat[3]);
			break;
		}
		case V3D_AROUND_LOCAL_ORIGINS:  /* object center */
		case V3D_AROUND_CENTER_MEAN:    /* multiple objects centers, only one object here*/
		default:
			zero_v3(result);
			break;
	}
}

static void uv_map_rotation_matrix(float result[4][4], RegionView3D *rv3d, Object *ob,
                                   float upangledeg, float sideangledeg, float radius)
{
	float rotup[4][4], rotside[4][4], viewmatrix[4][4], rotobj[4][4];
	float sideangle = 0.0f, upangle = 0.0f;
	int k;

	/* get rotation of the current view matrix */
	if (rv3d)
		copy_m4_m4(viewmatrix, rv3d->viewmat);
	else
		unit_m4(viewmatrix);

	/* but shifting */
	for (k = 0; k < 4; k++)
		viewmatrix[3][k] = 0.0f;

	/* get rotation of the current object matrix */
	copy_m4_m4(rotobj, ob->obmat);

	/* but shifting */
	for (k = 0; k < 4; k++)
		rotobj[3][k] = 0.0f;

	zero_m4(rotup);
	zero_m4(rotside);

	/* compensate front/side.. against opengl x,y,z world definition */
	/* this is "kanonen gegen spatzen", a few plus minus 1 will do here */
	/* i wanted to keep the reason here, so we're rotating*/
	sideangle = (float)M_PI * (sideangledeg + 180.0f) / 180.0f;
	rotside[0][0] =  cosf(sideangle);
	rotside[0][1] = -sinf(sideangle);
	rotside[1][0] =  sinf(sideangle);
	rotside[1][1] =  cosf(sideangle);
	rotside[2][2] =  1.0f;

	upangle = (float)M_PI * upangledeg / 180.0f;
	rotup[1][1] =  cosf(upangle) / radius;
	rotup[1][2] = -sinf(upangle) / radius;
	rotup[2][1] =  sinf(upangle) / radius;
	rotup[2][2] =  cosf(upangle) / radius;
	rotup[0][0] =  1.0f / radius;

	/* calculate transforms*/
	mul_m4_series(result, rotup, rotside, viewmatrix, rotobj);
}

static void uv_map_transform(bContext *C, wmOperator *op, float center[3], float rotmat[4][4])
{
	/* context checks are messy here, making it work in both 3d view and uv editor */
	Scene *scene = CTX_data_scene(C);
	Object *obedit = CTX_data_edit_object(C);
	BMEditMesh *em = BKE_editmesh_from_object(obedit);
	View3D *v3d = CTX_wm_view3d(C);
	RegionView3D *rv3d = CTX_wm_region_view3d(C);
	/* common operator properties */
	int align = RNA_enum_get(op->ptr, "align");
	int direction = RNA_enum_get(op->ptr, "direction");
	float radius = RNA_struct_find_property(op->ptr, "radius") ? RNA_float_get(op->ptr, "radius") : 1.0f;
	float upangledeg, sideangledeg;

	uv_map_transform_center(scene, v3d, center, obedit, em);

	if (direction == VIEW_ON_EQUATOR) {
		upangledeg = 90.0f;
		sideangledeg = 0.0f;
	}
	else {
		upangledeg = 0.0f;
		if (align == POLAR_ZY) sideangledeg = 0.0f;
		else sideangledeg = 90.0f;
	}

	/* be compatible to the "old" sphere/cylinder mode */
	if (direction == ALIGN_TO_OBJECT)
		unit_m4(rotmat);
	else 
		uv_map_rotation_matrix(rotmat, rv3d, obedit, upangledeg, sideangledeg, radius);

}

static void uv_transform_properties(wmOperatorType *ot, int radius)
{
	static EnumPropertyItem direction_items[] = {
		{VIEW_ON_EQUATOR, "VIEW_ON_EQUATOR", 0, "View on Equator", "3D view is on the equator"},
		{VIEW_ON_POLES, "VIEW_ON_POLES", 0, "View on Poles", "3D view is on the poles"},
		{ALIGN_TO_OBJECT, "ALIGN_TO_OBJECT", 0, "Align to Object", "Align according to object transform"},
		{0, NULL, 0, NULL, NULL}
	};
	static EnumPropertyItem align_items[] = {
		{POLAR_ZX, "POLAR_ZX", 0, "Polar ZX", "Polar 0 is X"},
		{POLAR_ZY, "POLAR_ZY", 0, "Polar ZY", "Polar 0 is Y"},
		{0, NULL, 0, NULL, NULL}
	};

	RNA_def_enum(ot->srna, "direction", direction_items, VIEW_ON_EQUATOR, "Direction",
	             "Direction of the sphere or cylinder");
	RNA_def_enum(ot->srna, "align", align_items, VIEW_ON_EQUATOR, "Align",
	             "How to determine rotation around the pole");
	if (radius)
		RNA_def_float(ot->srna, "radius", 1.0f, 0.0f, FLT_MAX, "Radius",
		              "Radius of the sphere or cylinder", 0.0001f, 100.0f);
}

static void correct_uv_aspect(Scene *scene, Object *ob, BMEditMesh *em)
{
	BMLoop *l;
	BMIter iter, liter;
	MLoopUV *luv;
	BMFace *efa;
	float scale, aspx, aspy;
	
	const int cd_loop_uv_offset = CustomData_get_offset(&em->bm->ldata, CD_MLOOPUV);

	ED_uvedit_get_aspect(scene, ob, em->bm, &aspx, &aspy);
	
	if (aspx == aspy)
		return;
		
	if (aspx > aspy) {
		scale = aspy / aspx;

		BM_ITER_MESH (efa, &iter, em->bm, BM_FACES_OF_MESH) {
			if (!BM_elem_flag_test(efa, BM_ELEM_SELECT))
				continue;
			
			BM_ITER_ELEM (l, &liter, efa, BM_LOOPS_OF_FACE) {
				luv = BM_ELEM_CD_GET_VOID_P(l, cd_loop_uv_offset);
				luv->uv[0] = ((luv->uv[0] - 0.5f) * scale) + 0.5f;
			}
		}
	}
	else {
		scale = aspx / aspy;

		BM_ITER_MESH (efa, &iter, em->bm, BM_FACES_OF_MESH) {
			if (!BM_elem_flag_test(efa, BM_ELEM_SELECT))
				continue;
			
			BM_ITER_ELEM (l, &liter, efa, BM_LOOPS_OF_FACE) {
				luv = BM_ELEM_CD_GET_VOID_P(l, cd_loop_uv_offset);
				luv->uv[1] = ((luv->uv[1] - 0.5f) * scale) + 0.5f;
			}
		}
	}
}

/******************** Map Clip & Correct ******************/

static void uv_map_clip_correct_properties(wmOperatorType *ot)
{
	RNA_def_boolean(ot->srna, "correct_aspect", 1, "Correct Aspect",
	                "Map UVs taking image aspect ratio into account");
	RNA_def_boolean(ot->srna, "clip_to_bounds", 0, "Clip to Bounds",
	                "Clip UV coordinates to bounds after unwrapping");
	RNA_def_boolean(ot->srna, "scale_to_bounds", 0, "Scale to Bounds",
	                "Scale UV coordinates to bounds after unwrapping");
}

static void uv_map_clip_correct(Scene *scene, Object *ob, BMEditMesh *em, wmOperator *op)
{
	BMFace *efa;
	BMLoop *l;
	BMIter iter, liter;
	MLoopUV *luv;
	float dx, dy, min[2], max[2];
	const bool correct_aspect = RNA_boolean_get(op->ptr, "correct_aspect");
	const bool clip_to_bounds = RNA_boolean_get(op->ptr, "clip_to_bounds");
	const bool scale_to_bounds = RNA_boolean_get(op->ptr, "scale_to_bounds");

	const int cd_loop_uv_offset = CustomData_get_offset(&em->bm->ldata, CD_MLOOPUV);

	/* correct for image aspect ratio */
	if (correct_aspect)
		correct_uv_aspect(scene, ob, em);

	if (scale_to_bounds) {
		INIT_MINMAX2(min, max);

		BM_ITER_MESH (efa, &iter, em->bm, BM_FACES_OF_MESH) {
			if (!BM_elem_flag_test(efa, BM_ELEM_SELECT))
				continue;

			BM_ITER_ELEM (l, &liter, efa, BM_LOOPS_OF_FACE) {
				luv = BM_ELEM_CD_GET_VOID_P(l, cd_loop_uv_offset);
				minmax_v2v2_v2(min, max, luv->uv);
			}
		}

		/* rescale UV to be in 1/1 */
		dx = (max[0] - min[0]);
		dy = (max[1] - min[1]);

		if (dx > 0.0f)
			dx = 1.0f / dx;
		if (dy > 0.0f)
			dy = 1.0f / dy;

		BM_ITER_MESH (efa, &iter, em->bm, BM_FACES_OF_MESH) {
			if (!BM_elem_flag_test(efa, BM_ELEM_SELECT))
				continue;

			BM_ITER_ELEM (l, &liter, efa, BM_LOOPS_OF_FACE) {
				luv = BM_ELEM_CD_GET_VOID_P(l, cd_loop_uv_offset);

				luv->uv[0] = (luv->uv[0] - min[0]) * dx;
				luv->uv[1] = (luv->uv[1] - min[1]) * dy;
			}
		}
	}
	else if (clip_to_bounds) {
		/* clipping and wrapping */
		BM_ITER_MESH (efa, &iter, em->bm, BM_FACES_OF_MESH) {
			if (!BM_elem_flag_test(efa, BM_ELEM_SELECT))
				continue;

			BM_ITER_ELEM (l, &liter, efa, BM_LOOPS_OF_FACE) {
				luv = BM_ELEM_CD_GET_VOID_P(l, cd_loop_uv_offset);
				CLAMP(luv->uv[0], 0.0f, 1.0f);
				CLAMP(luv->uv[1], 0.0f, 1.0f);
			}
		}
	}
}

/* ******************** Unwrap operator **************** */

/* assumes UV Map is checked, doesn't run update funcs */
void ED_uvedit_unwrap(Scene *scene, Object *obedit, const short sel, wmOperator *op)
{
	BMEditMesh *em = BKE_editmesh_from_object(obedit);
	UnwrapProperties unwrap = unwrap_properties_get(op, obedit);
	ParamHandle *handle;

	if (unwrap.use_subsurf)
		handle = construct_param_handle_subsurfed(scene, obedit, em, sel, &unwrap);
	else
		handle = construct_param_handle(scene, obedit, em->bm, false, sel, &unwrap);

	if (unwrap.use_slim) {
		SLIMMatrixTransfer *mt = slim_matrix_transfer(&unwrap);
		mt->reflection_mode = unwrap.reflection_mode;
		mt->transform_islands = true;

		param_slim_solve(handle, mt);
	}
	else {
		param_lscm_begin(handle, PARAM_FALSE, unwrap.use_abf);
		param_lscm_solve(handle);
		param_lscm_end(handle);
	}

	param_average(handle);
	param_pack(handle, scene->toolsettings->uvcalc_margin, false);

	param_flush(handle);
	param_delete(handle);
}

static int unwrap_exec(bContext *C, wmOperator *op)
{
	Scene *scene = CTX_data_scene(C);
	Object *obedit = CTX_data_edit_object(C);
	BMEditMesh *em = BKE_editmesh_from_object(obedit);

	float obsize[3];
	bool implicit = false;

	if (!uvedit_have_selection(scene, em, implicit)) {
		return OPERATOR_CANCELLED;
	}
	
	/* add uvs if they don't exist yet */
	if (!ED_uvedit_ensure_uvs(C, scene, obedit)) {
		return OPERATOR_CANCELLED;
	}

	mat4_to_size(obsize, obedit->obmat);
	if (!(fabsf(obsize[0] - obsize[1]) < 1e-4f && fabsf(obsize[1] - obsize[2]) < 1e-4f))
		BKE_report(op->reports, RPT_INFO,
		           "Object has non-uniform scale, unwrap will operate on a non-scaled version of the mesh");
	else if (is_negative_m4(obedit->obmat))
		BKE_report(op->reports, RPT_INFO,
		           "Object has negative scale, unwrap will operate on a non-flipped version of the mesh");

	/* remember packing marging */
	if (RNA_struct_property_is_set(op->ptr, "margin"))
		scene->toolsettings->uvcalc_margin = RNA_float_get(op->ptr, "margin");
	else
		RNA_float_set(op->ptr, "margin", scene->toolsettings->uvcalc_margin);

	/* execute unwrap */
	ED_uvedit_unwrap(scene, obedit, true, op);

	DAG_id_tag_update(obedit->data, 0);
	WM_event_add_notifier(C, NC_GEOM | ND_DATA, obedit->data);

	return OPERATOR_FINISHED;
}

static bool unwrap_draw_check_prop_slim(PointerRNA *UNUSED(ptr), PropertyRNA *prop)
{
	const char *prop_id = RNA_property_identifier(prop);

	return !(STREQ(prop_id, "fill_holes"));
}

static bool unwrap_draw_check_prop_abf(PointerRNA *UNUSED(ptr), PropertyRNA *prop)
{
	const char *prop_id = RNA_property_identifier(prop);

	return !(STREQ(prop_id, "reflection_mode") ||
			 STREQ(prop_id, "iterations") ||
			 STREQ(prop_id, "relative_scale") ||
			 STREQ(prop_id, "vertex_group") ||
			 STREQ(prop_id, "vertex_group_factor")
			 );
}

static void unwrap_draw(bContext *UNUSED(C), wmOperator *op)
{
	uiLayout *layout = op->layout;
	PointerRNA ptr;

	/* main draw call */
	RNA_pointer_create(NULL, op->type->srna, op->properties, &ptr);

	if (RNA_enum_get(op->ptr, "method") == 2) {
		uiDefAutoButsRNA(layout, &ptr, unwrap_draw_check_prop_slim, '\0');
	}
	else {
		uiDefAutoButsRNA(layout, &ptr, unwrap_draw_check_prop_abf, '\0');
	}
}

void UV_OT_unwrap(wmOperatorType *ot)
{
	static EnumPropertyItem method_items[] = {
		{0, "ANGLE_BASED", 0, "Angle Based", ""},
		{1, "CONFORMAL", 0, "Conformal", ""},
		{2, "SLIM", 0, "SLIM", "" },
		{0, NULL, 0, NULL, NULL}
	};

	static EnumPropertyItem reflection_items[] = {
		{0, "ALLOW", 0, "Allow Flips", ""},
		{1, "DISALLOW", 0, "Don't Allow Flips", ""},
		{0, NULL, 0, NULL, NULL}
	};

	/* identifiers */
	ot->name = "Unwrap";
	ot->description = "Unwrap the mesh of the object being edited";
	ot->idname = "UV_OT_unwrap";
	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
	
	/* api callbacks */
	ot->exec = unwrap_exec;
	ot->poll = ED_operator_uvmap;

	/* only draw relevant ui elements*/
	ot->ui = unwrap_draw;


	/* properties */
	RNA_def_enum(ot->srna, "method", method_items, 2, "Method",
	             "Unwrapping method (Angle Based usually gives better results than Conformal, while being somewhat slower)");

	RNA_def_boolean(ot->srna, "correct_aspect", 1, "Correct Aspect",
	                "Map UVs taking image aspect ratio into account");
	RNA_def_boolean(ot->srna, "use_subsurf_data", 0, "Use Subsurf Modifier",
	                "Map UVs taking vertex position after Subdivision Surface modifier has been applied");
	RNA_def_float_factor(ot->srna, "margin", 0.001f, 0.0f, 1.0f, "Margin", "Space between islands", 0.0f, 1.0f);

	/* ABF / LSCM only */
	RNA_def_boolean(ot->srna, "fill_holes", 1, "Fill Holes",
					"Virtual fill holes in mesh before unwrapping, to better avoid overlaps and preserve symmetry");

	/* SLIM only */
	RNA_def_enum(ot->srna, "reflection_mode", reflection_items, 0, "Reflection Mode",
				 "Allowing reflections means that depending on the position of pins, the map may be flipped. Lower distortion");
	RNA_def_int(ot->srna, "iterations", 10, 0, 10000, "Iterations",
				"Number of Iterations if the SLIM algorithm is used", 1, 30);
	RNA_def_float(ot->srna, "relative_scale", 1.0, 0.001, 1000.0, "Relative Scale",
				  "Relative Scale of UV Map with respect to pins", 0.1, 10.0);
	RNA_def_string(ot->srna, "vertex_group", NULL, MAX_ID_NAME, "Vertex Group", "Vertex group name for modulating the deform");
	RNA_def_float(ot->srna, "vertex_group_factor", 1.0, -10000.0, 10000.0, "Factor",
				"How much influence the weightmap has for weighted parameterization, 0 being no influence", -10.0, 10.0);
}

/**************** Project From View operator **************/
static int uv_from_view_exec(bContext *C, wmOperator *op);

static int uv_from_view_invoke(bContext *C, wmOperator *op, const wmEvent *UNUSED(event))
{
	View3D *v3d = CTX_wm_view3d(C);
	RegionView3D *rv3d = CTX_wm_region_view3d(C);
	Camera *camera = ED_view3d_camera_data_get(v3d, rv3d);
	PropertyRNA *prop;

	prop = RNA_struct_find_property(op->ptr, "camera_bounds");
	if (!RNA_property_is_set(op->ptr, prop)) RNA_property_boolean_set(op->ptr, prop, (camera != NULL));
	prop = RNA_struct_find_property(op->ptr, "correct_aspect");
	if (!RNA_property_is_set(op->ptr, prop)) RNA_property_boolean_set(op->ptr, prop, (camera == NULL));

	return uv_from_view_exec(C, op);
}

static int uv_from_view_exec(bContext *C, wmOperator *op)
{
	Scene *scene = CTX_data_scene(C);
	Object *obedit = CTX_data_edit_object(C);
	BMEditMesh *em = BKE_editmesh_from_object(obedit);
	ARegion *ar = CTX_wm_region(C);
	View3D *v3d = CTX_wm_view3d(C);
	RegionView3D *rv3d = CTX_wm_region_view3d(C);
	Camera *camera = ED_view3d_camera_data_get(v3d, rv3d);
	BMFace *efa;
	BMLoop *l;
	BMIter iter, liter;
	MLoopUV *luv;
	float rotmat[4][4];

	int cd_loop_uv_offset;

	/* add uvs if they don't exist yet */
	if (!ED_uvedit_ensure_uvs(C, scene, obedit)) {
		return OPERATOR_CANCELLED;
	}

	cd_loop_uv_offset = CustomData_get_offset(&em->bm->ldata, CD_MLOOPUV);

	if (RNA_boolean_get(op->ptr, "orthographic")) {
		uv_map_rotation_matrix(rotmat, rv3d, obedit, 90.0f, 0.0f, 1.0f);
		
		BM_ITER_MESH (efa, &iter, em->bm, BM_FACES_OF_MESH) {
			if (!BM_elem_flag_test(efa, BM_ELEM_SELECT))
				continue;

			BM_ITER_ELEM (l, &liter, efa, BM_LOOPS_OF_FACE) {
				luv = BM_ELEM_CD_GET_VOID_P(l, cd_loop_uv_offset);
				BLI_uvproject_from_view_ortho(luv->uv, l->v->co, rotmat);
			}
		}
	}
	else if (camera) {
		const bool camera_bounds = RNA_boolean_get(op->ptr, "camera_bounds");
		struct ProjCameraInfo *uci = BLI_uvproject_camera_info(v3d->camera, obedit->obmat,
		                                                       camera_bounds ? (scene->r.xsch * scene->r.xasp) : 1.0f,
		                                                       camera_bounds ? (scene->r.ysch * scene->r.yasp) : 1.0f);

		if (uci) {
			BM_ITER_MESH (efa, &iter, em->bm, BM_FACES_OF_MESH) {
				if (!BM_elem_flag_test(efa, BM_ELEM_SELECT))
					continue;

				BM_ITER_ELEM (l, &liter, efa, BM_LOOPS_OF_FACE) {
					luv = BM_ELEM_CD_GET_VOID_P(l, cd_loop_uv_offset);
					BLI_uvproject_from_camera(luv->uv, l->v->co, uci);
				}
			}
			
			MEM_freeN(uci);
		}
	}
	else {
		copy_m4_m4(rotmat, obedit->obmat);

		BM_ITER_MESH (efa, &iter, em->bm, BM_FACES_OF_MESH) {
			if (!BM_elem_flag_test(efa, BM_ELEM_SELECT))
				continue;

			BM_ITER_ELEM (l, &liter, efa, BM_LOOPS_OF_FACE) {
				luv = BM_ELEM_CD_GET_VOID_P(l, cd_loop_uv_offset);
				BLI_uvproject_from_view(luv->uv, l->v->co, rv3d->persmat, rotmat, ar->winx, ar->winy);
			}
		}
	}

	uv_map_clip_correct(scene, obedit, em, op);

	DAG_id_tag_update(obedit->data, 0);
	WM_event_add_notifier(C, NC_GEOM | ND_DATA, obedit->data);

	return OPERATOR_FINISHED;
}

static int uv_from_view_poll(bContext *C)
{
	RegionView3D *rv3d = CTX_wm_region_view3d(C);

	if (!ED_operator_uvmap(C))
		return 0;

	return (rv3d != NULL);
}

void UV_OT_project_from_view(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Project From View";
	ot->idname = "UV_OT_project_from_view";
	ot->description = "Project the UV vertices of the mesh as seen in current 3D view";

	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
	
	/* api callbacks */
	ot->invoke = uv_from_view_invoke;
	ot->exec = uv_from_view_exec;
	ot->poll = uv_from_view_poll;

	/* properties */
	RNA_def_boolean(ot->srna, "orthographic", 0, "Orthographic",
	                "Use orthographic projection");
	RNA_def_boolean(ot->srna, "camera_bounds", 1, "Camera Bounds",
	                "Map UVs to the camera region taking resolution and aspect into account");
	uv_map_clip_correct_properties(ot);
}

/********************** Reset operator ********************/

static int reset_exec(bContext *C, wmOperator *UNUSED(op))
{
	Scene *scene = CTX_data_scene(C);
	Object *obedit = CTX_data_edit_object(C);
	Mesh *me = (Mesh *)obedit->data;

	/* add uvs if they don't exist yet */
	if (!ED_uvedit_ensure_uvs(C, scene, obedit)) {
		return OPERATOR_CANCELLED;
	}

	ED_mesh_uv_loop_reset(C, me);

	DAG_id_tag_update(obedit->data, 0);
	WM_event_add_notifier(C, NC_GEOM | ND_DATA, obedit->data);
	
	return OPERATOR_FINISHED;
}

void UV_OT_reset(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Reset";
	ot->idname = "UV_OT_reset";
	ot->description = "Reset UV projection";

	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
	
	/* api callbacks */
	ot->exec = reset_exec;
	ot->poll = ED_operator_uvmap;
}

/****************** Sphere Project operator ***************/

static void uv_sphere_project(float target[2], float source[3], float center[3], float rotmat[4][4])
{
	float pv[3];

	sub_v3_v3v3(pv, source, center);
	mul_m4_v3(rotmat, pv);

	map_to_sphere(&target[0], &target[1], pv[0], pv[1], pv[2]);

	/* split line is always zero */
	if (target[0] >= 1.0f)
		target[0] -= 1.0f;  
}

static void uv_map_mirror(BMEditMesh *em, BMFace *efa, MTexPoly *UNUSED(tf))
{
	BMLoop *l;
	BMIter liter;
	MLoopUV *luv;
	float **uvs = BLI_array_alloca(uvs, efa->len);
	float dx;
	int i, mi;

	const int cd_loop_uv_offset = CustomData_get_offset(&em->bm->ldata, CD_MLOOPUV);

	BM_ITER_ELEM_INDEX (l, &liter, efa, BM_LOOPS_OF_FACE, i) {
		luv = BM_ELEM_CD_GET_VOID_P(l, cd_loop_uv_offset);
		uvs[i] = luv->uv;
	}

	mi = 0;
	for (i = 1; i < efa->len; i++)
		if (uvs[i][0] > uvs[mi][0])
			mi = i;

	for (i = 0; i < efa->len; i++) {
		if (i != mi) {
			dx = uvs[mi][0] - uvs[i][0];
			if (dx > 0.5f) uvs[i][0] += 1.0f;
		}
	}
}

static int sphere_project_exec(bContext *C, wmOperator *op)
{
	Scene *scene = CTX_data_scene(C);
	Object *obedit = CTX_data_edit_object(C);
	BMEditMesh *em = BKE_editmesh_from_object(obedit);
	BMFace *efa;
	BMLoop *l;
	BMIter iter, liter;
	MTexPoly *tf;
	MLoopUV *luv;
	float center[3], rotmat[4][4];

	int cd_loop_uv_offset;

	/* add uvs if they don't exist yet */
	if (!ED_uvedit_ensure_uvs(C, scene, obedit)) {
		return OPERATOR_CANCELLED;
	}

	cd_loop_uv_offset = CustomData_get_offset(&em->bm->ldata, CD_MLOOPUV);

	uv_map_transform(C, op, center, rotmat);

	BM_ITER_MESH (efa, &iter, em->bm, BM_FACES_OF_MESH) {
		if (!BM_elem_flag_test(efa, BM_ELEM_SELECT))
			continue;

		BM_ITER_ELEM (l, &liter, efa, BM_LOOPS_OF_FACE) {
			luv = BM_ELEM_CD_GET_VOID_P(l, cd_loop_uv_offset);

			uv_sphere_project(luv->uv, l->v->co, center, rotmat);
		}

		tf = CustomData_bmesh_get(&em->bm->pdata, efa->head.data, CD_MTEXPOLY);
		uv_map_mirror(em, efa, tf);
	}

	uv_map_clip_correct(scene, obedit, em, op);

	DAG_id_tag_update(obedit->data, 0);
	WM_event_add_notifier(C, NC_GEOM | ND_DATA, obedit->data);

	return OPERATOR_FINISHED;
}

void UV_OT_sphere_project(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Sphere Projection";
	ot->idname = "UV_OT_sphere_project";
	ot->description = "Project the UV vertices of the mesh over the curved surface of a sphere";

	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
	
	/* api callbacks */
	ot->exec = sphere_project_exec;
	ot->poll = ED_operator_uvmap;

	/* properties */
	uv_transform_properties(ot, 0);
	uv_map_clip_correct_properties(ot);
}

/***************** Cylinder Project operator **************/

static void uv_cylinder_project(float target[2], float source[3], float center[3], float rotmat[4][4])
{
	float pv[3];

	sub_v3_v3v3(pv, source, center);
	mul_m4_v3(rotmat, pv);

	map_to_tube(&target[0], &target[1], pv[0], pv[1], pv[2]);

	/* split line is always zero */
	if (target[0] >= 1.0f)
		target[0] -= 1.0f;  
}

static int cylinder_project_exec(bContext *C, wmOperator *op)
{
	Scene *scene = CTX_data_scene(C);
	Object *obedit = CTX_data_edit_object(C);
	BMEditMesh *em = BKE_editmesh_from_object(obedit);
	BMFace *efa;
	BMLoop *l;
	BMIter iter, liter;
	MTexPoly *tf;
	MLoopUV *luv;
	float center[3], rotmat[4][4];

	int cd_loop_uv_offset;

	/* add uvs if they don't exist yet */
	if (!ED_uvedit_ensure_uvs(C, scene, obedit)) {
		return OPERATOR_CANCELLED;
	}

	cd_loop_uv_offset = CustomData_get_offset(&em->bm->ldata, CD_MLOOPUV);

	uv_map_transform(C, op, center, rotmat);

	BM_ITER_MESH (efa, &iter, em->bm, BM_FACES_OF_MESH) {
		if (!BM_elem_flag_test(efa, BM_ELEM_SELECT))
			continue;
		
		BM_ITER_ELEM (l, &liter, efa, BM_LOOPS_OF_FACE) {
			luv = BM_ELEM_CD_GET_VOID_P(l, cd_loop_uv_offset);

			uv_cylinder_project(luv->uv, l->v->co, center, rotmat);
		}

		tf = CustomData_bmesh_get(&em->bm->pdata, efa->head.data, CD_MTEXPOLY);
		uv_map_mirror(em, efa, tf);
	}

	uv_map_clip_correct(scene, obedit, em, op);

	DAG_id_tag_update(obedit->data, 0);
	WM_event_add_notifier(C, NC_GEOM | ND_DATA, obedit->data);

	return OPERATOR_FINISHED;
}

void UV_OT_cylinder_project(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Cylinder Projection";
	ot->idname = "UV_OT_cylinder_project";
	ot->description = "Project the UV vertices of the mesh over the curved wall of a cylinder";

	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
	
	/* api callbacks */
	ot->exec = cylinder_project_exec;
	ot->poll = ED_operator_uvmap;

	/* properties */
	uv_transform_properties(ot, 1);
	uv_map_clip_correct_properties(ot);
}

/******************* Cube Project operator ****************/

void ED_uvedit_unwrap_cube_project(Object *ob, BMesh *bm, float cube_size, bool use_select)
{
	BMFace *efa;
	BMLoop *l;
	BMIter iter, liter;
	/* MTexPoly *tf; */ /* UNUSED */
	MLoopUV *luv;
	float *loc, dx, dy;
	int cox, coy;

	int cd_loop_uv_offset;

	cd_loop_uv_offset = CustomData_get_offset(&bm->ldata, CD_MLOOPUV);

	loc = ob->obmat[3];

	/* choose x,y,z axis for projection depending on the largest normal
	 * component, but clusters all together around the center of map. */

	BM_ITER_MESH (efa, &iter, bm, BM_FACES_OF_MESH) {
		int first = 1;

		/* tf = CustomData_bmesh_get(&em->bm->pdata, efa->head.data, CD_MTEXPOLY); */ /* UNUSED */
		if (use_select && !BM_elem_flag_test(efa, BM_ELEM_SELECT))
			continue;

		axis_dominant_v3(&cox, &coy, efa->no);

		dx = dy = 0;
		BM_ITER_ELEM (l, &liter, efa, BM_LOOPS_OF_FACE) {
			luv = BM_ELEM_CD_GET_VOID_P(l, cd_loop_uv_offset);

			luv->uv[0] = 0.5f + 0.5f * cube_size * (loc[cox] + l->v->co[cox]);
			luv->uv[1] = 0.5f + 0.5f * cube_size * (loc[coy] + l->v->co[coy]);

			if (first) {
				dx = floor(luv->uv[0]);
				dy = floor(luv->uv[1]);
				first = 0;
			}


			luv->uv[0] -= dx;
			luv->uv[1] -= dy;
		}
	}

}

static int cube_project_exec(bContext *C, wmOperator *op)
{
	Scene *scene = CTX_data_scene(C);
	Object *obedit = CTX_data_edit_object(C);
	BMEditMesh *em = BKE_editmesh_from_object(obedit);
	float cube_size = RNA_float_get(op->ptr, "cube_size");

	/* add uvs if they don't exist yet */
	if (!ED_uvedit_ensure_uvs(C, scene, obedit)) {
		return OPERATOR_CANCELLED;
	}

	ED_uvedit_unwrap_cube_project(obedit, em->bm, cube_size, true);
	uv_map_clip_correct(scene, obedit, em, op);

	DAG_id_tag_update(obedit->data, 0);
	WM_event_add_notifier(C, NC_GEOM | ND_DATA, obedit->data);

	return OPERATOR_FINISHED;
}

void UV_OT_cube_project(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Cube Projection";
	ot->idname = "UV_OT_cube_project";
	ot->description = "Project the UV vertices of the mesh over the six faces of a cube";

	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
	
	/* api callbacks */
	ot->exec = cube_project_exec;
	ot->poll = ED_operator_uvmap;

	/* properties */
	RNA_def_float(ot->srna, "cube_size", 1.0f, 0.0f, FLT_MAX, "Cube Size", "Size of the cube to project on", 0.001f, 100.0f);
	uv_map_clip_correct_properties(ot);
}
