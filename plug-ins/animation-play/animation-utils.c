/*
 * Animation Playback plug-in version 0.99.1
 *
 * (c) Jehan : 2012-2015 : jehan at girinstud.io
 *
 * GIMP - The GNU Image Manipulation Program
 * Copyright (C) 1995 Spencer Kimball and Peter Mattis
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <string.h>

#include <libgimp/gimp.h>
#undef GDK_DISABLE_DEPRECATED
#include <libgimp/gimpui.h>

#include "libgimp/stdplugins-intl.h"

#include "animation-utils.h"

/* total_alpha_preview:
 * Fill the @drawing_data with an alpha (grey chess) pattern.
 * This uses a static array, copied over each line (with some shift to
 * reproduce the pattern), using `memcpy()`.
 * The reason why we keep the pattern in the statically allocated memory,
 * instead of simply looping through @drawing_data and recreating the
 * pattern is simply because `memcpy()` implementations are supposed to
 * be more efficient than loops over an array. */
void
total_alpha_preview (guchar *drawing_data,
                     guint   drawing_width,
                     guint   drawing_height)
{
  static guint   alpha_line_width = 0;
  static guchar *alpha_line       = NULL;
  gint           i;

  g_assert (drawing_width > 0);

  /* If width change, we update the "alpha" line. */
  if (alpha_line_width < drawing_width + 8)
    {
      alpha_line_width = drawing_width + 8;

      g_free (alpha_line);

      /* A full line + 8 pixels (1 square). */
      alpha_line = g_malloc (alpha_line_width * 3);

      for (i = 0; i < alpha_line_width; i++)
        {
          /* 8 pixels dark grey, 8 pixels light grey, and so on. */
          if (i & 8)
            {
              alpha_line[i * 3 + 0] =
                alpha_line[i * 3 + 1] =
                alpha_line[i * 3 + 2] = 102;
            }
          else
            {
              alpha_line[i * 3 + 0] =
                alpha_line[i * 3 + 1] =
                alpha_line[i * 3 + 2] = 154;
            }
        }
    }

  for (i = 0; i < drawing_height; i++)
    {
      if (i & 8)
        {
          memcpy (&drawing_data[i * 3 * drawing_width],
                  alpha_line,
                  3 * drawing_width);
        }
      else
        {
          /* Every 8 vertical pixels, we shift the horizontal line by 8 pixels. */
          memcpy (&drawing_data[i * 3 * drawing_width],
                  alpha_line + 24,
                  3 * drawing_width);
        }
    }
}

/**
 * normal_blend:
 * @width: width of the returned #GeglBuffer.
 * @height: height of the returned #GeglBuffer.
 * @backdrop_buffer: optional backdrop image (may be %NULL).
 * @backdrop_scale_ratio: scale ratio (`]0.0, 1.0]`) for @backdrop_buffer.
 * @backdrop_offset_x: original X offset of the backdrop (not processed
 * with @backdrop_scale_ratio yet).
 * @backdrop_offset_y: original Y offset of the backdrop (not processed
 * with @backdrop_scale_ratio yet).
 * @source_buffer: source image (cannot be %NULL).
 * @source_scale_ratio: scale ratio (`]0.0, 1.0]`) for @source_buffer.
 * @source_offset_x: original X offset of the source (not processed with
 * @source_scale_ratio yet).
 * @source_offset_y: original Y offset of the source (not processed with
 * @source_scale_ratio yet).
 *
 * Creates a new #GeglBuffer of size @widthx@height with @source_buffer
 * scaled with @source_scale_ratio r, and translated by offsets:
 * (@source_offset_x * @source_scale_ratio,
 *  @source_offset_y * @source_scale_ratio).
 *
 * If @backdrop_buffer is not %NULL, it is resized with
 * @backdrop_scale_ratio, and offsetted by:
 * (@backdrop_offset_x * @backdrop_scale_ratio,
 *  @backdrop_offset_y * @backdrop_scale_ratio)
 *
 * Finally @source_buffer is composited over @backdrop_buffer in normal
 * blend mode.
 *
 * Returns: the newly allocated #GeglBuffer containing the result of
 * said scaling, translation and blending.
 */
GeglBuffer *
normal_blend (gint        width,
              gint        height,
              GeglBuffer *backdrop_buffer,
              gdouble     backdrop_scale_ratio,
              gint        backdrop_offset_x,
              gint        backdrop_offset_y,
              GeglBuffer *source_buffer,
              gdouble     source_scale_ratio,
              gint        source_offset_x,
              gint        source_offset_y)
{
  GeglBuffer *buffer;
  GeglNode   *graph;
  GeglNode   *source, *src_scale, *src_translate;
  GeglNode   *backdrop, *bd_scale, *bd_translate;
  GeglNode   *blend, *target;
  gdouble     offx;
  gdouble     offy;

  g_return_val_if_fail (source_scale_ratio >  0.0 &&
                        source_scale_ratio <= 1.0 &&
                        source_buffer             &&
                        (! backdrop_buffer ||
                         (backdrop_scale_ratio >= 0.1 &&
                          backdrop_scale_ratio <= 1.0)),
                        NULL);

  /* Panel image. */
  buffer = gegl_buffer_new (GEGL_RECTANGLE (0, 0, width, height),
                            gegl_buffer_get_format (source_buffer));
  graph  = gegl_node_new ();

  /* Source */
  source = gegl_node_new_child (graph,
                                "operation", "gegl:buffer-source",
                                "buffer", source_buffer,
                                NULL);
  src_scale  = gegl_node_new_child (graph,
                                    "operation", "gegl:scale-ratio",
                                    "sampler", GEGL_SAMPLER_NEAREST,
                                    "x", source_scale_ratio,
                                    "y", source_scale_ratio,
                                    NULL);

  offx = source_offset_x * source_scale_ratio;
  offy = source_offset_y * source_scale_ratio;
  src_translate =  gegl_node_new_child (graph,
                                        "operation", "gegl:translate",
                                        "x", offx,
                                        "y", offy,
                                        NULL);

  /* Target */
  target = gegl_node_new_child (graph,
                                "operation", "gegl:write-buffer",
                                "buffer", buffer,
                                NULL);

  if (backdrop_buffer)
    {
      /* Backdrop */
      backdrop = gegl_node_new_child (graph,
                                      "operation", "gegl:buffer-source",
                                      "buffer", backdrop_buffer,
                                      NULL);
      bd_scale  = gegl_node_new_child (graph,
                                       "operation", "gegl:scale-ratio",
                                       "sampler", GEGL_SAMPLER_NEAREST,
                                       "x", backdrop_scale_ratio,
                                       "y", backdrop_scale_ratio,
                                       NULL);

      offx = backdrop_offset_x * backdrop_scale_ratio;
      offy = backdrop_offset_y * backdrop_scale_ratio;
      bd_translate =  gegl_node_new_child (graph,
                                           "operation", "gegl:translate",
                                           "x", offx,
                                           "y", offy,
                                           NULL);

      gegl_node_link_many (source, src_scale, src_translate, NULL);
      gegl_node_link_many (backdrop, bd_scale, bd_translate, NULL);

      /* Blending */
      blend =  gegl_node_new_child (graph,
                                    "operation", "gegl:over",
                                    NULL);

      gegl_node_link_many (bd_translate, blend, target, NULL);
      gegl_node_connect_to (src_translate, "output",
                            blend, "aux");
    }
  else
    {
      gegl_node_link_many (source, src_scale, src_translate, target, NULL);
    }

  gegl_node_process (target);
  g_object_unref (graph);

  return buffer;
}
