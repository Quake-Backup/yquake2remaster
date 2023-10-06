/*
 * Copyright (C) 1997-2001 Id Software, Inc.
 * Copyright (C) 2018-2019 Krzysztof Kondrak
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * =======================================================================
 *
 * Lightmaps and dynamic lighting
 *
 * =======================================================================
 */

#include "header/local.h"

int r_dlightframecount;
vec3_t lightspot;

static void
R_RenderDlight(dlight_t *light)
{
	VkDescriptorSet	uboDescriptorSet;
	uint8_t	*vertData, *uboData;
	VkDeviceSize	vboOffset;
	uint32_t	uboOffset;
	VkBuffer	vbo;
	int		i, j;
	float	rad;

	rad = light->intensity * 0.35;

	struct {
		vec3_t verts;
		float color[3];
	} lightVerts[18];

	for (i = 0; i < 3; i++)
	{
		lightVerts[0].verts[i] = light->origin[i] - vpn[i] * rad;
	}

	lightVerts[0].color[0] = light->color[0] * 0.2;
	lightVerts[0].color[1] = light->color[1] * 0.2;
	lightVerts[0].color[2] = light->color[2] * 0.2;

	for (i = 16; i >= 0; i--)
	{
		float	a;

		a = i / 16.0 * M_PI * 2;
		for (j = 0; j < 3; j++)
		{
			lightVerts[i+1].verts[j] = light->origin[j] + vright[j] * cos(a)*rad
				+ vup[j] * sin(a)*rad;
			lightVerts[i+1].color[j] = 0.f;
		}
	}

	QVk_BindPipeline(&vk_drawDLightPipeline);

	vertData = QVk_GetVertexBuffer(sizeof(lightVerts), &vbo, &vboOffset);
	uboData = QVk_GetUniformBuffer(sizeof(r_viewproj_matrix), &uboOffset, &uboDescriptorSet);
	memcpy(vertData, lightVerts, sizeof(lightVerts));
	memcpy(uboData,  r_viewproj_matrix, sizeof(r_viewproj_matrix));

	vkCmdBindVertexBuffers(vk_activeCmdbuffer, 0, 1, &vbo, &vboOffset);
	vkCmdBindDescriptorSets(vk_activeCmdbuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_drawDLightPipeline.layout, 0, 1, &uboDescriptorSet, 1, &uboOffset);
	vkCmdBindIndexBuffer(vk_activeCmdbuffer, QVk_GetTriangleFanIbo(48), 0, VK_INDEX_TYPE_UINT16);
	vkCmdDrawIndexed(vk_activeCmdbuffer, 48, 1, 0, 0, 0);
}

void
R_RenderDlights(void)
{
	int i;
	dlight_t *l;

	if (!vk_flashblend->value)
	{
		return;
	}

	/* because the count hasn't advanced yet for this frame */
	r_dlightframecount = r_framecount + 1;

	l = r_newrefdef.dlights;

	for (i = 0; i < r_newrefdef.num_dlights; i++, l++)
	{
		R_RenderDlight(l);
	}
}

void
R_MarkSurfaceLights(dlight_t *light, int bit, mnode_t *node, int r_dlightframecount)
{
	msurface_t	*surf;
	int			i;

	/* mark the polygons */
	surf = r_worldmodel->surfaces + node->firstsurface;

	for (i = 0; i < node->numsurfaces; i++, surf++)
	{
		int sidebit;
		float dist;

		if (surf->dlightframe != r_dlightframecount)
		{
			surf->dlightbits = 0;
			surf->dlightframe = r_dlightframecount;
		}

		dist = DotProduct(light->origin, surf->plane->normal) - surf->plane->dist;

		if (dist >= 0)
		{
			sidebit = 0;
		}
		else
		{
			sidebit = SURF_PLANEBACK;
		}

		if ((surf->flags & SURF_PLANEBACK) != sidebit)
		{
			continue;
		}

		surf->dlightbits |= bit;
	}
}

void
R_PushDlights(void)
{
	dlight_t *l;
	int i;

	if (vk_flashblend->value)
	{
		return;
	}

	/* because the count hasn't advanced yet for this frame */
	r_dlightframecount = r_framecount + 1;

	l = r_newrefdef.dlights;

	for (i = 0; i < r_newrefdef.num_dlights; i++, l++)
	{
		R_MarkLights(l, 1 << i, r_worldmodel->nodes, r_dlightframecount,
			R_MarkSurfaceLights);
	}
}

void
R_LightPoint(const entity_t *currententity, refdef_t *refdef, const msurface_t *surfaces,
	const mnode_t *nodes, vec3_t p, vec3_t color, float modulate, vec3_t lightspot)
{
	vec3_t end, dist, pointcolor = {0, 0, 0};
	float r;
	int lnum;
	dlight_t *dl;

	if (!currententity)
	{
		color[0] = color[1] = color[2] = 1.0;
		return;
	}

	end[0] = p[0];
	end[1] = p[1];
	end[2] = p[2] - 2048;

	r = R_RecursiveLightPoint(surfaces, nodes, refdef->lightstyles,
		p, end, pointcolor, lightspot, modulate);

	if (r == -1)
	{
		VectorCopy(vec3_origin, color);
	}
	else
	{
		VectorCopy(pointcolor, color);
	}

	/* add dynamic lights */
	dl = refdef->dlights;

	for (lnum = 0; lnum < refdef->num_dlights; lnum++, dl++)
	{
		float	add;

		VectorSubtract(currententity->origin,
				dl->origin, dist);
		add = dl->intensity - VectorLength(dist);
		add *= (1.0f / 256.0f);

		if (add > 0)
		{
			VectorMA(color, add, dl->color, color);
		}
	}

	VectorScale(color, modulate, color);
}

static void
R_AddDynamicLights(msurface_t *surf)
{
	int lnum;
	int sd, td;
	float fdist, frad, fminlight;
	vec3_t impact, local;
	int s, t;
	int i;
	int smax, tmax;
	dlight_t *dl;
	float *plightdest;
	float fsacc, ftacc;

	smax = (surf->extents[0] >> surf->lmshift) + 1;
	tmax = (surf->extents[1] >> surf->lmshift) + 1;

	for (lnum = 0; lnum < r_newrefdef.num_dlights; lnum++)
	{
		if (!(surf->dlightbits & (1 << lnum)))
		{
			continue; /* not lit by this light */
		}

		dl = &r_newrefdef.dlights[lnum];
		frad = dl->intensity;
		fdist = DotProduct(dl->origin, surf->plane->normal) -
				surf->plane->dist;
		frad -= fabs(fdist);

		/* rad is now the highest intensity on the plane */
		fminlight = DLIGHT_CUTOFF;

		if (frad < fminlight)
		{
			continue;
		}

		fminlight = frad - fminlight;

		for (i = 0; i < 3; i++)
		{
			impact[i] = dl->origin[i] -
						surf->plane->normal[i] * fdist;
		}

		local[0] = DotProduct(impact, surf->lmvecs[0]) +
			surf->lmvecs[0][3] - surf->texturemins[0];
		local[1] = DotProduct(impact, surf->lmvecs[1]) +
			surf->lmvecs[1][3] - surf->texturemins[1];

		plightdest = s_blocklights;

		for (t = 0, ftacc = 0; t < tmax; t++, ftacc += (1 << surf->lmshift))
		{
			td = local[1] - ftacc;

			if (td < 0)
			{
				td = -td;
			}

			td *= surf->lmvlen[1];

			for (s = 0, fsacc = 0; s < smax; s++, fsacc += (1 << surf->lmshift), plightdest += 3)
			{
				sd = Q_ftol(local[0] - fsacc);

				if (sd < 0)
				{
					sd = -sd;
				}

				sd *= surf->lmvlen[0];

				if (sd > td)
				{
					fdist = sd + (td >> 1);
				}
				else
				{
					fdist = td + (sd >> 1);
				}

				if ((fdist < fminlight) && (plightdest < (s_blocklights_max - 3)))
				{
					float diff = frad - fdist;

					plightdest[0] += diff * dl->color[0];
					plightdest[1] += diff * dl->color[1];
					plightdest[2] += diff * dl->color[2];
				}
			}
		}
	}
}

void
R_SetCacheState(msurface_t *surf)
{
	int maps;

	for (maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != 255;
		 maps++)
	{
		surf->cached_light[maps] =
			r_newrefdef.lightstyles[surf->styles[maps]].white;
	}
}

float *s_blocklights = NULL, *s_blocklights_max = NULL;

/*
 * Combine and scale multiple lightmaps into the floating format in blocklights
 */
void
R_BuildLightMap(msurface_t *surf, byte *dest, int stride)
{
	int smax, tmax;
	int r, g, b, a, max;
	int i, j, size;
	byte *lightmap;
	float scale[4];
	int nummaps;
	float *bl;

	if (surf->texinfo->flags &
		(SURF_SKY | SURF_TRANS33 | SURF_TRANS66 | SURF_WARP))
	{
		ri.Sys_Error(ERR_DROP, "R_BuildLightMap called for non-lit surface");
	}

	smax = (surf->extents[0] >> surf->lmshift) + 1;
	tmax = (surf->extents[1] >> surf->lmshift) + 1;
	size = smax * tmax;

	if (!s_blocklights || (s_blocklights + (size * 3) >= s_blocklights_max))
	{
		int new_size = ROUNDUP(size * 3, 1024);

		if (new_size < 4096)
		{
			new_size = 4096;
		}

		if (s_blocklights)
		{
			free(s_blocklights);
		}

		s_blocklights = malloc(new_size * sizeof(float));
		s_blocklights_max = s_blocklights + new_size;

		if (!s_blocklights)
		{
			ri.Sys_Error(ERR_DROP, "Can't alloc s_blocklights");
		}
	}

	/* set to full bright if no light data */
	if (!surf->samples)
	{
		for (i = 0; i < size * 3; i++)
		{
			s_blocklights[i] = 255;
		}

		goto store;
	}

	/* count the # of maps */
	for (nummaps = 0; nummaps < MAXLIGHTMAPS && surf->styles[nummaps] != 255;
		 nummaps++)
	{
	}

	lightmap = surf->samples;

	/* add all the lightmaps */
	if (nummaps == 1)
	{
		int maps;

		for (maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != 255; maps++)
		{
			bl = s_blocklights;

			for (i = 0; i < 3; i++)
			{
				scale[i] = r_modulate->value *
						   r_newrefdef.lightstyles[surf->styles[maps]].rgb[i];
			}

			if ((scale[0] == 1.0F) &&
				(scale[1] == 1.0F) &&
				(scale[2] == 1.0F))
			{
				for (i = 0; i < size; i++, bl += 3)
				{
					bl[0] = lightmap[i * 3 + 0];
					bl[1] = lightmap[i * 3 + 1];
					bl[2] = lightmap[i * 3 + 2];
				}
			}
			else
			{
				for (i = 0; i < size; i++, bl += 3)
				{
					bl[0] = lightmap[i * 3 + 0] * scale[0];
					bl[1] = lightmap[i * 3 + 1] * scale[1];
					bl[2] = lightmap[i * 3 + 2] * scale[2];
				}
			}

			lightmap += size * 3; /* skip to next lightmap */
		}
	}
	else
	{
		int maps;

		memset(s_blocklights, 0, sizeof(s_blocklights[0]) * size * 3);

		for (maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != 255; maps++)
		{
			bl = s_blocklights;

			for (i = 0; i < 3; i++)
			{
				scale[i] = r_modulate->value *
						   r_newrefdef.lightstyles[surf->styles[maps]].rgb[i];
			}

			if ((scale[0] == 1.0F) &&
				(scale[1] == 1.0F) &&
				(scale[2] == 1.0F))
			{
				for (i = 0; i < size; i++, bl += 3)
				{
					bl[0] += lightmap[i * 3 + 0];
					bl[1] += lightmap[i * 3 + 1];
					bl[2] += lightmap[i * 3 + 2];
				}
			}
			else
			{
				for (i = 0; i < size; i++, bl += 3)
				{
					bl[0] += lightmap[i * 3 + 0] * scale[0];
					bl[1] += lightmap[i * 3 + 1] * scale[1];
					bl[2] += lightmap[i * 3 + 2] * scale[2];
				}
			}

			lightmap += size * 3; /* skip to next lightmap */
		}
	}

	/* add all the dynamic lights */
	if (surf->dlightframe == r_framecount)
	{
		R_AddDynamicLights(surf);
	}

store:
	/* put into texture format */
	stride -= (smax << 2);
	bl = s_blocklights;

	for (i = 0; i < tmax; i++, dest += stride)
	{
		for (j = 0; j < smax; j++)
		{
			r = Q_ftol(bl[0]);
			g = Q_ftol(bl[1]);
			b = Q_ftol(bl[2]);

			/* catch negative lights */
			if (r < 0)
			{
				r = 0;
			}

			if (g < 0)
			{
				g = 0;
			}

			if (b < 0)
			{
				b = 0;
			}

			/* determine the brightest of the three color components */
			if (r > g)
			{
				max = r;
			}
			else
			{
				max = g;
			}

			if (b > max)
			{
				max = b;
			}

			/* alpha is ONLY used for the mono lightmap case. For this
			   reason we set it to the brightest of the color components
			   so that things don't get too dim. */
			a = max;

			/* rescale all the color components if the
			   intensity of the greatest channel exceeds
			   1.0 */
			if (max > 255)
			{
				float t = 255.0F / max;

				r = r * t;
				g = g * t;
				b = b * t;
				a = a * t;
			}

			dest[0] = r;
			dest[1] = g;
			dest[2] = b;
			dest[3] = a;

			bl += 3;
			dest += 4;
		}
	}
}
