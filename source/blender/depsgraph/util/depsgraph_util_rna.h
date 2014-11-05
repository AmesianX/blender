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
 * The Original Code is Copyright (C) 2014 Blender Foundation.
 * All rights reserved.
 *
 * Original Author: Lukas Toenne
 * Contributor(s): None yet
 */

#ifndef __DEPSGRAPH_UTIL_RNA_H__
#define __DEPSGRAPH_UTIL_RNA_H__

extern "C" {
#include "BLI_utildefines.h"

#include "RNA_access.h"
}

//struct IDPtr;

/* Utility functions for creating PointerRNA inline */

BLI_INLINE PointerRNA make_rna_pointer(ID *id, StructRNA *type, void *data)
{
	PointerRNA ptr;
	RNA_pointer_create(id, type, data, &ptr);
	return ptr;
}

BLI_INLINE PointerRNA make_rna_id_pointer(ID *id)
{
	PointerRNA ptr;
	RNA_id_pointer_create(id, &ptr);
	return ptr;
}

#endif /* __DEPSGRAPH_UTIL_RNA_H__ */
